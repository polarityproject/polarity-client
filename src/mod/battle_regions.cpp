// Research commands over the snapshot regions: find, compare, dump, trace.

#include "mod/battle_internal.h"

#include "mod/battle_actors.h"
#include "mod/files.h"
#include "mod/ue4.h"
#include "mod/watch.h"

#include <map>

namespace sfv::mod {

using namespace battle;

namespace {

const StateRegion* FindRegion(const std::string& name) {
    if (!g_game) return nullptr;
    const size_t dot = name.find('.');
    if (name.size() > 1 && name[0] == 'v' && dot != std::string::npos) {
        const std::string want = "vtable rva 0x" + name.substr(1, dot - 1) + " ";
        int n = std::atoi(name.c_str() + dot + 1);
        for (const StateRegion& r : g_game->Regions()) {
            if (BattleRegionOwner(r.ptr).rfind(want, 0) == 0 && n-- == 0) return &r;
        }
        return nullptr;
    }
    for (const StateRegion& r : g_game->Regions()) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

std::map<std::string, std::vector<uint8_t>> g_regionSnap;

std::atomic<int> g_ftrace{0};
FILE* g_ftraceFile = nullptr;
std::vector<std::vector<uint8_t>> g_ftracePrev;
uint32_t g_ftraceTick = 0;

} // namespace

std::string BattleRegionOwner(const void* region) {
    const uintptr_t base = g_imageBase ? g_imageBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uintptr_t kTextBegin = 0x1000, kTextEnd = 0x2C28000, kRdataEnd = 0x3966000;
    const auto* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(region) & ~uintptr_t{7});
    for (size_t back = 0; back <= 0x4000; back += 8) {
        const uint8_t* at = p - back;
        if (!IsReadable(at, 8)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
        if (v < base + kTextEnd || v >= base + kRdataEnd || !IsReadable(reinterpret_cast<const void*>(v), 8)) continue;
        const uintptr_t first = *reinterpret_cast<const uintptr_t*>(v);
        if (first < base + kTextBegin || first >= base + kTextEnd) continue;
        char b[64];
        std::snprintf(b, sizeof(b), "vtable rva 0x%llx at -0x%zx", static_cast<unsigned long long>(v - base),
                      static_cast<size_t>(static_cast<const uint8_t*>(region) - at));
        return b;
    }
    return "unknown";
}

void BattleWatchRegionNamed(const std::string& name, size_t offset, int length) {
    const StateRegion* r = FindRegion(name);
    if (!r || offset + length > r->size) return Log("watchname: no region %s with +0x%zx", name.c_str(), offset);
    Log("watchname: %s +0x%zx (%d bytes): %s", name.c_str(), offset, length, ArmWatch(2, r->ptr + offset, length, g_imageBase) ? "armed" : "not armed");
}

void BattleFindInRegions(const std::string& kind, const std::string& a, const std::string& b) {
    if (!g_game) return;
    uintptr_t lo = 0, hi = 0;
    if (kind == "bac") {
        void* object = ue4::EnsureObjects(g_imageBase) ? ue4::FindObject(a, "KWBAC") : nullptr;
        if (!object || !IsReadable(static_cast<uint8_t*>(object) + kMoveFileDataOff, 0x10)) return Log("rfind: no KWBAC %s", a.c_str());
        lo = *reinterpret_cast<const uintptr_t*>(static_cast<uint8_t*>(object) + kMoveFileDataOff);
        hi = lo + *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(object) + kMoveFileSizeOff);
        Log("rfind: %s data at %p..%p", a.c_str(), reinterpret_cast<void*>(lo), reinterpret_cast<void*>(hi));
    } else if (kind == "near") {
        const StateRegion* r = FindRegion(a);
        if (!r) return Log("rfind: no region %s", a.c_str());
        lo = reinterpret_cast<uintptr_t>(r->ptr) - 0x1000;
        hi = reinterpret_cast<uintptr_t>(r->ptr) + r->size;
    }
    const float f = kind == "f" ? std::strtof(a.c_str(), nullptr) : 0;
    const int32_t n = kind == "i" ? static_cast<int32_t>(std::strtol(a.c_str(), nullptr, 0)) : 0;
    int hits = 0;
    for (const StateRegion& r : g_game->Regions()) {
        const size_t step = kind == "bac" || kind == "near" ? 8 : 4;
        for (size_t at = 0; at + step <= r.size && hits < 60; at += step) {
            bool match = false;
            if (kind == "f") match = std::fabs(*reinterpret_cast<const float*>(r.ptr + at) - f) < 1e-4f;
            else if (kind == "i") match = *reinterpret_cast<const int32_t*>(r.ptr + at) == n;
            else if (kind == "bac" || kind == "near") {
                const uintptr_t v = *reinterpret_cast<const uintptr_t*>(r.ptr + at);
                match = v >= lo && v < hi;
            }
            if (!match) continue;
            hits++;
            const uint64_t word = *reinterpret_cast<const uint64_t*>(r.ptr + (at & ~size_t{7}));
            Log("rfind: %s +0x%zX (%p): %016llX%s", r.name.c_str(), at, static_cast<const void*>(r.ptr + at), static_cast<unsigned long long>(word),
                kind == "bac" ? (" = data +0x" + [&] { char t[16]; std::snprintf(t, sizeof(t), "%llX", static_cast<unsigned long long>(*reinterpret_cast<const uintptr_t*>(r.ptr + at) - lo)); return std::string(t); }()).c_str() : "");
        }
    }
    Log("rfind: %d hits", hits);
    (void)b;
}

void BattleTraceRegions(int ticks) {
    if (g_ftraceFile) std::fclose(g_ftraceFile), g_ftraceFile = nullptr;
    g_ftracePrev.clear();
    g_ftraceTick = 0;
    if (!g_game || ticks <= 0) return;
    const std::wstring dir = FilesResearchDir();
    g_ftraceFile = _wfopen((dir + L"\\ftrace.bin").c_str(), L"wb");
    if (!g_ftraceFile) return;
    const auto& regions = g_game->Regions();
    const uint32_t n = static_cast<uint32_t>(regions.size());
    std::fwrite(&n, 4, 1, g_ftraceFile);
    for (const StateRegion& r : regions) {
        char at[32];
        std::snprintf(at, sizeof(at), " @%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r.ptr)));
        const std::string name = r.name + " " + BattleRegionOwner(r.ptr) + at;
        const uint16_t len = static_cast<uint16_t>(name.size());
        const uint32_t size = static_cast<uint32_t>(r.size);
        std::fwrite(&len, 2, 1, g_ftraceFile);
        std::fwrite(name.data(), 1, len, g_ftraceFile);
        std::fwrite(&size, 4, 1, g_ftraceFile);
        g_ftracePrev.emplace_back(r.size, uint8_t{0});
    }
    g_ftrace = ticks;
    Log("ftrace: %u regions, the next %d ticks", n, ticks);
}

void BattleFtraceTick() {
    if (g_ftrace <= 0 || !g_game || !g_ftraceFile) return;
    const auto& regions = g_game->Regions();
    if (regions.size() != g_ftracePrev.size()) return;
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    put(&g_ftraceTick, 4);
    uint32_t fighters[4] = {};
    int f = 0;
    for (const StateRegion& r : regions) {
        if (r.size != actor::kRegionSize || f >= 2 || !IsReadable(r.ptr - actor::kVtableAt, 8)) continue;
        if (*reinterpret_cast<const uintptr_t*>(r.ptr - actor::kVtableAt) - g_imageBase != actor::kFighterVtable) continue;
        const auto* runner = *reinterpret_cast<const uint8_t* const*>(r.ptr + actor::kRunner);
        if (runner && IsReadable(runner, 0x60)) {
            fighters[2 * f] = *reinterpret_cast<const uint32_t*>(runner + actor::kRunnerIndex);
            fighters[2 * f + 1] = *reinterpret_cast<const uint32_t*>(runner + actor::kRunnerFrame);
        }
        f++;
    }
    put(fighters, sizeof(fighters));
    const size_t countAt = out.size();
    uint32_t count = 0;
    put(&count, 4);
    for (size_t i = 0; i < regions.size(); i++) {
        const StateRegion& r = regions[i];
        uint8_t* prev = g_ftracePrev[i].data();
        for (size_t at = 0; at + 4 <= r.size; at += 4) {
            uint32_t v;
            std::memcpy(&v, r.ptr + at, 4);
            uint32_t p;
            std::memcpy(&p, prev + at, 4);
            if (v == p && g_ftraceTick) continue;
            std::memcpy(prev + at, &v, 4);
            const uint16_t ri = static_cast<uint16_t>(i);
            const uint32_t off = static_cast<uint32_t>(at);
            put(&ri, 2);
            put(&off, 4);
            put(&v, 4);
            count++;
        }
    }
    std::memcpy(out.data() + countAt, &count, 4);
    std::fwrite(out.data(), 1, out.size(), g_ftraceFile);
    g_ftraceTick++;
    if (--g_ftrace <= 0) {
        std::fclose(g_ftraceFile);
        g_ftraceFile = nullptr;
        Log("ftrace: done, %u ticks", g_ftraceTick);
    }
}

void BattleCompareRegions(const std::string& a, const std::string& b, long long from, long long to) {
    if (!g_game) return;
    const StateRegion *ra = FindRegion(a), *rb = FindRegion(b);
    if (!ra || !rb) return Log("rcmp: no region %s or %s", a.c_str(), b.c_str());
    int hits = 0;
    for (long long at = from; at + 4 <= to && hits < 120; at += 4) {
        if (!IsReadable(ra->ptr + at, 4) || !IsReadable(rb->ptr + at, 4)) continue;
        const uint32_t x = *reinterpret_cast<const uint32_t*>(ra->ptr + at), y = *reinterpret_cast<const uint32_t*>(rb->ptr + at);
        if (x == y || (x > 0x10000 && x < 0xFFFF0000 && y > 0x10000 && y < 0xFFFF0000)) continue;
        hits++;
        Log("rcmp: %+lld (0x%llX): %08X vs %08X", at, static_cast<unsigned long long>(at < 0 ? -at : at), x, y);
    }
    Log("rcmp: %d hits", hits);
}

void BattleDumpRegion(const std::string& name, const std::string& tag) {
    const StateRegion* r = FindRegion(name);
    if (!r || !IsReadable(r->ptr - 0x40, r->size + 0x40)) return Log("rdump: no region %s", name.c_str());
    const std::wstring dir = FilesResearchDir();
    const std::wstring path = dir + L"\\" + std::wstring(tag.begin(), tag.end()) + L".bin";
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(r->ptr - 0x40, 1, r->size + 0x40, f);
        std::fclose(f);
        Log("rdump: %s (%s, %zu bytes) to Polarity\\research\\%s.bin", name.c_str(), r->name.c_str(), r->size, tag.c_str());
    }
}

void BattlePeekRegion(const std::string& name, long long offset, int qwords) {
    if (!g_game) return;
    if (const StateRegion* found = FindRegion(name)) {
        const StateRegion& r = *found;
        for (int i = 0; i < qwords && i < 64; i++) {
            const uint8_t* p = r.ptr + offset + 8 * i;
            if (!IsReadable(p, 8)) break;
            const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
            std::string note;
            for (const StateRegion& o : g_game->Regions()) {
                const auto at = static_cast<long long>(v - reinterpret_cast<uintptr_t>(o.ptr));
                if (at >= -0x2000 && at < static_cast<long long>(o.size)) {
                    note = " = " + o.name + (at < 0 ? " -" : " +") + std::to_string(at < 0 ? -at : at);
                    break;
                }
            }
            if (note.empty() && ImageHolds(v)) {
                char b[32];
                std::snprintf(b, sizeof(b), " (rva 0x%llX)", static_cast<unsigned long long>(v - g_imageBase));
                note = b;
            }
            Log("rpeek: %s %+lld: %016llX%s", name.c_str(), offset + 8 * i, static_cast<unsigned long long>(v), note.c_str());
        }
        return;
    }
    Log("rpeek: no region %s", name.c_str());
}

void BattleScanForPointers(const std::string& from, size_t before, size_t after, const std::string& to) {
    if (!g_game) return;
    const StateRegion *f = FindRegion(from), *t = FindRegion(to);
    if (!f || !t) return Log("ptrscan: no region %s or %s", from.c_str(), to.c_str());
    const uintptr_t lo = reinterpret_cast<uintptr_t>(t->ptr) - 0x20000, hi = reinterpret_cast<uintptr_t>(t->ptr) + t->size;
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(f->ptr - before) & ~uintptr_t{7});
    const uint8_t* end = f->ptr + f->size + after;
    int hits = 0;
    for (const uint8_t* p = begin; p + 8 <= end && hits < 40; p += 8) {
        if (!IsReadable(p, 8)) {
            p = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(p) | 0xFFF)) - 7;
            continue;
        }
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        if (v < lo || v >= hi) continue;
        hits++;
        Log("ptrscan: %s %+lld holds %s %+lld", from.c_str(), static_cast<long long>(p - f->ptr), to.c_str(),
            static_cast<long long>(v - reinterpret_cast<uintptr_t>(t->ptr)));
    }
    Log("ptrscan: %d hits", hits);
}

void BattleSnapRegions() {
    g_regionSnap.clear();
    if (!g_game) return;
    for (const StateRegion& r : g_game->Regions()) g_regionSnap[r.name].assign(r.ptr, r.ptr + r.size);
    Log("rsnap: %zu regions", g_regionSnap.size());
}

void BattleDiffRegions(const std::string& mode, const std::string& prefix) {
    if (!g_game) return;
    int hits = 0;
    for (const StateRegion& r : g_game->Regions()) {
        if (!prefix.empty() && r.name.rfind(prefix, 0) != 0) continue;
        const auto it = g_regionSnap.find(r.name);
        if (it == g_regionSnap.end() || it->second.size() != r.size) continue;
        const uint8_t* old = it->second.data();
        for (size_t at = 0; at + 4 <= r.size && hits < 200; at += 4) {
            const uint32_t a = *reinterpret_cast<const uint32_t*>(old + at), b = *reinterpret_cast<const uint32_t*>(r.ptr + at);
            if (a == b) continue;
            bool show = mode == "all";
            if (mode == "side") {
                const float fa = *reinterpret_cast<const float*>(&a), fb = *reinterpret_cast<const float*>(&b);
                show = (fa == 1.0f && fb == -1.0f) || (fa == -1.0f && fb == 1.0f) || (a <= 1 && b <= 1);
                for (int k = 0; k < 4 && !show; k++) {
                    const uint8_t x = old[at + k], y = r.ptr[at + k];
                    show = x != y && x <= 1 && y <= 1;
                }
            }
            if (!show) continue;
            hits++;
            Log("rdiff: %s +0x%zX %08X -> %08X", r.name.c_str(), at, a, b);
        }
    }
    Log("rdiff: %d hits", hits);
}

void BattleLogRegionsOfSize(size_t size) {
    if (!g_game) return;
    if (size == 0) {
        std::map<std::string, std::map<size_t, int>> byOwner;
        for (const StateRegion& r : g_game->Regions()) byOwner[BattleRegionOwner(r.ptr)][r.size]++;
        for (const auto& [owner, sizes] : byOwner) {
            std::string text;
            for (const auto& [s, n] : sizes) text += " " + std::to_string(s) + (n > 1 ? "x" + std::to_string(n) : "");
            Log("regions: owner %s:%s", owner.c_str(), text.c_str());
        }
        return;
    }
    for (const StateRegion& r : g_game->Regions()) {
        if (r.size != size) continue;
        std::string words;
        for (size_t w = 0; w + 8 <= r.size && w < 0x100; w += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(r.ptr + w);
            if (!v) continue;
            char b[40];
            std::snprintf(b, sizeof(b), " +0x%zx=%llx", w, static_cast<unsigned long long>(v));
            words += b;
        }
        Log("regions: %s at %p, owner %s:%s", r.name.c_str(), static_cast<void*>(r.ptr), BattleRegionOwner(r.ptr).c_str(),
            words.c_str());
    }
}

} // namespace sfv::mod
