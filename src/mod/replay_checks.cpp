#include "mod/replay_internal.h"

namespace sfv::mod {

using namespace replay;

namespace {

// How far before or after its region a trap may watch the owner's memory.
constexpr long long kTrapSlack = 0x1000;
// A mask byte that leaves its whole byte out of the checksum.
constexpr uint8_t kIgnoredByte = 0xFF;

} // namespace

namespace replay {

void ReplayDriver::ArmFromHelper(int slot, const void* address, int length, uintptr_t base) {
    std::thread([=] {
        ThreadGuard running;
        ArmWatch(slot, address, length, base);
    }).join();
}

void ReplayDriver::WatchFrame(SfvGame& game, int f) {
    for (const Trap& t : traps_) {
        if (f != t.frame - 1) continue;
        if (t.region == "director") {
            uint8_t* d = FlowRoundDirector();
            if (!d) {
                Log("replay: trap: no round director");
                continue;
            }
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            for (int done = 0; done < t.bytes && trapSlots_ < 4; done += 8) {
                ArmFromHelper(trapSlots_++, d + t.offset + done, std::min(8, t.bytes - done), base);
            }
            Log("replay: trap armed on the round director +0x%llx (%d bytes) at %p before frame %d",
                static_cast<unsigned long long>(t.offset), t.bytes, static_cast<void*>(d + t.offset), t.frame);
            continue;
        }
        const auto r = std::find_if(game.Regions().begin(), game.Regions().end(),
                                    [&](const StateRegion& x) { return x.name == t.region; });
        const long long size = r == game.Regions().end() ? 0 : static_cast<long long>(r->size);
        const char* sign = t.offset < 0 ? "-" : "+";
        const unsigned long long at = static_cast<unsigned long long>(t.offset < 0 ? -t.offset : t.offset);
        if (r == game.Regions().end() || t.offset < -kTrapSlack || t.offset >= size + kTrapSlack) {
            Log("replay: trap: no region %s %s0x%llx", t.region.c_str(), sign, at);
            continue;
        }
        if (t.offset < 0 || t.offset >= size) Log("replay: trap on %s %s0x%llx is outside the region", t.region.c_str(), sign, at);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for (int done = 0; done < t.bytes && trapSlots_ < 4; done += 8) {
            ArmFromHelper(trapSlots_++, r->ptr + t.offset + done, std::min(8, t.bytes - done), base);
        }
        Log("replay: trap armed on %s %s0x%llx (%d bytes) at %p before frame %d", t.region.c_str(), sign, at, t.bytes,
            static_cast<void*>(r->ptr + t.offset), t.frame);
    }
    if (f < watchFrom_ || f > watchTo_) return;
    if (watchRegion_ == "trails") {
        LogTrails(game, f);
        return;
    }
    std::string bytes = "-";
    const size_t plus = watchRegion_.find('+');
    const std::string watchName = watchRegion_.substr(0, plus);
    const size_t at = plus == std::string::npos ? 0 : std::stoull(watchRegion_.substr(plus + 1), nullptr, 16);
    for (const StateRegion& r : game.Regions()) {
        if (r.name != watchName || at >= r.size) continue;
        char b[64];
        uint64_t w[2] = {};
        std::memcpy(w, r.ptr + at, std::min<size_t>(16, r.size - at));
        std::snprintf(b, sizeof(b), "%016llx %016llx", static_cast<unsigned long long>(w[0]),
                      static_cast<unsigned long long>(w[1]));
        bytes = b;
    }
    std::vector<uint64_t> hashes(game.Regions().size());
    HashRegions(game.Regions(), hashes.data());
    Log("replay: watch frame %d: %s %s | %s | state %08x | camera %s", f, watchRegion_.c_str(), bytes.c_str(),
        FlowRoundEndState().c_str(), FoldChecksum(hashes.data(), hashes.size()), CameraText().c_str());
}

void ReplayDriver::LogTrails(SfvGame& game, int f) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<const uint8_t*> objects;
    for (const StateRegion& r : game.Regions()) {
        if (r.size != 44) continue;
        const uint8_t* obj = r.ptr - 0xC0;
        if (!IsReadable(obj, kTrailBytes) || *reinterpret_cast<const uintptr_t*>(obj) - base != kTrailVtable) continue;
        objects.push_back(obj);
    }
    std::sort(objects.begin(), objects.end());
    const uint8_t* manager = objects.empty() ? nullptr : *reinterpret_cast<const uint8_t* const*>(objects[0] + 0x190);
    auto diff = [&](const char* what, int index, const uint8_t* p, size_t size) {
        if (!p || !IsReadable(p, size)) return;
        std::vector<uint64_t> now(size / 8);
        std::memcpy(now.data(), p, size);
        auto& seen = trailSeen_[p];
        if (seen.size() == now.size()) {
            std::string changes;
            for (size_t w = 0; w < now.size(); w++) {
                if (now[w] == seen[w]) continue;
                char b[80];
                std::snprintf(b, sizeof(b), " +0x%zx %llx>%llx", w * 8, static_cast<unsigned long long>(seen[w]),
                              static_cast<unsigned long long>(now[w]));
                changes += b;
            }
            if (!changes.empty()) Log("replay: trails frame %d %s %d%s", f, what, index, changes.c_str());
        }
        seen = std::move(now);
    };
    if (f == watchTo_ - 9 && trapSlots_ == 0) {
        auto unused = [](const uint8_t* obj) {
            for (size_t w = 0xC0; w < 0xEC; w += 4) {
                if (*reinterpret_cast<const uint32_t*>(obj + w) != 0) return false;
            }
            return true;
        };
        size_t after = 0;
        for (size_t i = 0; i < objects.size(); i++) {
            if (!unused(objects[i])) after = i + 1;
        }
        for (size_t i = after; i < objects.size(); i++) {
            const uint8_t* obj = objects[i];
            if (trapSlots_ >= 4) break;
            ArmFromHelper(trapSlots_++, obj + 0xE8, 4, base);
            Log("replay: trails: watching %p +0xe8 from frame %d", static_cast<const void*>(obj), f + 1);
        }
    }
    for (size_t i = 0; i < objects.size(); i++) diff("trail", static_cast<int>(i), objects[i], kTrailBytes);
    diff("manager", 0, manager, kManagerBytes);
    if (trailSeen_.size() == objects.size() + 1 && f == watchFrom_) {
        Log("replay: trails frame %d: %zu trail objects, manager %p", f, objects.size(), static_cast<const void*>(manager));
    }
}

std::string ReplayDriver::CameraText() {
    constexpr size_t kPov = 0x370;
    constexpr size_t kPlayer = 0x360, kCameraManager = 0x390;
    if (!camera_ && cameraTries_++ % 120 == 0 && ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) {
        ue4::ForEachObject([&](const void* object) {
            if (camera_ || ue4::ObjectName(ue4::ClassOf(object)).find("PlayerController") == std::string::npos) return;
            const auto* p = static_cast<const uint8_t*>(object);
            if (!IsReadable(p + kCameraManager, 8) || !*reinterpret_cast<const void* const*>(p + kPlayer)) return;
            const auto* manager = *reinterpret_cast<const uint8_t* const*>(p + kCameraManager);
            if (manager && ue4::ObjectName(ue4::ClassOf(manager)).find("CameraManager") != std::string::npos) camera_ = manager;
        });
        if (camera_) Log("replay: camera manager %s (%p)", ue4::Describe(camera_).c_str(), static_cast<const void*>(camera_));
    }
    float v[7];
    if (!camera_ || !IsReadable(camera_ + kPov, sizeof(v))) return "-";
    std::memcpy(v, camera_ + kPov, sizeof(v));
    char b[128];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f r %.2f %.2f %.2f fov %.2f", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    return b;
}

bool ReplayDriver::Valid(SfvGame& game) {
    game.RefreshRegions();
    if (game.LayoutVersion() == layout_ && !game.Regions().empty()) return true;
    Log("replay: the battle ended or changed under the replay at frame %d", stress_ > 0 ? session_->NextFrame() : frame_);
    Finish(game);
    return false;
}

const std::vector<StateRegion>& ReplayDriver::CheckRegions(SfvGame& game) {
    const std::vector<StateRegion>& live = game.Regions();
    if (game.LayoutVersion() == checkLayout_ && live.size() == checkCount_) return checkRegions_.empty() ? live : checkRegions_;
    checkLayout_ = game.LayoutVersion();
    checkCount_ = live.size();
    checkRegions_.clear();
    std::set<std::string> recordedNames;
    for (const RegionInfo& r : rf_.regions) recordedNames.insert(r.name);
    std::vector<StateRegion> kept;
    for (const StateRegion& r : live) {
        const bool canonical = r.name.size() > 1 && r.name[0] == 's' && std::isdigit(static_cast<unsigned char>(r.name[1]));
        if (canonical || recordedNames.count(r.name)) kept.push_back(r);
    }
    const size_t leftOut = live.size() - kept.size();
    const auto masksOf = [](const std::string& id) {
        const size_t at = id.find("/map-");
        return at == std::string::npos ? std::string() : id.substr(at);
    };
    if (masksOf(rf_.gameBuild) == masksOf(game.BuildId()) && !leftOut) return live;
    std::map<uint64_t, std::vector<const std::vector<uint8_t>*>> bySize;
    for (const RegionInfo& r : rf_.regions) {
        auto& masks = bySize[r.size];
        if (std::none_of(masks.begin(), masks.end(), [&](const std::vector<uint8_t>* m) { return *m == r.ignore; })) masks.push_back(&r.ignore);
    }
    bool sameOrder = rf_.regions.size() == kept.size();
    for (size_t i = 0; sameOrder && i < kept.size(); i++) sameOrder = rf_.regions[i].size == kept[i].size;
    checkRegions_ = std::move(kept);
    int changed = 0;
    if (masksOf(rf_.gameBuild) != masksOf(game.BuildId())) {
        for (size_t i = 0; i < checkRegions_.size(); i++) {
            const auto& masks = bySize[checkRegions_[i].size];
            const std::vector<uint8_t>* mask = masks.size() == 1 ? masks[0] : sameOrder ? &rf_.regions[i].ignore
                                                                                        : nullptr;
            if (mask && *mask != checkRegions_[i].ignore) {
                checkRegions_[i].ignore = *mask;
                changed++;
            }
        }
    }
    Log("replay: checked with the recording's checksum masks (%d of %zu regions masked differently on this build, %zu "
        "regions it does not have left out)",
        changed, checkRegions_.size(), leftOut);
    if (!changed && !leftOut) checkRegions_.clear();
    return checkRegions_.empty() ? live : checkRegions_;
}

void ReplayDriver::CheckFrame(SfvGame& game, int f) {
    const int before = firstMismatch_;
    CheckFrameAgainstRecording(game, f);
    if (bench_ && failDump_.empty() && before < 0 && firstMismatch_ >= 0) TakeFailDump(game, f, "straight");
    else if (bench_ && failDump_.empty() && f == BenchmarkDumpFrame()) TakeFailDump(game, f, "reference");
}

void ReplayDriver::TakeFailDump(SfvGame& game, int f, const char* label) {
    failDumpFrame_ = f;
    failDumpLabel_ = label;
    failDump_.resize(StateSize(game.Regions()));
    SaveRegions(game.Regions(), failDump_.data());
    failRegions_ = DescribeRegions(game.Regions());
    failHashes_.assign(game.Regions().size(), 0);
    HashRegions(game.Regions(), failHashes_.data());
    Log("replay: benchmark state of frame %d kept (%s)", f, label);
}

void ReplayDriver::CheckFrameAgainstRecording(SfvGame& game, int f) {
    if (f >= rf_.FrameCount() || rf_.hashes.empty()) return;
    std::vector<uint64_t> hashes(CheckRegions(game).size());
    HashRegions(CheckRegions(game), hashes.data());
    framesChecked_++;
    if (const StateDump* dump = rf_.FindDump(f)) CompareDump(game, *dump, f);
    if (rf_.HasRegionHashes(f)) {
        Compare(game, hashes.data(), rf_.FrameHashes(f), f);
        return;
    }
    const bool equal = FoldChecksum(hashes.data(), hashes.size()) == rf_.FrameChecksum(f);
    if (equal) return;
    if (firstMismatch_ < 0) firstMismatch_ = f;
    mismatchFrames_++;
    if (logged_++ < 12) Log("replay: frame %d's checksum differs from the recording (no region hashes kept)", f);
}

void ReplayDriver::CompareDump(SfvGame& game, const StateDump& dump, int f) {
    struct Recorded {
        const uint8_t* bytes;
        size_t size;
        bool taken;
    };

    std::vector<Recorded> recorded;
    size_t at = 0;
    for (const auto& r : rf_.regions) {
        const size_t size = static_cast<size_t>(r.size);
        if (at + size > dump.bytes.size()) break;
        recorded.push_back({dump.bytes.data() + at, size, false});
        at += size;
    }
    int differing = 0, shown = 0;
    std::vector<const StateRegion*> order;
    for (const StateRegion& r : game.Regions()) order.push_back(&r);
    const auto wholly = [](const StateRegion* r) {
        return !r->ignore.empty() && std::all_of(r->ignore.begin(), r->ignore.end(), [](uint8_t b) { return b == kIgnoredByte; });
    };
    std::stable_partition(order.begin(), order.end(), [&](const StateRegion* r) { return !wholly(r); });
    for (const StateRegion* rp : order) {
        const StateRegion& r = *rp;
        auto masked = [&r](size_t word) { return word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1); };
        auto diff = [&](const Recorded& d) {
            size_t n = 0;
            for (size_t w = 0; w + 8 <= r.size; w += 8) n += !masked(w / 8) && std::memcmp(r.ptr + w, d.bytes + w, 8) != 0;
            return n;
        };
        Recorded* best = nullptr;
        size_t bestDiff = SIZE_MAX;
        for (auto& d : recorded) {
            if (d.taken || d.size != r.size) continue;
            const size_t n = diff(d);
            if (n < bestDiff) {
                best = &d;
                bestDiff = n;
                if (n == 0) break;
            }
        }
        if (!best) continue;
        best->taken = true;
        if (bestDiff == 0) continue;
        differing++;
        if (shown < 96) Log("replay: frame %d dump: %s owner %s", f, r.name.c_str(), BattleRegionOwner(r.ptr).c_str());
        for (size_t w = 0; w + 8 <= r.size && shown < 96; w += 8) {
            if (masked(w / 8) || std::memcmp(r.ptr + w, best->bytes + w, 8) == 0) continue;
            uint64_t mine, theirs;
            std::memcpy(&mine, r.ptr + w, 8);
            std::memcpy(&theirs, best->bytes + w, 8);
            Log("replay: frame %d dump: %s +0x%zx here %016llx, recorded %016llx", f, r.name.c_str(), w,
                static_cast<unsigned long long>(mine), static_cast<unsigned long long>(theirs));
            shown++;
        }
    }
    Log("replay: frame %d against the recording's dump: %d regions differ", f, differing);
}

int ReplayDriver::Compare(SfvGame& game, const uint64_t* ours, const uint64_t* recorded, int f) {
    if (!recorded) return 0;
    std::map<uint64_t, std::multiset<uint64_t>> bySize;
    for (size_t j = 0; j < rf_.regions.size(); j++) bySize[rf_.regions[j].size].insert(recorded[j]);
    int differ = 0;
    std::string names;
    const std::vector<StateRegion>& regions = CheckRegions(game);
    for (size_t i = 0; i < regions.size(); i++) {
        auto& pool = bySize[regions[i].size];
        const auto it = pool.find(ours[i]);
        if (it != pool.end()) {
            pool.erase(it);
            continue;
        }
        const std::string& name = regions[i].name;
        if (f >= 0 && (name == "viewtarget.clock" || name == "pawn.clock")) {
            presentationFrames_++;
            continue;
        }
        differ++;
        if (f >= 0) regionFrames_[name]++;
        if (names.size() < 160) names += (names.empty() ? "" : ", ") + name;
    }
    if (differ && f >= 0) {
        if (firstMismatch_ < 0) firstMismatch_ = f;
        mismatchFrames_++;
        if (logged_++ < 12) Log("replay: frame %d differs from the recording in %d regions: %s", f, differ, names.c_str());
    }
    if (differ && f == -1) {
        if (firstMismatch_ < 0) firstMismatch_ = 0;
        Log("replay: state before frame 0 differs in %d regions: %s", differ, names.c_str());
    }
    return differ;
}

void ReplayDriver::Finish(SfvGame& game) {
    ForgetHudMovies();
    if (trapSlots_) {
        WatchReport();
        DisarmWatches();
        trapSlots_ = 0;
    }
    if (stress_ == 0 && keepFrom_ >= 0 && kept_.FrameCount() > 0) {
        kept_.finalThrough = kept_.FrameCount() - 1;
        const std::string stem = name_.substr(0, name_.rfind('.'));
        const std::string path = Utf8Path(MatchesDir()) + "\\" + stem + "_kept.sfvr";
        std::string error;
        if (SaveReplay(kept_, path, &error, ReplayDetail::Full)) Log("replay: kept this run as %s", path.c_str());
        else Log("replay: cannot keep this run: %s", error.c_str());
    }
    game.inputOverride = nullptr;
    game.exclusiveTicks = false;
    game.latchEachTick = true;
    if (bench_) {
        CapturedRun run;
        if (benchCapturing_) benchCaptured_ = hitch::EndCapture();
        for (const auto& c : benchCaptured_) {
            run.micros.push_back(c.micros);
            run.net.push_back(c.net);
            run.battle.push_back(c.battle);
            run.draw.push_back(c.draw);
        }
        run.matched = firstMismatch_ < 0 && stressFrames_.empty();
        run.firstDiffer = firstMismatch_ >= 0 ? firstMismatch_ : stressFrames_.empty() ? -1
                                                                                       : *stressFrames_.begin();
        run.straightFirst = firstMismatch_;
        run.resimFirst = stressFrames_.empty() ? -1 : *stressFrames_.begin();
        run.straightFrames = mismatchFrames_;
        run.resimFrames = static_cast<int>(stressFrames_.size());
        for (const std::string& r : resimRegions_) run.regions += (run.regions.empty() ? "" : ",") + r;
        run.session = benchSession_;
        if (!failDump_.empty()) {
            ReplayFile d;
            d.modVersion = rf_.modVersion;
            d.gameBuild = game.BuildId();
            d.matchId = rf_.matchId;
            d.localPlayer = rf_.localPlayer;
            d.setup = rf_.setup;
            d.regions = failRegions_;
            d.initialHashes = failHashes_;
            d.dumps.push_back({failDumpFrame_, failDumpLabel_, std::move(failDump_)});
            const std::string name = "benchmark_" + std::to_string(static_cast<long long>(std::time(nullptr))) + "_" + failDumpLabel_ +
                                     "_frame" + std::to_string(failDumpFrame_) + ".sfvr";
            std::string error;
            if (SaveReplay(d, Utf8Path(LogsDir() + L"\\" + std::wstring(name.begin(), name.end())), &error, ReplayDetail::Full)) {
                run.dump = name;
                PruneOldest(LogsDir(), L"benchmark_*.sfvr", 6);
                Log("replay: benchmark state dump %s", name.c_str());
            } else Log("replay: cannot save the benchmark state dump: %s", error.c_str());
            failDump_.clear();
        }
        run.stopped = benchStopped_;
        FinishBenchmark(run);
        std::lock_guard lock(g_viewMutex);
        g_view = ReplayView{};
        g_viewCommands.clear();
        g_viewTaken = 0;
    }
    std::function<void()> leave;
    if (viewer_ && !restarting_) {
        UiSoundMuteGame(false);
        BattleStopCapturedSounds();
        BattleRecordSounds(false);
        std::lock_guard lock(g_viewMutex);
        g_view = ReplayView{};
        g_viewCommands.clear();
        g_viewTaken = 0;
        leave = std::move(g_viewLeave);
    } else if (viewer_) {
        const int stopped = BattleStopSounds();
        Log("replay: the battle's %d sounds stopped for the restart", stopped);
        std::lock_guard lock(g_viewMutex);
        g_view.seeking = true;
        g_view.paused = false;
        g_view.handled += static_cast<uint32_t>(handledNow_);
        handledNow_ = 0;
    }
    const std::string restartPath = path_, restartTitle = title_;
    const RestartPending restartWith{startAt_, paused_, fast_, roundStarts_, roundEnds_, roundCalls_};
    const bool restart = viewer_ && restarting_;
    FlowSetRoundDirectorPerTick(false, false);
    InputIsolatePads(false);
    const int stressDesyncs = static_cast<int>(stressFrames_.size());
    const bool pass = ready_ && firstMismatch_ < 0 && stressDesyncs == 0 && framesChecked_ > last_;
    const std::string mode = stress_ > 0  ? ", stress distance " + std::to_string(stress_)
                             : delay_ > 0 ? ", P" + std::to_string(remote_ + 1) + " " + std::to_string(delay_) +
                                                " frames late (" + std::to_string(rollbacks_) + " rollbacks, " +
                                                std::to_string(resimulated_) + " frames resimulated)"
                                          : "";
    Log("replay: %s %s, %d of %d frames checked%s, %d frames differ from the recording (first %d), "
        "%d presentation clock differences%s",
        name_.c_str(),
        pass ? "PASS" : "FAIL", framesChecked_, last_ + 1, mode.c_str(),
        mismatchFrames_, firstMismatch_, presentationFrames_,
        stress_ > 0 ? (", " + std::to_string(stressDesyncs) + " frames resimulated differently").c_str() : "");
    std::vector<std::pair<int, std::string>> byCount;
    for (const auto& [name, frames] : regionFrames_) byCount.push_back({frames, name});
    std::sort(byCount.rbegin(), byCount.rend());
    std::string top;
    for (size_t i = 0; i < byCount.size() && i < 12; i++) {
        top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
    }
    if (!top.empty()) Log("replay:   regions (frames): %s", top.c_str());
    top.clear();
    byCount.clear();
    for (const auto& [name, frames] : stressRegionFrames_) byCount.push_back({frames, name});
    std::sort(byCount.rbegin(), byCount.rend());
    for (size_t i = 0; i < byCount.size() && i < 12; i++) {
        top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
    }
    if (!top.empty()) Log("replay:   resimulated differently (frames): %s", top.c_str());
    const bool viewer = viewer_ || bench_;
    BattleSetFrameDriver(nullptr);
    delete this;
    if (restart) {
        g_restartPending = restartWith;
        ReplayRequest(restartPath, 0, -1, -1, "", -1, -1, false, "", -1, 0, 0, -1, restartTitle, true);
    } else if (viewer) {
        FlowChangeScene("MainMenuState");
        if (leave) leave();
    }
}

} // namespace replay

} // namespace sfv::mod
