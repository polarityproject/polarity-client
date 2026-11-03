#pragma once
// IGame over Street Fighter V's own rollback manager.
//
// Available now (from static analysis, to be confirmed by the probes):
//   - the snapshot region list, read from the manager's {ptr, size} table
//   - save/load of those regions, plus the game's post-restore callbacks
//   - restoring a frame held in the game's own ring (the plan's handle mode)
//   - per-region pointer masks (the state map) so hashes are portable
//
// Not available until phase 3 finds the hooks:
//   - BeginMatch: driving local Versus into a given MatchSetup
//   - ReadLocalInput / Advance: the per-player input slot in InputUnit and a
//     way to run exactly one battle tick on demand

#include "core/game.h"
#include "mod/image.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sfv::mod {

// True for a value that is an address in use in this process (heap, image):
// the AddressTest for state hashing (core/state.h).
bool LooksLikeAddress(uint64_t value);

// Which 8-byte words of each region hold pointers. Learned from local matches
// by mode=learn_statemap and shipped with the mod, so every peer masks the
// same words.
struct StateMap {
    struct Region {
        uint64_t size = 0;
        std::vector<uint8_t> ignore;
    };
    std::vector<Region> regions;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    uint64_t Fingerprint() const;
};

// Diagnostics: a write watch on a region (by its canonical name, "s884.0") of
// the next battle, from the moment its region list is built.
void WatchRegionOfNextBattle(const std::string& name, size_t offset, int length);
void ArmPendingRegionWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase);

// Game callbacks a load does not run, by key (thunk rva << 32 | the rva of
// the function its context names), as the load cost lines print them. Empty:
// all run. Set at start from [netplay] skip_load_callbacks; `skipcallbacks`
// changes it (research).
void SetSkippedLoadCallbacks(std::vector<uint64_t> keys);
// The callbacks loads leave out by default ([netplay] skip_load_callbacks).
std::vector<uint64_t> DefaultSkippedLoadCallbacks();

class SfvGame : public IGame {
public:
    explicit SfvGame(const GameImage& image) : image_(image) {}

    // Called from the RollbackUnit::Pre hook, on the game thread, every tick.
    void OnRollbackUnitTick(uint8_t* unit);

    uint8_t* Unit() const { return unit_; }
    uint8_t* Manager() const;

    // Re-reads the region table. True when the layout changed (new match).
    bool RefreshRegions();
    // KiwiGameSingleton's battle tick counter (outside the game's snapshot).
    uint8_t* SingletonTicks() const;
    // Increments whenever the layout changes; a driver bound to one battle
    // compares it to notice that battle ending.
    int LayoutVersion() const { return layoutVersion_; }
    // Sorts the regions again by their current contents. The order is fixed
    // when a battle's regions first appear, mid-setup; peers that sort again
    // at the same frame list them identically (for comparing dumps).
    void Recanonicalize()
    {
        FindPresentationState();
        rawLayout_.clear();
        RefreshRegions();
    }
    // Battle state the game keeps outside its snapshot that the battle ticks
    // advance (outside_probe.h): the pawns' and the camera's animation clocks.
    // Rolled back with the rest, else a rollback during the intro or the round
    // call leaves them ahead and the fight starts early on that machine.
    void FindPresentationState();
    void SetStateMap(const StateMap* map);

    // The InputUnit and BattleFrame objects, seen by their hooks. Their frame
    // counters are added to the snapshot as extra regions (they are missing
    // from the game's own list) when includeSyncCounters is set.
    void OnInputUnit(uint8_t* inputUnit);
    // A different BattleFrame object means a new battle: the unit pointers
    // seen so far belong to the old one and are dropped until seen again.
    void OnBattleFrame(uint8_t* battleFrame)
    {
        lastFrameMs_ = GetTickCount64();
        if (battleFrame != battleFrame_) {
            battleFrame_ = battleFrame;
            unit_ = nullptr;
            inputUnit_ = nullptr;
            battleFrameVersion_++;
        }
    }
    int BattleFrameVersion() const { return battleFrameVersion_; }

    // A new battle's warm-up (netplay and replays), once per frame after
    // BeginWarmup: true while it runs a neutral tick, until the battle's
    // objects (and so its state regions) exist; then it is frozen at frame 0
    // as it is at that moment, which does not depend on how many ticks that
    // took (12 or 14 give the same state). Ticks after that would start the
    // round's flow (outside the snapshot). A battle whose objects came after
    // only 3 ticks (its game busy loading) went on differently from frame 0
    // than one that took 6 or more (2026-10-30: two players at 3, their
    // spectator at 16, a desync from frame 0; cause open).
    void BeginWarmup()
    {
        warmupTicks_ = 0;
        objectsAt_ = -1;
    }
    bool WarmupStep();
    int WarmupTicks() const { return warmupTicks_; }
    int ObjectsAt() const { return objectsAt_; } // the neutral tick the objects showed up at, -1 before
    // A new battle that reuses the old BattleFrame object: forget the units.
    void ForgetBattleObjects()
    {
        presentation_.clear();
        unit_ = nullptr;
        inputUnit_ = nullptr;
    }
    // A battle frame ran within the last half second.
    bool BattleRunning() const { return battleFrame_ && GetTickCount64() - lastFrameMs_ < 500; }
    // The battle simulation itself ticked (its InputUnit recorded a frame)
    // within the last half second; false while the pause menu is open.
    bool BattleTicking() const { return inputUnit_ && GetTickCount64() - lastInputMs_ < 500; }
    uint8_t* InputUnit() const { return inputUnit_; }
    uint8_t* BattleFrameObject() const { return battleFrame_; }
    bool includeSyncCounters = true;

    // Runs exactly one battle tick with these inputs (through the original
    // BattleFrame::Update), on the game thread, between frames.
    using BattleFrameFn = void (*)(uint8_t*);
    void SetBattleFrameFunction(BattleFrameFn fn) { runFrame_ = fn; }
    bool CanRunTicks() const { return runFrame_ && battleFrame_; }
    // Physical pad slot whose input is "the local player" (pad 0 = P1 slot).
    int localPadSlot = 0;
    std::atomic<int> inputLog{0}; // research: ticks to log the game's per-pad values for
    // When set, ReadLocalInput(p) returns pad p's own input (both local: tests).
    bool bothPadsLocal = false;
    // When set, the local player's input comes from here instead of what the
    // game computed from the pad (netplay isolates the pads from the game).
    PackedInput (*localInput)(int pad) = nullptr;
    // When set, both players' inputs come from here (a replay's stress run
    // feeds a recording's inputs through a stress session, replay_driver.h).
    std::function<PackedInput(int player)> inputOverride;
    // Set while a session owns the battle: only the ticks it runs may record
    // inputs (battle_driver.cpp).
    bool exclusiveTicks = false;
    // Driven ticks latch the pads' values themselves (LatchInputs); off only
    // to replay recordings from before adapter-9, made with the game's own
    // once-per-rendered-frame latch.
    bool latchEachTick = true;

    // Diagnostics for a desync from frame 0 whose cause lies outside the
    // snapshot (Guile vs Lucia 43c9c9cb, 2026-11-03: one side's battle objects
    // came after only 3 neutral ticks, and its battle camera panned a tick
    // later from frame 0). The battle camera (its 112-byte region at +0x1C0)
    // and both fighters' simulation objects (their 300-byte regions at
    // +0x764) are kept as they are at the freeze, at the session's start and
    // after its first ticks, and go to the log when asked: a desync in the
    // first frames (both sides then log theirs), or `startprobe`. Short
    // warm-ups alone are common (both games of a local session: 3 ticks).
    // ArmStartProbe: at the freeze. CaptureStartProbe: at the session's start.
    void ArmStartProbe();
    void CaptureStartProbe(const char* when);
    void LogStartProbe(const char* why);

    // Words that differ between the first simulation of a frame and its
    // resimulation without affecting gameplay (engine-side transforms read by
    // gameplay code), keyed by region size and word offset so the mask holds
    // across matchups. Left out of hashes. Learned by "learnnoise".
    bool LoadNoise(const std::wstring& path);
    void LoadNoiseText(const char* text, size_t len); // the sfv_noise.txt format
    // What BuildId's noise part hashes: the mask as loaded.
    uint64_t NoiseHash() const;
    bool SaveNoise(const std::wstring& path) const;
    bool AddNoise(size_t regionSize, size_t offset); // true if new
    size_t NoiseWords() const
    {
        size_t n = 0;
        for (const auto& [size, offsets] : noise_) n += offsets.size();
        return n;
    }

    // Adds the words of the current region contents that look like pointers.
    void LearnPointers(StateMap& map) const;

    struct Inventory {
        size_t regionCount = 0;
        uint64_t stateBytes = 0;
        size_t groupCount = 0;
        uint64_t ringCount = 0;
        uint64_t ringMapSize = 0;
        game::FrameId currentId{-1, -1};
        size_t postRestoreCallbacks = 0;
    };
    Inventory Inspect() const;
    // Logs every region, group and callback (once per match).
    void LogLayout() const;

    // Matches won by each side since the last character select (the game's
    // WinCountLeft / WinCountRight), from the battle's round state; false
    // when no battle's regions are known.
    bool MatchWins(uint32_t& left, uint32_t& right) const;

    // The game's own Restore on a frame still held in its ring.
    bool RestoreRingFrame(const game::FrameId& id, bool discardNewer);

    // IGame
    std::string BuildId() const override;
    bool BeginMatch(const MatchSetup& setup) override;
    const std::vector<StateRegion>& Regions() override { return regions_; }
    PackedInput ReadLocalInput(int player) override;
    void Advance(const FrameInputs& inputs, bool rollingBack) override;
    void OnStateLoaded(int frame) override;
    bool OutcomeDecided() override;

private:
    // A region of a pooled visual effect object (kEffectRegions in sfv_game.cpp):
    // out of the checksum.
    bool IsEffectRegion(const StateRegion& r) const;
    void RunPostRestoreCallbacks(const game::FrameId& id);
    void LatchInputs(); // InputUnit::LatchFrame, after a tick or a load
    struct ProbeCapture {
        std::string when;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> objects; // name, bytes
    };
    std::mutex probeMutex_;
    std::vector<ProbeCapture> probe_;
    int probeTicks_ = 0;     // driven ticks after the freeze still to capture
    bool probeLogged_ = false;
    // Time after loads (performance counter ticks), per game callback (by rva)
    // and for the round state the mod puts back (OnStateLoaded logs them).
    struct CallbackCost {
        int64_t ticks = 0;
        int calls = 0;
    };
    std::map<uintptr_t, CallbackCost> callbackCosts_;
    int64_t ownLoadTicks_ = 0;
    int loadsTimed_ = 0;

    GameImage image_;
    uint8_t* unit_ = nullptr;
    uint8_t* inputUnit_ = nullptr;
    struct ExtraRegion {
        std::string name;
        uint8_t* ptr;
        size_t size;
    };
    std::vector<ExtraRegion> presentation_;
    uint64_t lastInputMs_ = 0;
    uint8_t* battleFrame_ = nullptr;
    BattleFrameFn runFrame_ = nullptr;
    uint32_t sampled_[4] = {}; // inputs the game computed for each pad, last tick
    uint32_t inject_[2] = {};
    bool injecting_ = false;
    std::vector<StateRegion> regions_;
    std::vector<std::pair<uint8_t*, size_t>> rawLayout_;
    int layoutVersion_ = 0;
    int battleFrameVersion_ = 0;
    int warmupTicks_ = 0;
    int objectsAt_ = -1;
    std::atomic<uint64_t> lastFrameMs_{0};
    std::map<size_t, std::set<size_t>> noise_;
    const StateMap* stateMap_ = nullptr;
};

} // namespace sfv::mod
