#include "mod/flow_internal.h"

namespace sfv::mod {

using namespace flow;

namespace {

using ExecFn = void (*)(void* context, void* stack, void* result);

ExecFn g_tracedOrig = nullptr;

std::string g_tracedName;

std::atomic<int> g_traceCalls{0};

void TraceDetour(void* context, void* stack, void* result) {
    if (g_traceCalls++ < 200) {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
        Log("functrace: %s on %s (tick %u), from rva 0x%llX", g_tracedName.c_str(), ue4::Describe(context).c_str(), TickNow(),
            static_cast<unsigned long long>(from));
    }
    g_tracedOrig(context, stack, result);
}

std::vector<std::pair<size_t, uintptr_t>> CodePointers(const uint8_t* fn) {
    std::vector<std::pair<size_t, uintptr_t>> out;
    for (size_t at = 0x30; at < 0x140; at += 8) {
        if (!IsReadable(fn + at, 8)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(fn + at);
        if (v > g_base + 0x1000 && v < g_base + 0x2800000) out.push_back({at, v - g_base});
    }
    return out;
}

} // namespace

void FlowLogFunction(const std::string& name) {
    if (!ue4::EnsureObjects(g_base)) return Log("ufunc: no object array");
    int found = 0;
    const bool ofClass = !name.empty() && name[0] == '*';
    ue4::ForEachObject([&](const void* object) {
        if (ue4::ObjectName(ue4::ClassOf(object)) != "Function") return;
        if (ofClass ? ue4::ObjectName(ue4::OuterOf(object)) != name.substr(1) : ue4::ObjectName(object) != name) return;
        found++;
        std::string pointers;
        for (const auto& [at, rva] : CodePointers(static_cast<const uint8_t*>(object))) {
            char b[48];
            std::snprintf(b, sizeof(b), " +0x%zX=0x%llX", at, static_cast<unsigned long long>(rva));
            pointers += b;
        }
        Log("ufunc: %s.%s at %p, code pointers:%s", ue4::ObjectName(ue4::OuterOf(object)).c_str(), ue4::ObjectName(object).c_str(), object,
            pointers.c_str());
    });
    if (!found) Log("ufunc: no function %s", name.c_str());
}

void FlowLogObjectsNamed(const std::string& name, bool dump) {
    if (!ue4::EnsureObjects(g_base)) return Log("uobj: no object array");
    int found = 0;
    ue4::ForEachObject([&](const void* object) {
        if (ue4::ObjectName(object) != name) return;
        found++;
        const void* outer = ue4::OuterOf(object);
        Log("uobj: %s at %p, class %s, outer %s (%s), size %zu", name.c_str(), object, ue4::ObjectName(ue4::ClassOf(object)).c_str(),
            ue4::ObjectName(outer).c_str(), outer ? ue4::ObjectName(ue4::ClassOf(outer)).c_str() : "-", ue4::ObjectSize(object));
        if (!dump) return;
        const auto* o = static_cast<const uint8_t*>(object);
        for (size_t at = 0; at < 0x100 && IsReadable(o + at, 8); at += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(o + at);
            std::string text;
            if (v > 0x10000 && IsReadable(reinterpret_cast<const void*>(v), 16)) {
                const auto* p = reinterpret_cast<const uint8_t*>(v);
                for (int i = 0; i < 16; i++) text += p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : '.';
            }
            Log("uobj:   +0x%02zX %016llX %s", at, static_cast<unsigned long long>(v), text.c_str());
        }
    });
    if (!found) Log("uobj: nothing named %s", name.c_str());
}

void FlowTraceFunction(const std::string& name, const std::string& owner, uint32_t offset) {
    if (g_tracedOrig) return Log("functrace: %s is traced already (one at a time)", g_tracedName.c_str());
    if (!ue4::EnsureObjects(g_base)) return Log("functrace: no object array");
    void* target = nullptr;
    ue4::ForEachObject([&](const void* object) {
        if (target || ue4::ObjectName(object) != name || ue4::ObjectName(ue4::ClassOf(object)) != "Function") return;
        if (!owner.empty() && ue4::ObjectName(ue4::OuterOf(object)) != owner) return;
        const auto* fn = static_cast<const uint8_t*>(object);
        if (IsReadable(fn + offset, 8)) target = *reinterpret_cast<void* const*>(fn + offset);
    });
    if (!target) return Log("functrace: %s%s%s not found", owner.c_str(), owner.empty() ? "" : ".", name.c_str());
    g_tracedName = owner.empty() ? name : owner + "." + name;
    g_traceCalls = 0;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&TraceDetour), reinterpret_cast<void**>(&g_tracedOrig)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_tracedOrig = nullptr;
        return Log("functrace: hook at rva 0x%llX failed", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - g_base));
    }
    Log("functrace: tracing %s (native rva 0x%llX)", g_tracedName.c_str(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - g_base));
}

void FlowLogRoundInfo() {
    uint8_t* d = LiveRoundDirector();
    uint8_t* context = d ? BattleContext(d) : nullptr;
    if (!context || !IsReadable(context + 0x1820, kRoundResult + kRoundResultSize - 0x1820)) {
        Log("roundinfo: no battle context");
        return;
    }
    auto words = [](const uint8_t* p, size_t n) {
        std::string s;
        char b[16];
        for (size_t i = 0; i < n; i += 4) {
            std::snprintf(b, sizeof(b), "%s%x", i ? " " : "", *reinterpret_cast<const uint32_t*>(p + i));
            s += b;
        }
        return s;
    };
    Log("roundinfo: round state (ctx+0x1820): %s", words(context + 0x1820, 0xE4).c_str());
    Log("roundinfo: +0x1904: %s | result +0x78788: %s", words(context + kRoundCount, kRoundCountSize).c_str(),
        words(context + kRoundResult, kRoundResultSize).c_str());
    const uint32_t rounds = std::min<uint32_t>(*reinterpret_cast<const uint32_t*>(context + kRoundCount + 4), 7);
    for (uint32_t i = 0; i < rounds; i++) {
        const uint8_t* rec = context + kRoundHistory + i * kRoundRecord;
        Log("roundinfo: record %u: %s", i, words(rec, 0x80).c_str());
    }
    Log("roundinfo: last record: %s", words(context + kLastRecord, 0x80).c_str());
}

void FlowLogStageTable() {
    int count = 0;
    const uint8_t* entries = StageEntries(&count);
    if (!entries) {
        Log("stages: the stage table is not loaded yet");
        return;
    }
    Log("stages: %d entries", count);
    for (int i = 0; i < count && i < 256; i++) {
        const uint8_t* e = entries + i * kStageEntrySize;
        Log("  stage %2d: %-5s %-28s %s | %s | %s | %s", i, FStringText(e).c_str(), FlowStageName(i).c_str(), FStringText(e + 0x10).c_str(),
            FStringText(e + 0x20).c_str(), FStringText(e + 0x30).c_str(), FStringText(e + 0x40).c_str());
    }
}

void FlowLogWorldContext() {
    FlowRunOnGameThread([] {
        auto* engine = *reinterpret_cast<uint8_t* const*>(g_base + kEngineGlobal);
        if (!engine) return;
        void* viewport = *reinterpret_cast<void* const*>(engine + 0x5E8);
        if (!viewport) return;
        using GetWorldFn = void* (*)(void*);
        void* world = (*reinterpret_cast<GetWorldFn* const*>(viewport))[0xE0 / 8](viewport);
        using ContextFn = uint8_t* (*)(void*, void*);
        uint8_t* ctx = reinterpret_cast<ContextFn>(g_base + 0x1CD9130)(engine, world);
        Log("worldctx: world %s (%p) context %p", ue4::Describe(world).c_str(), world, static_cast<void*>(ctx));
        if (!ctx) return;
        for (int off = 0x1C0; off < 0x220; off += 0x10) {
            uint64_t a = 0, b = 0;
            std::memcpy(&a, ctx + off, 8);
            std::memcpy(&b, ctx + off + 8, 8);
            Log("worldctx +0x%x: %016llx %016llx", off, (unsigned long long)a, (unsigned long long)b);
        }
    });
}

int FlowSchedulerPhaseNow() { return g_schedPhase; }

std::string FlowRoundEndState() {
    char buf[160];
    const uint8_t* d = LiveRoundDirector();
    const uint8_t* m = LivePhaseMachine();
    const auto* recorder = *reinterpret_cast<const uint8_t* const*>(g_base + 0x39DA0D0);
    const bool haveRecorder = recorder && IsReadable(recorder, 0x420);
    std::snprintf(buf, sizeof(buf),
                  "tick %u, director %d kind %d stop %d, phase %d requested %d, journal %zu, recorder %d/%d %d/%d",
                  TickNow(), d ? Field<int32_t>(d, kDirectorState) : -1, d ? Field<int32_t>(d, kDirectorStopKind) : -1,
                  g_roundFlow.stopTicks, m ? Field<int32_t>(m, 0x130) : -1, m ? Field<int32_t>(m, 0x134) : -1,
                  g_roundOverJournal.size(), haveRecorder ? Field<int32_t>(recorder, 0xE0) : -1,
                  haveRecorder ? Field<int32_t>(recorder, 0xE4) : -1, haveRecorder ? Field<int32_t>(recorder, 0x1B0) : -1,
                  haveRecorder ? Field<int32_t>(recorder, 0x1B4) : -1);
    return buf;
}

uint8_t* FlowPhaseMachineObject() { return LivePhaseMachine(); }

void FlowLogPhaseMachine() {
    uint8_t* m = g_phaseMachine;
    if (!m || !IsReadable(m, 0x140)) {
        Log("phases: no phase machine seen yet (it is known once a battle ticks)");
        return;
    }
    auto q = [](const uint8_t* p, uint32_t off) { return *reinterpret_cast<const uint64_t*>(p + off); };
    auto d = [](const uint8_t* p, uint32_t off) { return *reinterpret_cast<const int32_t*>(p + off); };
    auto rva = [](uint64_t v) { return v >= g_base && v < g_base + 0x5000000 ? v - g_base : 0; };
    auto where = [](const void* p) { return g_regionOf ? g_regionOf(p) : std::string(); };
    Log("phases: machine %p (vtable rva 0x%llX) [%s], current %d, requested %d, +0x138 %d, +0x13C %02x, current object %p",
        static_cast<void*>(m), static_cast<unsigned long long>(rva(q(m, 0))), where(m + 0x130).c_str(), d(m, 0x130),
        d(m, 0x134), d(m, 0x138), m[0x13C], reinterpret_cast<void*>(q(m, 0x120)));
    if (uint8_t* battle = BattleContext(m)) {
        Log("phases: battle context %p, +0x18D0 %08x [%s]", static_cast<void*>(battle), d(battle, 0x18D0),
            where(battle + 0x18D0).c_str());
        for (uint32_t off : {0x1904u, 0x1908u, 0x1910u, 0x1910u + 0x10FC8u, 0x78788u, 0x78798u, 0x787ACu, 0x787B8u}) {
            Log("phases:   battle context +0x%X [%s]", off, where(battle + off).c_str());
        }
    }
    const auto* phases = reinterpret_cast<const uint8_t*>(q(m, 0xF8));
    const int count = phases ? static_cast<int>((q(m, 0x100) - q(m, 0xF8)) / 16) : 0;
    if (!phases || count <= 0 || count > 64 || !IsReadable(phases, count * 16)) {
        Log("phases: no phase array (%p, %d)", static_cast<const void*>(phases), count);
        return;
    }
    for (int i = 0; i < count; i++) {
        const auto* obj = reinterpret_cast<const uint8_t*>(q(phases, i * 16));
        if (!obj || !IsReadable(obj, 0x40)) {
            Log("phases: [%d] %p", i, static_cast<const void*>(obj));
            continue;
        }
        Log("phases: [%d] %p [%s] vtable rva 0x%llX, id %d, +0x8 %d, +0x50 rva 0x%llX, +0x68 rva 0x%llX, +0x70 %016llx, +0x78 %08x",
            i, static_cast<const void*>(obj), where(obj + 0x70).c_str(), static_cast<unsigned long long>(rva(q(obj, 0))), d(obj, 0x20), d(obj, 8),
            static_cast<unsigned long long>(rva(q(obj, 0x50))), static_cast<unsigned long long>(rva(q(obj, 0x68))),
            static_cast<unsigned long long>(q(obj, 0x70)), d(obj, 0x78));
    }
}

std::string FlowHistory() {
    std::lock_guard lock(g_mutex);
    std::string s;
    for (const auto& l : g_history) s += l + " | ";
    return s;
}

} // namespace sfv::mod
