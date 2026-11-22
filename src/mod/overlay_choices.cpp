#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

Tables ReadTables() {
    Tables t;
    for (int i = 0; i < FlowCharaCount() && !Stopping(); i++) {
        const std::string name = FlowCharaName(i);
        if (!BattleSetupIsFighter(i) || name.empty()) continue;
        std::vector<int> costumes;
        std::vector<std::string> names;
        std::vector<std::vector<int>> colors;
        std::vector<bool> costumeOwned;
        std::vector<std::vector<bool>> colorOwned;
        const std::map<int, CostumeSpec> specs = FlowCharaCostumeSpecs(i);
        std::string charaCode;
        for (const auto& [k, spec] : specs) {
            if (charaCode.empty()) charaCode = spec.charaCode;
        }
        for (int k = 0; k < FlowCharaCostumeCount(i); k++) {
            const int n = FlowCostumeColorCount(i, k);
            if (n <= 0) continue;
            costumes.push_back(k);
            const auto spec = specs.find(k);
            const bool named = spec != specs.end() && !spec->second.name.empty() && spec->second.name.rfind("ID_", 0) != 0;
            names.push_back(named ? spec->second.name : "Costume " + std::to_string(costumes.size()));
            std::vector<int> ids = spec != specs.end() ? spec->second.colors : std::vector<int>{};
            if (ids.empty()) {
                for (int c = 0; c < n; c++) ids.push_back(c);
            }
            costumeOwned.push_back(spec == specs.end() || EntitlementsOwns(spec->second.costumeCode));
            std::vector<bool> owned;
            for (int id : ids) {
                bool has = true;
                if (spec != specs.end()) {
                    const auto code = spec->second.colorCodes.find(id);
                    has = code == spec->second.colorCodes.end() || EntitlementsOwns(code->second);
                }
                owned.push_back(has);
            }
            colorOwned.push_back(std::move(owned));
            colors.push_back(std::move(ids));
        }
        if (costumes.empty()) continue;
        t.charaOwned.push_back(EntitlementsOwns(charaCode));
        t.costumeOwned.push_back(std::move(costumeOwned));
        t.colorOwned.push_back(std::move(colorOwned));
        std::vector<CostumeSwatches> swatches;
        for (int k : costumes) swatches.push_back(FlowCostumeSwatchColors(i, k));
        t.swatches.push_back(std::move(swatches));
        t.vcomments.emplace_back();
        t.elevenCostume.push_back(FlowCostumeInFolder(i, "0C"));
        t.charas.push_back(FlowCharaCode(i));
        t.charaIndex.push_back(i);
        t.labels.push_back(name);
        t.costumes.push_back(std::move(costumes));
        t.costumeNames.push_back(std::move(names));
        t.colors.push_back(std::move(colors));
        t.vskills.push_back(FlowSkillNames(FlowCharaCode(i), false));
        t.vtriggers.push_back(FlowSkillNames(FlowCharaCode(i), true));
    }
    std::vector<size_t> order(t.charas.size());
    for (size_t k = 0; k < order.size(); k++) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return std::lexicographical_compare(t.labels[a].begin(), t.labels[a].end(), t.labels[b].begin(), t.labels[b].end(),
                                            [](char x, char y) {
                                                return std::tolower(static_cast<unsigned char>(x)) <
                                                       std::tolower(static_cast<unsigned char>(y));
                                            });
    });
    auto reorder = [&order](auto& v) {
        auto sorted = v;
        for (size_t k = 0; k < order.size(); k++) sorted[k] = std::move(v[order[k]]);
        v = std::move(sorted);
    };
    reorder(t.charas);
    reorder(t.charaIndex);
    reorder(t.labels);
    reorder(t.costumes);
    reorder(t.costumeNames);
    reorder(t.colors);
    reorder(t.vskills);
    reorder(t.vtriggers);
    reorder(t.vcomments);
    reorder(t.elevenCostume);
    reorder(t.swatches);
    reorder(t.charaOwned);
    reorder(t.costumeOwned);
    reorder(t.colorOwned);
    if (const int eleven = FlowCharaIndex("ELV"); eleven >= 0) {
        t.charas.push_back("ELV");
        t.charaIndex.push_back(eleven);
        t.labels.push_back("ELEVEN");
        t.costumes.push_back({0});
        t.costumeNames.push_back({"Silver"});
        t.colors.push_back({{0}});
        t.vskills.emplace_back();
        t.vtriggers.emplace_back();
        t.vcomments.emplace_back();
        t.elevenCostume.push_back(-1);
        t.swatches.push_back({CostumeSwatches{}});
        t.charaOwned.push_back(true);
        t.costumeOwned.push_back({true});
        t.colorOwned.push_back({{true}});
    }
    for (int i = 0; i < FlowStageCount() && !Stopping(); i++) {
        if (!FlowIsMenuStage(i)) continue;
        t.stages.push_back(FlowStageCode(i));
        const std::string name = FlowStageName(i);
        t.stageLabels.push_back(name.empty() ? FlowStageCode(i) : name);
        t.stageOwned.push_back(EntitlementsOwns(FlowStageAvailableCode(i)));
        std::vector<std::string> codes, names;
        std::vector<bool> owned;
        for (int k : FlowStageSettings(i)) {
            if (!FlowIsPlayableStage(k)) continue;
            codes.push_back(FlowStageCode(k));
            names.push_back(FlowStageName(k).empty() ? FlowStageCode(k) : FlowStageName(k));
            owned.push_back(EntitlementsOwns(FlowStageAvailableCode(k)));
        }
        t.settings.push_back(std::move(codes));
        t.settingLabels.push_back(std::move(names));
        t.settingOwned.push_back(std::move(owned));
    }
    t.loaded = !t.charas.empty() && !t.stages.empty();
    if (t.loaded) {
        int charas = 0, stages = 0, costumes = 0, costumesOwned = 0, colors = 0, colorsOwned = 0;
        std::string lockedCharas, lockedStages;
        for (size_t i = 0; i < t.charas.size(); i++) {
            charas += t.charaOwned[i];
            if (!t.charaOwned[i]) lockedCharas += " " + t.charas[i];
            for (bool b : t.costumeOwned[i]) costumes++, costumesOwned += b;
            for (const auto& v : t.colorOwned[i]) {
                for (bool b : v) colors++, colorsOwned += b;
            }
        }
        for (size_t i = 0; i < t.stages.size(); i++) {
            stages += t.stageOwned[i];
            if (!t.stageOwned[i]) lockedStages += " " + t.stages[i];
        }
        Log("overlay: owned (%s): %d of %zu characters, %d of %d costumes, %d of %d colors, %d of %zu stages; locked:%s; stages:%s",
            EntitlementsKnown() ? "the profile's list" : "no list read: all open", charas, t.charas.size(), costumesOwned, costumes,
            colorsOwned, colors, stages, t.stages.size(), lockedCharas.empty() ? " none" : lockedCharas.c_str(),
            lockedStages.empty() ? " none" : lockedStages.c_str());
    }
    return t;
}

void ReadComments(std::vector<std::string> codes) {
    Act([codes = std::move(codes)] {
        const ULONGLONG from = GetTickCount64();
        auto out = std::make_unique<std::map<std::string, std::vector<std::string>>>();
        for (const std::string& code : codes) {
            if (Stopping()) return;
            if (code != "ELV") (*out)[code] = FlowVCommentNames(code);
        }
        Log("overlay: V-Comment names read in %llu ms", GetTickCount64() - from);
        std::lock_guard lock(g_tablesMutex);
        g_readComments = std::move(out);
    });
}

void LoadTables() {
    if (g_tables.loaded) {
        std::lock_guard lock(g_tablesMutex);
        if (g_readComments) {
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.vcomments.size(); i++) {
                const auto it = g_readComments->find(g_tables.charas[i]);
                if (it != g_readComments->end()) g_tables.vcomments[i] = it->second;
            }
            g_readComments.reset();
        }
        return;
    }
    const std::string scene = FlowCurrentScene();
    if (scene.empty() || scene == "BootState" || scene == "InitialState") return;
    if (!FlowStageNamesReady()) return;
    {
        std::lock_guard lock(g_tablesMutex);
        if (g_readTables) {
            if (g_readTables->loaded) {
                g_tables = std::move(*g_readTables);
                Log("overlay: character lists read: %zu characters, %zu stages (scene %s)", g_tables.charas.size(),
                    g_tables.stages.size(), FlowCurrentScene().c_str());
                ReadComments(g_tables.charas);
                FlowLoungeCommentText(0);
            }
            g_readTables.reset();
            g_readingTables = false;
            return;
        }
    }
    if (g_readingTables.exchange(true)) return;
    Act([] {
        static int tries = 0;
        const ULONGLONG from = GetTickCount64();
        auto t = std::make_unique<Tables>(ReadTables());
        tries++;
        Log("overlay: character lists %s in %llu ms (try %d, scene %s)", t->loaded ? "read" : "not there yet", GetTickCount64() - from, tries,
            FlowCurrentScene().c_str());
        if (!t->loaded) Sleep(1000);
        std::lock_guard lock(g_tablesMutex);
        g_readTables = std::move(t);
    });
}

int CostumeSlot(int chara, int costume) {
    const std::vector<int>& costumes = g_tables.costumes[chara];
    const auto it = std::find(costumes.begin(), costumes.end(), costume);
    return it == costumes.end() ? -1 : static_cast<int>(it - costumes.begin());
}

bool CharaOwned(int chara) {
    return chara >= 0 && chara < static_cast<int>(g_tables.charaOwned.size()) && g_tables.charaOwned[chara];
}

bool CostumeOwned(int chara, int slot) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.costumeOwned.size())) return false;
    const auto& v = g_tables.costumeOwned[chara];
    return slot >= 0 && slot < static_cast<int>(v.size()) && v[slot];
}

bool ColorOwnedAt(int chara, int slot, int at) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.colorOwned.size())) return false;
    const auto& costumes = g_tables.colorOwned[chara];
    if (slot < 0 || slot >= static_cast<int>(costumes.size())) return false;
    return at >= 0 && at < static_cast<int>(costumes[slot].size()) && costumes[slot][at];
}

bool StageOwned(int stage, int setting) {
    if (stage < 0 || stage >= static_cast<int>(g_tables.stageOwned.size())) return false;
    if (setting <= 0) return g_tables.stageOwned[stage];
    const auto& s = g_tables.settingOwned[stage];
    return setting - 1 < static_cast<int>(s.size()) && s[setting - 1];
}

void InitChoices() {
    if (g_choices.initialised || !g_tables.loaded) return;
    if (!g_panelPath.empty()) g_loadouts = ConfigReadLoadouts(g_panelPath);
    const NetplayOptions o = NetOptions();
    g_choices.pick = true;
    if (o.hasPick) {
        const std::string code = UnpackCode(o.pick.character);
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
        if (it != g_tables.charas.end()) g_choices.chara = static_cast<int>(it - g_tables.charas.begin());
        g_choices.costume = o.pick.costume;
        g_choices.color = o.pick.color;
        g_choices.vskill = o.pick.vskill + 1;
        g_choices.vtrigger = o.pick.vtrigger + 1;
        g_choices.variant = o.pick.variation;
        g_choices.comment = o.pick.comment;
    }
    g_choices.stage = kStageRandom;
    for (int i = 0; i < static_cast<int>(g_tables.stages.size()) && !o.stageCode.empty(); i++) {
        if (g_tables.stages[i] == o.stageCode) g_choices.stage = kFirstStage + i;
        const auto& settings = g_tables.settings[i];
        const auto it = std::find(settings.begin(), settings.end(), o.stageCode);
        if (it != settings.end()) {
            g_choices.stage = kFirstStage + i;
            g_choices.setting = 1 + static_cast<int>(it - settings.begin());
        }
    }
    g_choices.delay = o.inputDelay;
    std::snprintf(g_choices.name, sizeof(g_choices.name), "%s", o.name.c_str());
    g_choices.allowSpectators = o.allowSpectators;
    g_choices.uploadReports = o.uploadReports;
    g_choices.showFps = o.showOwnFps;
    g_choices.shareCountry = o.shareCountry;
    g_choices.streamerMode = o.streamerMode;
    g_choices.wordFilter = o.wordFilter;
    if (!g_randomOffSaved) {
        g_randomOff = o.randomOff;
        g_randomOffSaved = o.randomOffSaved;
    }
    g_choices.initialised = true;
}

NetplayOptions ChosenOptions() {
    NetplayOptions o = NetOptions();
    const Choices& c = g_choices;
    o.hasPick = c.pick && c.chara < static_cast<int>(g_tables.charas.size());
    if (o.hasPick) {
        o.pick.character = PackCode(g_tables.charas[c.chara].c_str());
        o.pick.costume = static_cast<uint16_t>(c.costume);
        o.pick.color = static_cast<uint8_t>(c.color);
        o.pick.vskill = static_cast<uint8_t>(c.vskill - 1);
        o.pick.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
        o.pick.variation = static_cast<uint8_t>(c.variant);
        o.pick.comment = static_cast<uint8_t>(c.comment);
        if (IsEleven(c.chara)) o.pick = ElevenPick(g_choices);
    }
    o.stage = kKeepStage;
    o.stageCode = "random";
    if (g_randomOffSaved) {
        o.randomOff = g_randomOff;
        o.randomOffSaved = true;
    }
    if (c.stage >= kFirstStage && c.stage - kFirstStage < static_cast<int>(g_tables.stages.size())) {
        const int i = c.stage - kFirstStage;
        const auto& settings = g_tables.settings[i];
        o.stageCode = c.setting > 0 && c.setting <= static_cast<int>(settings.size()) ? settings[c.setting - 1] : g_tables.stages[i];
    }
    o.inputDelay = static_cast<uint8_t>(c.delay);
    o.name = c.name;
    o.allowSpectators = c.allowSpectators;
    o.uploadReports = c.uploadReports;
    o.showOwnFps = c.showFps;
    o.shareCountry = c.shareCountry;
    o.streamerMode = c.streamerMode;
    o.wordFilter = c.wordFilter;
    return o;
}

std::string CharaLabel(const std::string& code) {
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    return it == g_tables.charas.end() ? code : g_tables.labels[it - g_tables.charas.begin()];
}

std::string SkillText(const std::string& code, bool vtrigger, unsigned index) {
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it != g_tables.charas.end()) {
        const auto& names = (vtrigger ? g_tables.vtriggers : g_tables.vskills)[it - g_tables.charas.begin()];
        if (index < names.size() && !names[index].empty()) return Roman(static_cast<int>(index) + 1) + " " + names[index];
    }
    return Roman(static_cast<int>(index) + 1);
}

std::string ColorLabel(int id) {
    return id >= 100 ? "EX Color " + std::to_string(id - 99) : "Color " + std::to_string(id + 1);
}

std::string StageText(uint16_t index) {
    const std::string code = FlowStageCode(index);
    const auto it = std::find(g_tables.stages.begin(), g_tables.stages.end(), code);
    if (code.empty()) return "as prepared";
    return it == g_tables.stages.end() ? code : g_tables.stageLabels[it - g_tables.stages.begin()];
}

void SaveChoices() {
    const NetplayOptions o = ChosenOptions();
    Act([o] {
        NetOptions().name = o.name;
        NetOptions().allowSpectators = o.allowSpectators;
        NetOptions().uploadReports = o.uploadReports;
        ReportsSetOn(o.uploadReports);
        NetOptions().showOwnFps = o.showOwnFps;
        NetOptions().shareCountry = o.shareCountry;
        WeyveService::SetHideCountry(!o.shareCountry);
        NetOptions().streamerMode = o.streamerMode;
        RoomsScreenSetStreamerMode(o.streamerMode);
        NetOptions().wordFilter = o.wordFilter;
        RoomsSetWordFilter(o.wordFilter);
        NetOptions().inputDelay = o.inputDelay;
        if (!g_panelPath.empty()) ConfigSaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

int VariantsOf(int chara, int costume) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.charaIndex.size())) return 0;
    const std::optional<uint8_t> variants = FlowCostumeVariantsKnown(g_tables.charaIndex[chara], costume);
    return variants ? *variants : -1;
}

bool IsEleven(int chara) { return chara >= 0 && chara < static_cast<int>(g_tables.charas.size()) && g_tables.charas[chara] == "ELV"; }

void RollEleven(Choices& c) {
    static std::mt19937 rng{std::random_device{}()};
    std::vector<int> fighters;
    for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
        if (g_tables.elevenCostume[i] >= 0 && i != c.elevenAs) fighters.push_back(i);
    }
    c.elevenAs = fighters.empty() ? -1 : fighters[rng() % fighters.size()];
    c.vskill = 1 + static_cast<int>(rng() % 2);
    c.vtrigger = 1 + static_cast<int>(rng() % 2);
}

PlayerSetup ElevenPick(Choices& c) {
    if (c.elevenAs < 0 || c.elevenAs >= static_cast<int>(g_tables.charas.size())) RollEleven(c);
    PlayerSetup p;
    if (c.elevenAs < 0) return p;
    p.character = PackCode(g_tables.charas[c.elevenAs].c_str());
    p.costume = static_cast<uint16_t>(g_tables.elevenCostume[c.elevenAs]);
    p.vskill = static_cast<uint8_t>(c.vskill - 1);
    p.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
    return p;
}

bool IsElevenSetup(const PlayerSetup& p) {
    const std::string code = UnpackCode(p.character);
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it == g_tables.charas.end()) return false;
    const int costume = g_tables.elevenCostume[it - g_tables.charas.begin()];
    return costume >= 0 && p.costume == costume;
}

bool PickValid(Choices& c) {
    if (!g_tables.loaded || !c.initialised) return false;
    c.chara = std::clamp(c.chara, 0, static_cast<int>(g_tables.charas.size()) - 1);
    if (!CharaOwned(c.chara)) {
        for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
            if (CharaOwned(i) && !IsEleven(i)) {
                c.chara = i;
                break;
            }
        }
    }
    int slot = CostumeSlot(c.chara, c.costume);
    if (slot < 0 || !CostumeOwned(c.chara, slot)) {
        slot = 0;
        for (int k = 0; k < static_cast<int>(g_tables.costumes[c.chara].size()); k++) {
            if (CostumeOwned(c.chara, k)) {
                slot = k;
                break;
            }
        }
        c.costume = g_tables.costumes[c.chara][slot];
    }
    const std::vector<int>& colors = g_tables.colors[c.chara][slot];
    const auto at = std::find(colors.begin(), colors.end(), c.color);
    if (at == colors.end() || !ColorOwnedAt(c.chara, slot, static_cast<int>(at - colors.begin()))) {
        c.color = colors[0];
        for (int k = 0; k < static_cast<int>(colors.size()); k++) {
            if (ColorOwnedAt(c.chara, slot, k)) {
                c.color = colors[k];
                break;
            }
        }
    }
    if (c.stage >= kFirstStage && !StageOwned(c.stage - kFirstStage, c.setting)) {
        if (StageOwned(c.stage - kFirstStage, kDefaultSetting)) {
            c.setting = kDefaultSetting;
        } else {
            c.stage = kStageRandom;
            c.setting = kDefaultSetting;
        }
    }
    return true;
}

int TakenColor(const Choices& c) {
    if (!g_otherHasPick || !g_tables.loaded || c.chara < 0 || c.chara >= static_cast<int>(g_tables.charas.size()) || IsEleven(c.chara)) {
        return -1;
    }
    if (g_otherPick.character != PackCode(g_tables.charas[c.chara].c_str()) || g_otherPick.costume != c.costume) return -1;
    return g_otherPick.color;
}

void AvoidTakenColor(Choices& c) {
    const int taken = TakenColor(c);
    if (taken < 0 || c.color != taken) return;
    const std::vector<int>& ids = g_tables.colors[c.chara][std::max(0, CostumeSlot(c.chara, c.costume))];
    const int n = static_cast<int>(ids.size());
    const int at = static_cast<int>(std::find(ids.begin(), ids.end(), c.color) - ids.begin());
    const int slot = std::max(0, CostumeSlot(c.chara, c.costume));
    for (int k = 1; n > 1 && at < n && k < n; k++) {
        const int next = (at + k) % n;
        if (ColorOwnedAt(c.chara, slot, next)) {
            c.color = ids[next];
            break;
        }
    }
}

void SwitchCharacter(Choices& c, int chara) {
    if (chara == c.chara || !CharaOwned(chara)) return;
    PlayerSetup& left = g_loadouts[g_tables.charas[c.chara]];
    left.costume = static_cast<uint16_t>(c.costume);
    left.color = static_cast<uint8_t>(c.color);
    left.vskill = static_cast<uint8_t>(c.vskill - 1);
    left.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
    left.variation = static_cast<uint8_t>(c.variant);
    left.comment = static_cast<uint8_t>(c.comment);
    c.chara = chara;
    if (IsEleven(chara)) RollEleven(c);
    const auto it = g_loadouts.find(g_tables.charas[chara]);
    const PlayerSetup saved = it != g_loadouts.end() ? it->second : PlayerSetup{};
    c.costume = saved.costume;
    c.color = saved.color;
    c.vskill = saved.vskill + 1;
    c.vtrigger = saved.vtrigger + 1;
    c.variant = saved.variation;
    c.comment = saved.comment;
    PickValid(c);
    AvoidTakenColor(c);
}

std::string CommentText(const std::string& code, int comment) {
    if (comment == kCommentAuto) return "Auto";
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it == g_tables.charas.end()) return "Comment " + std::to_string(comment);
    return "Comment " + std::to_string(comment);
}

std::string FramesText(int n) { return std::to_string(n) + (n == 1 ? " frame" : " frames"); }

void SyncChoices(const NetView& v) {
    if (!g_tables.loaded || !g_choices.initialised || g_previewing) return;
    static bool sentHas = false;
    static PlayerSetup sent;
    static std::string sentStage;
    static int sentDelay = -1;
    NetplayOptions o = ChosenOptions();
    if (const int variants = VariantsOf(g_choices.chara, g_choices.costume); variants < 0 || (o.pick.variation & variants) != o.pick.variation) {
        o.pick.variation = 0;
    }
    const bool pickChanged = o.hasPick != sentHas || !(o.pick == sent);
    const bool stageChanged = v.host && o.stageCode != sentStage;
    const bool delayChanged = o.inputDelay != sentDelay;
    if (!pickChanged && !stageChanged && !delayChanged) return;
    sentHas = o.hasPick;
    sent = o.pick;
    if (v.host) sentStage = o.stageCode;
    sentDelay = o.inputDelay;
    Act([o, pickChanged, stageChanged, delayChanged] {
        if (pickChanged) NetSetPick(o.hasPick, o.pick);
        if (stageChanged) NetSetStage(o.stageCode);
        if (delayChanged) NetSetDelay(o.inputDelay);
        if (!g_panelPath.empty()) ConfigSaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

std::string ShortCharaName(const std::string& code) { return CharaLabel(code); }

void DrawTexturePreview() {
    std::lock_guard lock(g_textureMutex);
    if (!g_texture) return;
    const float scale = std::min(1.0f, 512.0f * g_uiScale / std::max<uint32_t>(1, std::max(g_textureW, g_textureH)));
    ImGui::SetNextWindowPos(ImVec2(20 * g_uiScale, 20 * g_uiScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Texture##research", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("%s  %ux%u", g_textureLabel.c_str(), g_textureW, g_textureH);
        ImGui::Image(reinterpret_cast<ImTextureID>(g_texture), ImVec2(g_textureW * scale, g_textureH * scale));
    }
    ImGui::End();
}

SelectData PreviewSelectData() {
    SelectData data;
    for (size_t i = 0; i < g_tables.charas.size(); i++) {
        SelectChara c;
        c.code = g_tables.charas[i];
        c.name = g_tables.labels[i];
        c.costumes = g_tables.costumeNames[i];
        for (const CostumeSwatches& sw : g_tables.swatches[i]) {
            std::vector<std::pair<uint32_t, uint32_t>> both = sw.colors;
            both.insert(both.end(), sw.ex.begin(), sw.ex.end());
            c.swatches.push_back(std::move(both));
        }
        c.vskills = g_tables.vskills[i];
        c.vtriggers = g_tables.vtriggers[i];
        c.vcomments = g_tables.vcomments[i];
        c.locked = !CharaOwned(static_cast<int>(i));
        data.charas.push_back(std::move(c));
    }
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        SelectStage stage{g_tables.stages[i], g_tables.stageLabels[i], {}};
        stage.locked = !StageOwned(static_cast<int>(i), 0);
        for (size_t k = 0; k < g_tables.settings[i].size(); k++) {
            SelectStage setting{g_tables.settings[i][k], g_tables.settingLabels[i][k], {}};
            setting.locked = !StageOwned(static_cast<int>(i), static_cast<int>(k) + 1);
            stage.settings.push_back(std::move(setting));
        }
        data.stages.push_back(std::move(stage));
    }
    data.you = g_choices.name;
    return data;
}

} // namespace sfv::mod::overlay
