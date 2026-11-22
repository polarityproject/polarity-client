#pragma once
// What the overlay's files share: the render thread's state, fonts, tables, picks and the preview (docs/overlay.md).

#include "mod/overlay.h"

#include "mod/battle_driver.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"
#include "core/random_stage.h"
#include "mod/pak_textures.h"
#include "mod/ui_sound.h"
#include "mod/profile.h"
#include "mod/textures.h"
#include "core/keeper_protocol.h"
#include "core/match_code.h"
#include "mod/config.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/entitlements.h"
#include "mod/files.h"
#include "mod/font_resources.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/select_screen.h"
#include "mod/battle_log_screen.h"
#include "mod/battle_setup.h"
#include "mod/benchmark.h"
#include "mod/replay_driver.h"
#include "mod/fight_ui.h"
#include "core/key_names.h"
#include "mod/hub_screen.h"
#include "mod/ui_kit.h"
#include "mod/rooms_screen.h"
#include "mod/room_screen.h"
#include "mod/watch_chat.h"
#include "mod/watch_panel.h"
#include "mod/settings_screen.h"
#include "core/match_ticket.h"
#include "mod/rooms_client.h"
#include "mod/profile.h"
#include "mod/status.h"
#include "mod/report_upload.h"
#include "mod/updater.h"
#include "mod/weyve_service.h"
#include "core/png.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <xinput.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <random>
#include <memory>
#include <mutex>
#include <sstream>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace sfv::mod::overlay {

constexpr int kPresentSlot = 8;

constexpr int kResizeBuffersSlot = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT syncInterval, UINT flags);

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT count, UINT width, UINT height,
                                                    DXGI_FORMAT format, UINT flags);

inline PresentFn g_origPresent = nullptr;

inline ResizeBuffersFn g_origResizeBuffers = nullptr;

inline void* g_presentTarget = nullptr;

inline void* g_resizeTarget = nullptr;

inline void** g_tablePresentSlot = nullptr;

inline PresentFn g_tablePresentNext = nullptr;

inline thread_local bool t_inTablePresent = false;

inline IDXGISwapChain* g_swapChain = nullptr;

inline ID3D11Device* g_device = nullptr;

inline ID3D11DeviceContext* g_context = nullptr;

inline ID3D11RenderTargetView* g_rtv = nullptr;

inline HWND g_window = nullptr;

inline bool g_imgui = false;

inline std::atomic<bool> g_visible{false};

inline std::atomic<bool> g_hubAllowed{false};

inline std::atomic<bool> g_chordKey{false};

inline std::atomic<bool> g_reloadChoices{false};

inline std::atomic<bool> g_spectating{false};

inline std::atomic<bool> g_watchPanelTap{false};

inline std::atomic<ULONGLONG> g_xinputAt{0};

inline std::atomic<bool> g_voting{false};

inline std::atomic<bool> g_watchHold{false};

inline std::atomic<HWND> g_inputWindow{nullptr};

inline std::string g_build;

std::string BuildVersion();

inline std::string g_restartError;

inline std::atomic<float> g_fps{0};

inline std::atomic<bool> g_inMatch{false};

inline std::string g_packResult;

inline std::atomic<bool> g_noVsync{false};

inline std::atomic<UINT> g_gameSyncInterval{0};

inline std::atomic<UINT> g_gamePresentFlags{0};

inline std::atomic<int64_t> g_presentMicros{0};

inline std::atomic<int64_t> g_presentFrames{0};

struct Message {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};

inline std::mutex g_inputMutex;

inline std::vector<Message> g_input;

inline std::wstring g_panelPath;

struct PadInput {
    std::atomic<uint16_t> buttons{0};
    std::atomic<int16_t> lx{0}, ly{0};
    std::atomic<uint8_t> lt{0}, rt{0};
};

inline PadInput g_pads[XUSER_MAX_COUNT];

inline uint32_t g_keyPad = 0;

void LoadKeyboardMap();

void FeedPads(ImGuiIO& io);

void ReleaseTarget();

bool CreateTarget(IDXGISwapChain* swapChain);

void ReleaseImGui();

const ImVec4 kGold(1.00f, 0.89f, 0.55f, 1.0f);

const ImVec4 kGoldLine(0.85f, 0.72f, 0.36f, 1.0f);

const ImVec4 kCream(0.99f, 0.98f, 0.91f, 1.0f);

const ImVec4 kOlive(0.22f, 0.22f, 0.17f, 1.0f);

const ImVec4 kP1(0.36f, 0.62f, 1.0f, 1.0f);

const ImVec4 kP2(1.0f, 0.36f, 0.30f, 1.0f);

const ImVec4 kRed(1.0f, 0.36f, 0.30f, 1.0f);

const ImVec4 kGreen(0.55f, 0.86f, 0.45f, 1.0f);

const ImVec4 kDim(0.66f, 0.65f, 0.59f, 1.0f);

inline float g_uiScale = 1.0f;

inline float g_builtScale = 0.0f;

inline ImFont* g_fontHeading = nullptr;

inline ImFont* g_fontBig = nullptr;

inline ImFont* g_fontSmall = nullptr;

inline ImFont* g_fontTiny = nullptr;

inline ImFont* g_fontBold = nullptr;

inline ImFont* g_fontHuge = nullptr;

inline ImFont* g_fontBody = nullptr;

inline std::atomic<int> g_selectRequest{-1};

inline std::atomic<bool> g_selectLive{false};

float UiScaleFor(float height);

ImVec4 Rgb(int r, int g, int b, float a = 1.0f);

void ApplyTheme(float scale);

std::pair<void*, int> EmbeddedFont(int id);

const std::vector<uint8_t>& CjkFont();

int CjkFace();

void BuildFonts(float scale);

std::string Upper(std::string text);

ImVec4 PingColorVec(unsigned ms, unsigned jitterMs);

ImVec4 PathColor(const NetView& v);

void DrawLinkIcon(ImDrawList* d, ImVec2 at, float size, lobby::Link link, ImU32 color);

void DrawRelayIcon(ImDrawList* d, ImVec2 at, float size, ImU32 color);

std::string Roman(int n);

int SmoothedFps(int seat, int fps);

bool SetUp(IDXGISwapChain* swapChain);

inline bool g_previewing = false;

void Act(std::function<void()> action);

struct Tables {
    std::vector<std::string> charas;
    std::vector<int> charaIndex;
    std::vector<std::string> labels;
    std::vector<std::vector<int>> costumes;
    std::vector<std::vector<std::string>> costumeNames;
    std::vector<std::vector<std::vector<int>>> colors;
    std::vector<std::string> stages;
    std::vector<std::string> stageLabels;
    std::vector<std::vector<std::string>> settings;
    std::vector<std::vector<std::string>> settingLabels;
    std::vector<std::vector<std::string>> vskills;
    std::vector<std::vector<std::string>> vtriggers;
    std::vector<std::vector<std::string>> vcomments;
    std::vector<int> elevenCostume;
    std::vector<std::vector<CostumeSwatches>> swatches;
    std::vector<bool> charaOwned;
    std::vector<std::vector<bool>> costumeOwned;
    std::vector<std::vector<std::vector<bool>>> colorOwned;
    std::vector<bool> stageOwned;
    std::vector<std::vector<bool>> settingOwned;
    bool loaded = false;
};

inline Tables g_tables;

Tables ReadTables();

inline std::mutex g_tablesMutex;

inline std::unique_ptr<Tables> g_readTables;

inline std::atomic<bool> g_readingTables{false};

inline std::unique_ptr<std::map<std::string, std::vector<std::string>>> g_readComments;

void ReadComments(std::vector<std::string> codes);

void LoadTables();

int CostumeSlot(int chara, int costume);

bool CharaOwned(int chara);

bool CostumeOwned(int chara, int slot);

bool ColorOwnedAt(int chara, int slot, int at);

bool StageOwned(int stage, int setting);

constexpr int kStageRandom = 1, kFirstStage = 2;
constexpr int kDefaultSetting = 0;

struct Choices {
    bool initialised = false;
    bool pick = false;
    int chara = 0;
    int costume = 0;
    int color = 0;
    int vskill = 1;
    int vtrigger = 1;
    int variant = 0;
    int comment = 0;
    int elevenAs = -1;
    int stage = 1;
    int setting = 0;
    int delay = 2;
    char code[64] = "";
    char name[lobby::kMaxName + 1] = "";
    bool allowSpectators = true;
    bool uploadReports = true;
    bool showFps = false;
    bool shareCountry = true;
    bool streamerMode = false;
    bool wordFilter = true;
};

inline Choices g_choices;

inline std::vector<std::string> g_randomOff;

inline bool g_randomOffSaved = false;

bool IsEleven(int chara);

void RollEleven(Choices& c);

PlayerSetup ElevenPick(Choices& c);

bool IsElevenSetup(const PlayerSetup& p);

std::string CommentText(const std::string& code, int comment);

inline std::map<std::string, PlayerSetup> g_loadouts;

void InitChoices();

NetplayOptions ChosenOptions();

std::string CharaLabel(const std::string& code);

std::string SkillText(const std::string& code, bool vtrigger, unsigned index);

std::string ColorLabel(int id);

std::string StageText(uint16_t index);

void SaveChoices();

int VariantsOf(int chara, int costume);

bool IsEleven(int chara);

void RollEleven(Choices& c);

PlayerSetup ElevenPick(Choices& c);

bool IsElevenSetup(const PlayerSetup& p);

bool PickValid(Choices& c);

inline bool g_otherHasPick = false;

inline PlayerSetup g_otherPick;

int TakenColor(const Choices& c);

void AvoidTakenColor(Choices& c);

void SwitchCharacter(Choices& c, int chara);

std::string CommentText(const std::string& code, int comment);

constexpr int kMaxDelayFrames = 10;

std::string FramesText(int n);

void SyncChoices(const NetView& v);

std::string ShortCharaName(const std::string& code);

struct Preview {
    bool hud = false;
    ULONGLONG fadeAt = 0;
    bool strip = false;
    bool desync = false;
    bool side[kNumPlayers] = {false, false};
    NetView::Phase sidesPhase = NetView::Phase::PostMatch;
    int you = 0;
    lobby::Choice choice[kNumPlayers] = {lobby::Choice::None, lobby::Choice::Rematch};
    bool ready[kNumPlayers] = {false, true};
    bool relayed = true;
    bool set = false;

    bool Any() const { return hud || strip || desync || side[0] || side[1]; }
};

inline Preview g_preview;

inline bool g_sampleToasts = false;

inline std::mutex g_textureMutex;

inline ID3D11ShaderResourceView* g_texture = nullptr;

inline uint32_t g_textureW = 0, g_textureH = 0;

inline std::string g_textureLabel;

void DrawTexturePreview();

SelectData PreviewSelectData();

int ElevenIndex();

std::pair<uint32_t, uint32_t> SwatchOf(int chara, int slot, int color);

std::string CommentLine(int chara, int comment);

SelectSide SelectSideFor(const NetView& v, int i);

int SelectStageFor(const NetView& v, int* setting);

SelectLive SelectLiveFor(const NetView& v);

void StepRow(Choices& c, int row, int delta);

struct SentSelect {
    int screen = -1;
    uint8_t row = 0;
    uint32_t hover = 0;
};

inline SentSelect g_sentSelect;

void ApplySelectInput(const SelectInput& in, const NetView& v);

void DrawStrip(const NetView& v);

struct Toast {
    std::string text;
    ImVec4 color;
    ULONGLONG until;
    ULONGLONG at = 0;
};

inline std::vector<Toast> g_toasts;

void AddToast(const std::string& text, ImVec4 color = ImVec4(1, 1, 1, 1), int seconds = 4);

bool OnMenuScreen();

inline std::atomic<bool> g_migrated{false};

void ToastMigrated();

void ToastReports();

void ToastUpdate();

void ToastChanges(const NetView& now);

Notice DesyncNotice(const NetView& v);

NetView PreviewView(NetView::Phase phase);

void DrawNotices(bool fight, const NetView& v);

constexpr ULONGLONG kBarsFadeMs = 800;

void DrawPlayerBars(const NetView& v);

const char* VoteText(lobby::Choice c);

void DrawSides(const NetView& v, const bool (&show)[kNumPlayers] = {true, true});

NetView PreviewView(NetView::Phase phase);

inline std::mutex g_previewMutex;

inline std::string g_previewWords;

void UseServer(const std::string& chosen);

std::vector<std::string> ServerChoices();

std::string ServerLabel(const std::string& server);

std::vector<SettingsRow> SettingsRows();

enum class BenchModal { None,
                        Confirm,
                        Loading,
                        Result };

inline BenchModal g_benchModal = BenchModal::None;

inline int g_benchChoice = 0;

inline BenchmarkResult g_benchShown;

inline bool g_benchReturn = false;

enum class BenchStep { Reading,
                       Loading,
                       Starting,
                       Fading };

struct BenchLaunch {
    bool active = false;
    BenchStep step = BenchStep::Reading;
    int drawn = 0;
    uint64_t switchesAt = 0;
    double since = 0, stepSince = 0, blackFrom = -1, revealFrom = -1;
};

inline BenchLaunch g_benchLaunch;

constexpr double kBenchToBlack = 0.3, kBenchReveal = 0.5, kBenchGiveUp = 60;

void OpenBenchmarkModal(bool result);

void StartBenchmarkRun();

void StepBenchmarkLaunch();

void DrawBenchmarkLaunch(const HubFonts& f);

struct BenchDataLine {
    int kind = -1;
    std::string head, body;
};

BenchDataLine BenchmarkDataLine(const BenchmarkResult& b);

void DrawBenchmarkModal(const HubFonts& f, bool takeInput);

void ApplySetting(const SettingsInput& in);

inline std::mutex g_favoritesMutex;

inline ProfileFavorites g_favorites;

inline int g_tableDelay = -1;

inline bool g_myCharacterOpen = false;

void ApplyProfileFavorite(Choices& c);

inline bool g_backToRooms = false;

inline int g_watchTable = -1;

enum class RoomTraining { Off,
                          Asked,
                          In };

inline RoomTraining g_roomTraining = RoomTraining::Off;

inline double g_roomTrainingAt = 0;

bool InTrainingScene();

void StartRoomTraining(const RoomsView& rooms);

void StepRoomTraining();

void StartTableWatch(const WatchTicket& w);

void StartTableSession(const keeper::TableTicket& t);

struct MyCharacter {
    Choices saved;
    bool fromProfile = false;
    bool stagePage = false;
    int stageBefore = 1, settingBefore = 0;
    bool savedNow = false;
    int8_t delaySetting = kDelayAuto;
    int openedFrame = 0;
    float alpha = 0;
};

inline MyCharacter g_my;

void ApplyProfileFavorite(Choices& c);

void OpenMyCharacter();

void CloseMyCharacter(bool keep, const char* why);

void SaveMyCharacter();

void SetRandomOff(std::vector<std::string> off, const char* why);

std::string RandomStagesText();

SelectCard MyCharacterCard(const RoomsView& rooms);

void ApplyMyCharacterInput(const SelectInput& in);

void DrawMyCharacter(const RoomsView& rooms, bool takeInput);

inline bool g_roomsWhenReady = false;

void DrawHubScreens(bool takeInput);

enum class HubScreen { None,
                       Hub,
                       RoomBrowser,
                       Room,
                       Settings,
                       MyCharacter };
inline HubScreen g_hubScreen = HubScreen::None;

void DrawHubFrame(bool takeInput);

CardPick TablePick(const NetplayOptions& o, const std::string& favorite);

void SetTableSkills(int vskill, int vtrigger);

inline std::string g_nameSeen;

inline bool g_nameChanged = false;

inline std::vector<std::string> g_createRules;

void OpenHubRooms(const NetplayOptions& o);

inline std::atomic<bool> g_reopenLog{false};

struct LogLaunch {
    bool active = false;
    uint64_t switchesAt = 0;
    double since = 0, blackFrom = -1;
};

inline LogLaunch g_logLaunch;

constexpr double kLaunchToBlack = 0.3, kLaunchBlackHeld = 0.6, kLaunchGiveUp = 45;

std::string StageLabelOf(uint16_t stage);

void DrawHubScreens(bool takeInput);

bool OnMenuScreen();

void DrawMenuHint();

void DrawPreviews();

void Draw(IDXGISwapChain* swapChain);

void TakeScreenshot(IDXGISwapChain* swapChain);

HRESULT DrawAndPresent(PresentFn next, IDXGISwapChain* self, UINT syncInterval, UINT flags);

inline std::mutex g_shotMutex;

inline std::string g_shotPath;

inline int g_shotScale = 1;

std::wstring Widen(const std::string& s);

bool ReadBack(IDXGISwapChain* swapChain, int scale, int* outW, int* outH, std::vector<uint8_t>* outRgb);

bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb);

struct BurstFrame {
    uint32_t tick;
    int w, h;
    std::vector<uint8_t> rgb;
};

inline std::string g_burstDir;

inline int g_burstLeft = 0, g_burstScale = 2, g_burstIndex = 0;

inline std::vector<BurstFrame> g_burstFrames;

void TakeBurstFrame(IDXGISwapChain* swapChain);

void TakeScreenshot(IDXGISwapChain* swapChain);

bool WriteTableSlot(void** slot, void* value);

HRESULT STDMETHODCALLTYPE TablePresentDetour(IDXGISwapChain* self, UINT syncInterval, UINT flags);

void HookPresentTable(IDXGISwapChain* self);

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* self, UINT syncInterval, UINT flags);

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);

bool FindSwapChainFunctions();

} // namespace sfv::mod::overlay
