#include "mod/entitlements.h"

#include "builtin_steam_grants.h"
#include "mod/image.h"
#include "mod/log.h"
#include "mod/ue4.h"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

// The game's save path (rvas of the final Steam build):
//   0xE138F0  a save slot's loader (virtual; `this` = slot, +0x41 = loaded).
//             It reads the file and decodes it, or, when there is none,
//             builds a default profile and writes it.
//   0xE18D10  decodes a file's bytes (TArray<uint8>) in place, after the read.
//   0x113C220 FMemory::Realloc(ptr, size, alignment).
constexpr uintptr_t kLoaderRva = 0xE138F0;
constexpr uintptr_t kDecodeRva = 0xE18D10;
constexpr uintptr_t kReallocRva = 0x113C220;
const uint8_t kLoaderHead[] = {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x48, 0x8D, 0x68, 0xA1};
const uint8_t kDecodeHead[] = {0x40, 0x53, 0x55, 0x57, 0x41, 0x56};
const uint8_t kReallocHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
// EntitlementVersion of the final game data: a profile saved with another
// (or none) has its list replaced by the free content when it loads.
constexpr int32_t kEntitlementVersion = 9;

struct ByteArray {
    uint8_t* data;
    int32_t num;
    int32_t max;
};
using Codec_t = void(__fastcall*)(ByteArray*);
using Loader_t = void(__fastcall*)(void*, uint8_t);
using Realloc_t = void*(__fastcall*)(void*, size_t, uint32_t);

uintptr_t g_base = 0;
Codec_t g_origDecode = nullptr;
Loader_t g_origLoader = nullptr;
Realloc_t g_realloc = nullptr;
std::atomic<int> g_decodes{0};
bool g_freshProfile = false;   // the profile being loaded is the default one the game just wrote

std::map<uint32_t, std::vector<std::string>> g_grants;   // Steam app -> codes it grants
// Granted to every profile: content only a CFN feature unlocked, which no
// one can get any more. The Dojo stage (StageSpec 50, DOJ) came with joining
// a clan on CFN.
const char* const kAlwaysGranted[] = {"VeUuEoC0"};
bool g_merge = true;   // [mod] steam_entitlements

// What the profile owns as it loaded (OwnsEntitlement): its list after the merge.
std::mutex g_ownedMutex;
bool g_ownedKnown = false;
std::set<std::string> g_owned;

void ParseGrants()
{
    std::istringstream in(std::string(reinterpret_cast<const char*>(kSteamGrants), sizeof(kSteamGrants)));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        uint32_t app = 0;
        if (!(words >> app)) continue;
        std::string code;
        while (words >> code) g_grants[app].push_back(code);
    }
}

// ISteamApps::BIsSubscribedApp (vtable slot 6) for every app in the table:
// the Steam client answers from its license cache, no server involved.
bool OwnedApps(std::vector<uint32_t>* owned)
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    using Getter = void* (*)();
    const auto getter = steam ? reinterpret_cast<Getter>(GetProcAddress(steam, "SteamApps")) : nullptr;
    void* apps = getter ? getter() : nullptr;
    if (!apps) return false;
    void* const* vt = *reinterpret_cast<void* const* const*>(apps);
    using BoolApp = bool (*)(void*, uint32_t);
    for (const auto& [app, codes] : g_grants) {
        if (reinterpret_cast<BoolApp>(vt[6])(apps, app)) owned->push_back(app);
    }
    return true;
}

std::string FStringUtf8(const uint8_t* p)
{
    const auto* chars = *reinterpret_cast<const wchar_t* const*>(p);
    const int n = *reinterpret_cast<const int32_t*>(p + 8);
    if (!chars || n <= 1 || n > 256 || !IsReadable(chars, static_cast<size_t>(n) * 2)) return {};
    std::string out;
    for (int i = 0; i < n - 1; i++) out.push_back(chars[i] < 0x80 ? static_cast<char>(chars[i]) : '?');
    return out;
}

// The free content: EntitlementSpec rows (DA_Spec) with DefaultValue set.
// UDataTable: RowStruct +0x28, row map elements (0x18: FName, row) +0x40,
// count +0x48; properties: first child +0x38, next +0x28, offset +0x4C.
bool FreeCodes(std::vector<std::string>* out)
{
    if (!ue4::EnsureObjects(g_base)) return false;
    const auto* table = static_cast<const uint8_t*>(ue4::FindObject("EntitlementSpec", "DataTable"));
    if (!table || !IsReadable(table + 0x28, 0x28)) return false;
    const auto* rowStruct = *reinterpret_cast<const uint8_t* const*>(table + 0x28);
    const auto* elements = *reinterpret_cast<const uint8_t* const*>(table + 0x40);
    const int count = *reinterpret_cast<const int32_t*>(table + 0x48);
    int refCode = -1, defaultValue = -1;
    const uint8_t* f = rowStruct ? *reinterpret_cast<const uint8_t* const*>(rowStruct + 0x38) : nullptr;
    for (int k = 0; f && k < 60 && IsReadable(f, 0x50); k++) {
        const std::string name = ue4::ObjectName(f);
        if (name == "RefCode") refCode = *reinterpret_cast<const int32_t*>(f + 0x4C);
        if (name == "DefaultValue") defaultValue = *reinterpret_cast<const int32_t*>(f + 0x4C);
        f = *reinterpret_cast<const uint8_t* const*>(f + 0x28);
    }
    if (refCode < 0 || defaultValue < 0 || !elements || count <= 0) return false;
    for (int i = 0; i < count; i++) {
        const auto* row = *reinterpret_cast<const uint8_t* const*>(elements + i * 0x18 + 8);
        if (!row || !IsReadable(row, static_cast<size_t>(std::max(refCode + 16, defaultValue + 1)))) continue;
        if (!row[defaultValue]) continue;
        std::string code = FStringUtf8(row + refCode);
        if (!code.empty()) out->push_back(std::move(code));
    }
    return !out->empty();
}

// The plaintext of a profile save: "GPRG", int32, then sections (4cc, int32
// version) of tagged properties, each list closed by "None" and an int32.
// A tag: FString name, FString type, int32 size, int32 index, then by type
// StructProperty: FString struct name; BoolProperty: the value byte;
// ByteProperty: FString enum; ArrayProperty: FString inner type; MapProperty:
// two FStrings; then `size` bytes of value. An ObjectProperty's value is the
// object's own tag list. An array of structs is an int32 count, then each
// element's tag list. FStrings here are ANSI with the NUL counted.
class Plain {
public:
    explicit Plain(ByteArray* a) : a_(a) {}

    int32_t Int(int at) const
    {
        int32_t v;
        std::memcpy(&v, a_->data + at, 4);
        return v;
    }
    void SetInt(int at, int32_t v) { std::memcpy(a_->data + at, &v, 4); }

    bool Str(int at, std::string* s, int* next) const
    {
        if (at < 0 || at + 4 > a_->num) return false;
        const int32_t n = Int(at);
        if (n < 0 || n > 1024 || at + 4 + n > a_->num) return false;
        if (s) s->assign(reinterpret_cast<const char*>(a_->data + at + 4), n > 0 ? n - 1 : 0);
        *next = at + 4 + n;
        return true;
    }

    struct Tag {
        int at = -1;        // the tag's start
        std::string name, type, extra;
        int sizeAt = 0;     // the int32 size
        int value = 0;      // the value's start
        int end = 0;        // after the value
    };

    // The tag at `at`; false at the closing "None" (next = after it) or on bad data.
    bool ReadTag(int at, Tag* t, int* next) const
    {
        int p;
        t->at = at;
        if (!Str(at, &t->name, &p)) return false;
        if (t->name == "None") {
            *next = p;
            return false;
        }
        if (!Str(p, &t->type, &p) || p + 8 > a_->num) return false;
        t->sizeAt = p;
        const int32_t size = Int(p);
        p += 8;
        t->extra.clear();
        if (t->type == "StructProperty" || t->type == "ByteProperty" || t->type == "ArrayProperty") {
            if (!Str(p, &t->extra, &p)) return false;
        }
        else if (t->type == "MapProperty") {
            std::string v;
            if (!Str(p, &t->extra, &p) || !Str(p, &v, &p)) return false;
        }
        else if (t->type == "BoolProperty") {
            p += 1;
        }
        if (size < 0 || p + size > a_->num) return false;
        t->value = p;
        t->end = p + size;
        *next = t->end;
        return true;
    }

    bool Insert(int at, const std::string& bytes)
    {
        const int grow = static_cast<int>(bytes.size());
        if (a_->max < a_->num + grow) {
            auto* d = static_cast<uint8_t*>(g_realloc(a_->data, static_cast<size_t>(a_->num + grow), 0));
            if (!d) return false;
            a_->data = d;
            a_->max = a_->num + grow;
        }
        std::memmove(a_->data + at + grow, a_->data + at, static_cast<size_t>(a_->num - at));
        std::memcpy(a_->data + at, bytes.data(), bytes.size());
        a_->num += grow;
        return true;
    }

    int Num() const { return a_->num; }
    const uint8_t* Data() const { return a_->data; }

private:
    ByteArray* a_;
};

void PutStr(std::string* out, const std::string& s)
{
    const int32_t n = static_cast<int32_t>(s.size() + 1);
    out->append(reinterpret_cast<const char*>(&n), 4);
    out->append(s);
    out->push_back('\0');
}

void PutInt(std::string* out, int32_t v) { out->append(reinterpret_cast<const char*>(&v), 4); }

// An FString as UE4 writes one: ANSI (length counting the NUL) when every
// character is ASCII, else UTF-16 with the length negated.
std::string FStringBytes(const std::string& utf8)
{
    std::string out;
    bool ascii = true;
    for (unsigned char c : utf8) ascii = ascii && c < 0x80;
    if (ascii) {
        PutStr(&out, utf8);
        return out;
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    PutInt(&out, -(n + 1));
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * 2);
    out.append(2, '\0');
    return out;
}

// The Steam persona name (ISteamFriends::GetPersonaName, slot 0), "" without Steam.
std::string SteamName()
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    using Getter = void* (*)();
    const auto friends = steam ? reinterpret_cast<Getter>(GetProcAddress(steam, "SteamFriends")) : nullptr;
    void* iface = friends ? friends() : nullptr;
    if (!iface) return "";
    using GetPersonaName = const char* (*)(void*);
    const char* name = (*reinterpret_cast<GetPersonaName**>(iface))[0](iface);
    return name ? name : "";
}

// A fresh profile has no name (CFN gave it the fighter ID): the Steam name
// goes in as MainUserProfileDetails.ProfileDataset.FightersId. The default
// profile leaves ProfileDataset out (all its fields at their defaults), so
// the struct goes in with that one field.
void NameFreshProfile(Plain& s)
{
    const std::string name = SteamName();
    if (name.empty()) {
        Log("entitlements: fresh profile: no Steam name to give it");
        return;
    }
    Plain::Tag obj;
    int next = 0;
    if (!s.ReadTag(16, &obj, &next) || obj.name != "MainUserProfileDetails" || obj.type != "ObjectProperty") {
        Log("entitlements: fresh profile: no MainUserProfileDetails; left without a name");
        return;
    }
    Plain::Tag dataset;
    int noneAt = -1;
    for (int at = obj.value; at < obj.end;) {
        Plain::Tag t;
        if (!s.ReadTag(at, &t, &next)) {
            if (next > at && next <= obj.end) noneAt = at;
            break;
        }
        if (t.name == "ProfileDataset" && t.type == "StructProperty") dataset = t;
        at = next;
    }
    if (noneAt < 0) return;
    std::string id;
    PutStr(&id, "FightersId");
    PutStr(&id, "StrProperty");
    const std::string value = FStringBytes(name);
    PutInt(&id, static_cast<int32_t>(value.size()));
    PutInt(&id, 0);
    id += value;
    if (dataset.at >= 0) {
        // It has a ProfileDataset: the name goes first in it, unless it has one.
        for (int at = dataset.value; at < dataset.end;) {
            Plain::Tag t;
            if (!s.ReadTag(at, &t, &next)) break;
            if (t.name == "FightersId") return;
            at = next;
        }
        if (!s.Insert(dataset.value, id)) return;
        s.SetInt(dataset.sizeAt, s.Int(dataset.sizeAt) + static_cast<int32_t>(id.size()));
        s.SetInt(obj.sizeAt, s.Int(obj.sizeAt) + static_cast<int32_t>(id.size()));
    }
    else {
        std::string tag;
        PutStr(&tag, "ProfileDataset");
        PutStr(&tag, "StructProperty");
        PutInt(&tag, static_cast<int32_t>(id.size() + 9));   // its list: the name, then None (4 + 5)
        PutInt(&tag, 0);
        PutStr(&tag, "KWProfileDataset");
        tag += id;
        PutStr(&tag, "None");
        if (!s.Insert(noneAt, tag)) return;
        s.SetInt(obj.sizeAt, s.Int(obj.sizeAt) + static_cast<int32_t>(tag.size()));
    }
    Log("entitlements: fresh profile named %s (the Steam name)", name.c_str());
}

void PutIdElement(std::string* out, const std::string& code)
{
    PutStr(out, "Id");
    PutStr(out, "StrProperty");
    PutInt(out, static_cast<int32_t>(4 + code.size() + 1));
    PutInt(out, 0);
    PutStr(out, code);
    PutStr(out, "None");
}

// The codes this account gets, worked out once (on the first profile load).
bool g_wantReady = false;
std::vector<std::string> g_want;
std::string g_wantSummary;

void WorkOutWant()
{
    if (g_wantReady) return;
    g_wantReady = true;
    std::vector<uint32_t> owned;
    if (!OwnedApps(&owned)) Log("entitlements: no Steam (SteamApps() is null): only the Dojo stage to add");
    std::set<std::string> want(std::begin(kAlwaysGranted), std::end(kAlwaysGranted));
    std::string apps;
    for (uint32_t app : owned) {
        for (const std::string& c : g_grants[app]) want.insert(c);
        apps += " " + std::to_string(app);
    }
    g_want.assign(want.begin(), want.end());
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%zu of %zu Steam apps owned, %zu codes with the Dojo", owned.size(), g_grants.size(),
                  g_want.size());
    g_wantSummary = buf;
    Log("entitlements: %s; apps:%s", buf, apps.empty() ? " none" : apps.c_str());
}

void RecordOwned(std::set<std::string> codes, bool known)
{
    std::lock_guard lock(g_ownedMutex);
    g_owned = std::move(codes);
    g_ownedKnown = known;
    Log("entitlements: the mod's screens offer what the profile owns (%zu codes)%s", g_owned.size(),
        known ? "" : ": not known, everything open");
}

void MergeEntitlements(ByteArray* a)
{
    if (!a || a->num < 16 || std::memcmp(a->data, "GPRG", 4) != 0) return;
    if (g_merge) WorkOutWant();
    Plain s(a);
    if (g_freshProfile) {
        g_freshProfile = false;
        NameFreshProfile(s);
    }
    // The profile's EntitlementsData: the first tag of that name (top level of UPDS).
    int p = -1;
    for (int at = 16; at + 21 <= s.Num(); at++) {
        if (s.Int(at) == 17 && std::memcmp(s.Data() + at + 4, "EntitlementsData", 17) == 0) {
            p = at;
            break;
        }
    }
    Plain::Tag obj;
    int next = 0;
    if (p < 0 || !s.ReadTag(p, &obj, &next) || obj.type != "ObjectProperty") {
        Log("entitlements: no EntitlementsData in the profile save; left as it is");
        return;
    }
    // Its tags: EntitlementVersion, AuthorizedEntitlementIds, then None.
    Plain::Tag version, ids;
    int noneAt = -1;
    for (int at = obj.value; at < obj.end;) {
        Plain::Tag t;
        if (!s.ReadTag(at, &t, &next)) {
            if (next > at && next <= obj.end) noneAt = at;
            break;
        }
        if (t.name == "EntitlementVersion" && t.type == "IntProperty") version = t;
        if (t.name == "AuthorizedEntitlementIds" && t.type == "ArrayProperty" && t.extra == "StructProperty") ids = t;
        at = next;
    }
    if (noneAt < 0) {
        Log("entitlements: EntitlementsData does not parse; left as it is");
        return;
    }
    // The codes it has.
    std::set<std::string> have;
    int count = 0;
    if (ids.at >= 0) {
        count = s.Int(ids.value);
        int at = ids.value + 4;
        for (int i = 0; i < count && at < ids.end; i++) {
            Plain::Tag t;
            while (s.ReadTag(at, &t, &next)) {
                if (t.name == "Id" && t.type == "StrProperty") {
                    std::string code;
                    int q;
                    if (s.Str(t.value, &code, &q)) have.insert(code);
                }
                at = next;
            }
            at = next;   // after the element's None
        }
    }
    const int32_t savedVersion = version.at >= 0 ? s.Int(version.value) : 0;
    if (!g_merge) {
        // The game keeps this list only at the current version (else it
        // puts the free content alone in its place).
        std::vector<std::string> free;
        if (savedVersion == kEntitlementVersion) RecordOwned(have, true);
        else if (FreeCodes(&free)) RecordOwned(std::set<std::string>(free.begin(), free.end()), true);
        else RecordOwned({}, false);
        return;
    }
    // A list of another version (or none) would be replaced by the free
    // content: the free content goes in with the current version instead.
    std::vector<std::string> want = g_want;
    bool setVersion = false;
    size_t freeCount = 0;
    if (savedVersion != kEntitlementVersion) {
        std::vector<std::string> free;
        if (FreeCodes(&free)) {
            freeCount = free.size();
            want.insert(want.end(), free.begin(), free.end());
            setVersion = true;
        }
        else {
            Log("entitlements: profile list version %d, and the free content is not readable yet: the game resets the "
                "list this time, Steam content comes with the next start",
                savedVersion);
            RecordOwned({}, false);
            return;
        }
    }
    std::string elems;
    std::set<std::string> added;
    for (const std::string& c : want) {
        if (have.count(c) || added.count(c)) continue;
        PutIdElement(&elems, c);
        added.insert(c);
    }
    if (added.empty() && !setVersion) {
        RecordOwned(have, true);
        Log("entitlements: profile has %d codes (version %d), Steam adds none (%s)", count, savedVersion, g_wantSummary.c_str());
        return;
    }
    // Nothing is changed until the codes are in: a failed insert leaves the
    // save as it was (and the list the game will keep, or reset, is unknown).
    auto failed = [&] {
        Log("entitlements: cannot grow the profile save; left as it is");
        if (savedVersion == kEntitlementVersion) RecordOwned(have, true);
        else RecordOwned({}, false);
    };
    int objGrow = 0;
    if (ids.at >= 0) {
        // Elements go at the end of the array; its count and size, and the object's size, grow.
        const int countAt = ids.value;
        const int grow = static_cast<int>(elems.size());
        if (!s.Insert(ids.end, elems)) return failed();
        s.SetInt(countAt, s.Int(countAt) + static_cast<int32_t>(added.size()));
        s.SetInt(ids.sizeAt, s.Int(ids.sizeAt) + grow);
        objGrow += grow;
    }
    else {
        // A new array tag before the object's None (after anything else in it).
        std::string tag;
        PutStr(&tag, "AuthorizedEntitlementIds");
        PutStr(&tag, "ArrayProperty");
        PutInt(&tag, static_cast<int32_t>(4 + elems.size()));
        PutInt(&tag, 0);
        PutStr(&tag, "StructProperty");
        PutInt(&tag, static_cast<int32_t>(added.size()));
        tag += elems;
        if (!s.Insert(noneAt, tag)) return failed();
        objGrow += static_cast<int>(tag.size());
    }
    // The version: the tag comes before the list, so the inserts above (all
    // after it) did not move its value.
    if (setVersion && version.at >= 0) s.SetInt(version.value, kEntitlementVersion);
    bool versionIn = !setVersion || version.at >= 0;
    if (!versionIn) {
        std::string tag;
        PutStr(&tag, "EntitlementVersion");
        PutStr(&tag, "IntProperty");
        PutInt(&tag, 4);
        PutInt(&tag, 0);
        PutInt(&tag, kEntitlementVersion);
        versionIn = s.Insert(obj.value, tag);
        if (versionIn) objGrow += static_cast<int>(tag.size());
    }
    s.SetInt(obj.sizeAt, s.Int(obj.sizeAt) + objGrow);
    if (!versionIn) {
        // The codes are in but the list stays at its old version: the game
        // replaces it with the free content this time.
        Log("entitlements: cannot add the list version; the game resets the list this time");
        RecordOwned({}, false);
        return;
    }
    {
        std::set<std::string> owned = have;
        owned.insert(added.begin(), added.end());
        RecordOwned(std::move(owned), true);
    }
    Log("entitlements: profile had %d codes (version %d); added %zu (%s%s)", count, savedVersion, added.size(),
        g_wantSummary.c_str(), setVersion ? (", free content " + std::to_string(freeCount) + ", version set").c_str() : "");
}

void __fastcall DecodeDetour(ByteArray* a)
{
    g_decodes++;
    g_origDecode(a);
    MergeEntitlements(a);
}

// With no save file the loader builds a default profile and writes it,
// without decoding anything. Loading that file back (the loaded flag
// cleared) sends a fresh profile through the decode like a saved one.
void __fastcall LoaderDetour(void* slot, uint8_t arg)
{
    const int before = g_decodes.load();
    g_origLoader(slot, arg);
    auto* loaded = static_cast<uint8_t*>(slot) + 0x41;
    if (g_decodes.load() != before || *loaded != 1) return;
    Log("entitlements: no save yet (the game wrote a default one); loading it back");
    *loaded = 0;
    g_freshProfile = true;
    g_origLoader(slot, arg);
    g_freshProfile = false;
}

bool Hook(uintptr_t rva, const uint8_t* head, size_t n, LPVOID detour, LPVOID* original, const char* name)
{
    auto* target = reinterpret_cast<uint8_t*>(g_base + rva);
    if (!IsReadable(target, n) || std::memcmp(target, head, n) != 0) {
        Log("entitlements: the %s at rva %llX is not what this build expects; off", name, static_cast<unsigned long long>(rva));
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("entitlements: cannot hook the %s; off", name);
        return false;
    }
    return true;
}

} // namespace

void InstallEntitlements(uintptr_t imageBase, const std::wstring& iniPath)
{
    g_merge = GetPrivateProfileIntW(L"mod", L"steam_entitlements", 1, iniPath.c_str()) != 0;
    if (!g_merge) Log("entitlements: Steam content not added ([mod] steam_entitlements=0); the profile's own list still counts");
    g_base = imageBase;
    ParseGrants();
    auto* realloc = reinterpret_cast<uint8_t*>(imageBase + kReallocRva);
    if (!IsReadable(realloc, sizeof(kReallocHead)) || std::memcmp(realloc, kReallocHead, sizeof(kReallocHead)) != 0) {
        Log("entitlements: FMemory::Realloc is not where this build expects it; off");
        return;
    }
    g_realloc = reinterpret_cast<Realloc_t>(realloc);
    if (!Hook(kDecodeRva, kDecodeHead, sizeof(kDecodeHead), reinterpret_cast<LPVOID>(&DecodeDetour),
              reinterpret_cast<LPVOID*>(&g_origDecode), "save decoder")) {
        return;
    }
    if (g_merge) {
        Hook(kLoaderRva, kLoaderHead, sizeof(kLoaderHead), reinterpret_cast<LPVOID>(&LoaderDetour),
             reinterpret_cast<LPVOID*>(&g_origLoader), "save loader");
    }
    Log("entitlements: on (%zu Steam apps in the table)", g_grants.size());
}

bool EntitlementsKnown()
{
    std::lock_guard lock(g_ownedMutex);
    return g_ownedKnown;
}

bool OwnsEntitlement(const std::string& code)
{
    if (code.empty()) return true;
    std::lock_guard lock(g_ownedMutex);
    return !g_ownedKnown || g_owned.count(code) != 0;
}

} // namespace sfv::mod
