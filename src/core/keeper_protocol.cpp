#include "core/keeper_protocol.h"

#include <algorithm>
#include <charconv>

namespace sfv::keeper {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kAnswerMagic = "SFVK1";

bool Number(std::string_view text, int64_t* out)
{
    if (text.empty() || text.size() > 10) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool Int(std::string_view text, int* out)
{
    int64_t n = 0;
    if (!Number(text, &n) || n < INT32_MIN || n > INT32_MAX) return false;
    *out = static_cast<int>(n);
    return true;
}

std::vector<std::string_view> Split(std::string_view text, char sep)
{
    std::vector<std::string_view> parts;
    size_t from = 0;
    while (true) {
        const size_t at = text.find(sep, from);
        parts.push_back(text.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
        if (at == std::string_view::npos) break;
        from = at + 1;
    }
    return parts;
}

// Names and codes in the list: no tabs, newlines or commas (the list's
// separators), nothing below space.
std::string Clean(std::string_view text)
{
    std::string s;
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == ',') continue;
        s += ch;
    }
    return s;
}

std::string EncodeEntry(const ListedRoom& r)
{
    std::string faces;
    for (size_t i = 0; i < r.faces.size() && i < 6; i++) {
        if (i) faces += ',';
        faces += Clean(r.faces[i]);
    }
    return Clean(r.code) + '\t' + std::to_string(r.preset) + '\t' + std::to_string(r.members) + '\t' +
           std::to_string(r.tables) + '\t' + std::to_string(r.openSeats) + '\t' + Clean(r.rules) + '\t' + faces + '\t' +
           Clean(r.owner);
}

bool DecodeEntry(std::string_view text, ListedRoom* out)
{
    const auto f = Split(text, '\t');
    if (f.size() != 8 || f[0].empty()) return false;
    ListedRoom r;
    r.code = std::string(f[0]);
    if (!Int(f[1], &r.preset) || !Int(f[2], &r.members) || !Int(f[3], &r.tables) || !Int(f[4], &r.openSeats)) return false;
    r.rules = std::string(f[5]);
    if (!f[6].empty()) {
        for (std::string_view face : Split(f[6], ',')) r.faces.emplace_back(face);
    }
    r.owner = std::string(f[7]);
    *out = std::move(r);
    return true;
}

} // namespace

std::string TableKey(int table) { return "t" + std::to_string(table); }
std::string ListKey(int index) { return "pr" + std::to_string(index); }

std::string ProofMessage(std::string_view room, uint32_t member)
{
    return "sfvr-keeper 1 " + std::string(room) + " " + std::to_string(member);
}

std::string Base64Encode(const uint8_t* data, size_t size)
{
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return out;
}

bool Base64Decode(std::string_view text, std::vector<uint8_t>* out)
{
    out->clear();
    uint32_t n = 0;
    int bits = 0;
    size_t pad = 0;
    for (char ch : text) {
        if (ch == '=') {
            pad++;
            continue;
        }
        if (pad) return false; // data after padding
        const char* at = std::char_traits<char>::find(kAlphabet, 64, ch);
        if (!at) return false;
        n = (n << 6) | uint32_t(at - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((n >> bits) & 0xFF));
        }
    }
    return pad <= 2 && (text.size() % 4 == 0);
}

const char* PresetName(int preset)
{
    static const char* const kNames[kPresetCount] = {
        "Casual sets",        "Long sets",           "Beginners welcome", "Learning a character",
        "Ranked warmup",      "Tournament practice", "Mirror matches",    "Quick rotation",
        "Anything goes",      "Chill and chat",      "Late night",        "Friends only"};
    return preset >= 0 && preset < kPresetCount ? kNames[preset] : "Room";
}

std::string FormatRequest(const Request& request)
{
    std::string s = std::to_string(request.seq) + ":" + request.kind;
    for (const std::string& a : request.args) s += ":" + a;
    return s;
}

bool ParseRequest(std::string_view text, Request* out)
{
    const auto parts = Split(text, ':');
    if (parts.size() < 2 || parts[1].empty()) return false;
    int64_t seq = 0;
    if (!Number(parts[0], &seq) || seq <= 0 || seq > UINT32_MAX) return false;
    Request r;
    r.seq = static_cast<uint32_t>(seq);
    r.kind = std::string(parts[1]);
    for (size_t i = 2; i < parts.size(); i++) r.args.emplace_back(parts[i]);
    *out = std::move(r);
    return true;
}

Request RoomRequest(uint32_t seq, const RoomSettings& settings)
{
    return {seq, "room", {std::to_string(settings.tables), settings.isPublic ? "1" : "0", std::to_string(settings.preset)}};
}

bool ParseRoomSettings(const Request& request, RoomSettings* out)
{
    if (request.kind != "room" || request.args.size() != 3) return false;
    RoomSettings s;
    int pub = 0;
    if (!Int(request.args[0], &s.tables) || !Int(request.args[1], &pub) || !Int(request.args[2], &s.preset)) return false;
    if (s.tables < 1 || s.tables > kMaxTables || (pub != 0 && pub != 1) || s.preset < 0 || s.preset >= kPresetCount) return false;
    s.isPublic = pub == 1;
    *out = s;
    return true;
}

std::string FormatAnswer(const Answer& answer)
{
    std::string text;
    for (char ch : answer.text) text += ch == '\n' ? ' ' : ch;
    return std::string(kAnswerMagic) + "\n" + std::to_string(answer.seq) + "\n" + answer.kind + "\n" +
           (answer.ok ? "ok" : "no") + "\n" + text;
}

bool ParseAnswer(const uint8_t* data, size_t size, Answer* out)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (text.substr(0, kAnswerMagic.size()) != kAnswerMagic) return false;
    const auto f = Split(text, '\n');
    if (f.size() != 5 || f[0] != kAnswerMagic || (f[3] != "ok" && f[3] != "no")) return false;
    int64_t seq = 0;
    if (!Number(f[1], &seq) || seq <= 0 || seq > UINT32_MAX) return false;
    out->seq = static_cast<uint32_t>(seq);
    out->kind = std::string(f[2]);
    out->ok = f[3] == "ok";
    out->text = std::string(f[4]);
    return true;
}

void SortRoomList(std::vector<ListedRoom>* rooms)
{
    // 0: open seats, 1: room to queue, 2: full.
    auto group = [](const ListedRoom& r) { return r.members >= kRoomCap ? 2 : r.openSeats > 0 ? 0 : 1; };
    std::stable_sort(rooms->begin(), rooms->end(), [&](const ListedRoom& a, const ListedRoom& b) {
        if (group(a) != group(b)) return group(a) < group(b);
        return a.members > b.members;
    });
}

std::vector<std::string> PackRoomList(const std::vector<ListedRoom>& rooms)
{
    std::vector<std::string> values;
    std::string current;
    for (const ListedRoom& r : rooms) {
        const std::string entry = EncodeEntry(r);
        if (entry.size() > kMaxValue) continue; // cannot happen with names under 96 bytes
        if (!current.empty() && current.size() + 1 + entry.size() > kMaxValue) {
            values.push_back(std::move(current));
            current.clear();
            if (values.size() == kListKeys) return values;
        }
        if (!current.empty()) current += '\n';
        current += entry;
    }
    if (!current.empty()) values.push_back(std::move(current));
    return values;
}

std::vector<ListedRoom> UnpackRoomList(const std::map<std::string, std::string>& data)
{
    std::vector<ListedRoom> rooms;
    for (int i = 0; i < kListKeys; i++) {
        const auto it = data.find(ListKey(i));
        if (it == data.end()) continue;
        for (std::string_view line : Split(it->second, '\n')) {
            ListedRoom r;
            if (DecodeEntry(line, &r)) rooms.push_back(std::move(r));
        }
    }
    return rooms;
}

size_t Utf8Length(std::string_view text)
{
    size_t n = 0;
    for (char ch : text) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
    return n;
}

bool ChatLimiter::Allow(int64_t nowMs, std::string_view text)
{
    if (lastMs_ >= 0 && nowMs > lastMs_) tokens_ = std::min<double>(kBurst, tokens_ + double(nowMs - lastMs_) / kRefillMs);
    lastMs_ = std::max(lastMs_, nowMs);
    const bool ok = !text.empty() && Utf8Length(text) <= kChatMaxChars && tokens_ >= 1.0;
    if (ok) tokens_ -= 1.0;
    else strikes_.push_back(nowMs);
    std::erase_if(strikes_, [&](int64_t t) { return nowMs - t >= kStrikeWindowMs; });
    return ok;
}

int ChatLimiter::Strikes(int64_t nowMs) const
{
    return static_cast<int>(std::count_if(strikes_.begin(), strikes_.end(),
                                          [&](int64_t t) { return nowMs - t < kStrikeWindowMs; }));
}

Request PeekRequest(uint32_t seq, std::string_view code) { return {seq, "peek", {std::string(code)}}; }

std::vector<std::string> PackRoomDetail(const RoomDetail& detail, size_t partBytes)
{
    // Control bytes never travel inside a field (they separate them).
    auto clean = [](std::string_view s) {
        std::string out;
        for (char ch : s) {
            if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
        }
        return out;
    };
    std::vector<std::string> records;
    for (const std::string& t : detail.tables) records.push_back("t" + clean(t));
    for (const RoomDetail::Person& p : detail.people) records.push_back("p" + clean(p.chara) + "\x1F" + clean(p.name));
    std::vector<std::string> bodies;
    std::string body;
    for (const std::string& r : records) {
        if (!body.empty() && body.size() + 1 + r.size() > partBytes) {
            bodies.push_back(std::move(body));
            body.clear();
        }
        if (!body.empty()) body += '\x1E';
        body += r;
    }
    bodies.push_back(std::move(body)); // an empty room still answers once
    std::vector<std::string> parts;
    for (size_t i = 0; i < bodies.size(); i++)
        parts.push_back(std::to_string(i + 1) + "/" + std::to_string(bodies.size()) + "\x1D" + bodies[i]);
    return parts;
}

bool ReadDetailPart(std::string_view text, int* part, int* parts, std::string* records)
{
    const size_t gs = text.find('\x1D'), slash = text.find('/');
    if (gs == std::string_view::npos || slash == std::string_view::npos || slash > gs) return false;
    if (!Int(text.substr(0, slash), part) || !Int(text.substr(slash + 1, gs - slash - 1), parts)) return false;
    if (*parts < 1 || *parts > 64 || *part < 1 || *part > *parts) return false;
    *records = std::string(text.substr(gs + 1));
    return true;
}

bool JoinRoomDetail(const std::vector<std::string>& records, RoomDetail* out)
{
    RoomDetail d;
    for (const std::string& body : records) {
        if (body.empty()) continue;
        for (std::string_view r : Split(body, '\x1E')) {
            if (r.empty()) return false;
            if (r[0] == 't') d.tables.emplace_back(r.substr(1));
            else if (r[0] == 'p') {
                const size_t us = r.find('\x1F');
                if (us == std::string_view::npos) return false;
                d.people.push_back({std::string(r.substr(us + 1)), std::string(r.substr(1, us - 1))});
            }
            else return false;
        }
    }
    d.code = out->code;
    *out = std::move(d);
    return true;
}

std::string FoldName(std::string_view name)
{
    std::string out;
    for (char ch : name) {
        char c = ch;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        switch (c) {
        case '0': c = 'o'; break;
        case '1': c = 'i'; break;
        case '3': c = 'e'; break;
        case '4': c = 'a'; break;
        case '5': c = 's'; break;
        case '7': c = 't'; break;
        case '@': c = 'a'; break;
        case '$': c = 's'; break;
        default: break;
        }
        if (c >= 'a' && c <= 'z') out += c;
    }
    return out;
}

bool NameRejected(std::string_view name, const std::vector<std::string>& words)
{
    const std::string folded = FoldName(name);
    for (const std::string& w : words) {
        if (!w.empty() && folded.find(w) != std::string::npos) return true;
    }
    return false;
}

std::string FormatMask(const std::map<uint32_t, int>& numbers)
{
    std::string s;
    for (const auto& [id, n] : numbers) {
        if (!s.empty()) s += ',';
        s += std::to_string(id) + "=" + std::to_string(n);
    }
    return s;
}

std::map<uint32_t, int> ParseMask(std::string_view text)
{
    std::map<uint32_t, int> numbers;
    if (text.empty()) return numbers;
    for (std::string_view part : Split(text, ',')) {
        const size_t eq = part.find('=');
        int64_t id = 0;
        int n = 0;
        if (eq == std::string_view::npos || !Number(part.substr(0, eq), &id) || !Int(part.substr(eq + 1), &n)) continue;
        if (id > 0 && id <= UINT32_MAX && n > 0 && n <= kRoomCap * 4) numbers[static_cast<uint32_t>(id)] = n;
    }
    return numbers;
}

void FormatPing(bool pong, uint64_t clockUs, uint8_t (&out)[kPingSize])
{
    out[0] = 'S';
    out[1] = 'F';
    out[2] = 'V';
    out[3] = 'P';
    out[4] = pong ? 2 : 1;
    for (int i = 0; i < 8; i++) out[5 + i] = static_cast<uint8_t>(clockUs >> (8 * i));
}

bool ParsePing(const uint8_t* data, size_t size, bool* pong, uint64_t* clockUs)
{
    if (size != kPingSize || data[0] != 'S' || data[1] != 'F' || data[2] != 'V' || data[3] != 'P' || (data[4] != 1 && data[4] != 2))
        return false;
    *pong = data[4] == 2;
    *clockUs = 0;
    for (int i = 0; i < 8; i++) *clockUs |= uint64_t(data[5 + i]) << (8 * i);
    return true;
}

uint32_t NextOwner(const std::vector<uint32_t>& joinOrder, uint32_t leaving, uint32_t keeper)
{
    for (uint32_t id : joinOrder) {
        if (id != leaving && id != keeper && id != 0) return id;
    }
    return 0;
}

} // namespace sfv::keeper
