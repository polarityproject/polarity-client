#pragma once

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

inline thread_local bool t_inTablePresent = false; // the code hook below passes the frame on

inline IDXGISwapChain* g_swapChain = nullptr; // the one ImGui was set up for

inline ID3D11Device* g_device = nullptr;

inline ID3D11DeviceContext* g_context = nullptr;

inline ID3D11RenderTargetView* g_rtv = nullptr;

inline HWND g_window = nullptr;

inline bool g_imgui = false;

inline std::atomic<bool> g_visible{false};

inline std::atomic<bool> g_hubAllowed{false}; // at the game's menus with no session: Back + Start opens the hub

inline std::atomic<bool> g_chordKey{false}; // F1, the keyboard's Back + Start (window thread -> FeedPads)

inline std::atomic<bool> g_reloadChoices{false}; // OverlayReloadChoices: read the choices again

inline std::atomic<bool> g_spectating{false}; // a spectator's session: a Start tap opens the watch panel (M5)

inline std::atomic<bool> g_watchPanelTap{false}; // ... tapped (the frame toggles the panel)

inline std::atomic<ULONGLONG> g_xinputAt{0}; // when the XInput hook last saw a pad (OverlayPad)

inline std::atomic<bool> g_voting{false}; // the post-match vote is up and takes input

inline std::atomic<bool> g_watchHold{false};

inline std::atomic<HWND> g_inputWindow{nullptr}; // the window ImGui reads input for

inline std::string g_build; // the mod's version (OverlaySetBuild)

std::string BuildVersion();

inline std::string g_restartError; // the update modal's last failed restart, why

inline std::atomic<float> g_fps{0}; // presented frames per second (OverlayRenderFps)

inline std::atomic<bool> g_inMatch{false}; // a netplay match runs (the frame-drop log, Start held for the panel)

inline std::string g_packResult; // render thread: what the last "Pack logs to share" did

inline std::atomic<bool> g_noVsync{false}; // present without waiting for the display's refresh

inline std::atomic<UINT> g_gameSyncInterval{0}; // the sync interval and flags the game presents with (OverlayVsyncInfo)

inline std::atomic<UINT> g_gamePresentFlags{0};

inline std::atomic<int64_t> g_presentMicros{0}; // ... time in Present, summed with the frames (OverlayVsyncInfo)

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

inline float g_uiScale = 1.0f; // sizes: 1 at 720 lines (render thread)

inline float g_builtScale = 0.0f; // what the fonts were made for

inline ImFont* g_fontHeading = nullptr; // bold italic, as the menus' entries

inline ImFont* g_fontBig = nullptr; // match codes

inline ImFont* g_fontSmall = nullptr; // the player bars' details

inline ImFont* g_fontTiny = nullptr; // ... their frame rate and rollback numbers

inline ImFont* g_fontBold = nullptr; // Barlow Bold: the screens' names, buttons, focused rows

inline ImFont* g_fontHuge = nullptr; // the select screen's character names

inline ImFont* g_fontBody = nullptr; // the default font

inline std::atomic<int> g_selectRequest{-1}; // OverlayRequestSelectPreview: the phase to open at (+4: watching)

inline std::atomic<bool> g_selectLive{false}; // a session's select screen is up (or held over the jump) and takes input

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
    std::vector<std::string> charas; // playable characters
    std::vector<int> charaIndex;     // each one's index in the game's character table
    std::vector<std::string> labels; // "CHUN-LI"
    std::vector<std::vector<int>> costumes;
    std::vector<std::vector<std::string>> costumeNames;
    std::vector<std::vector<std::vector<int>>> colors;
    std::vector<std::string> stages;                     // the stage select's stages
    std::vector<std::string> stageLabels;                // "Bustling Side Street"
    std::vector<std::vector<std::string>> settings;      // per stage: its settings' stage codes (The Grid: S25)
    std::vector<std::vector<std::string>> settingLabels; // their names ("The Grid Alternative")
    std::vector<std::vector<std::string>> vskills;       // per character, the game's names
    std::vector<std::vector<std::string>> vtriggers;
    std::vector<std::vector<std::string>> vcomments;    // the V-Comments after Auto
    std::vector<int> elevenCostume;                     // the 0C costume Eleven wears in each fighter (-1: none)
    std::vector<std::vector<CostumeSwatches>> swatches; // per costume (as listed): the colors' swatches
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

inline std::unique_ptr<Tables> g_readTables; // from the worker, taken by the render thread

inline std::atomic<bool> g_readingTables{false};

inline std::unique_ptr<std::map<std::string, std::vector<std::string>>> g_readComments;

void ReadComments(std::vector<std::string> codes);

void LoadTables();

int CostumeSlot(int chara, int costume);

bool CharaOwned(int chara);

bool CostumeOwned(int chara, int slot);

bool ColorOwnedAt(int chara, int slot, int at);

bool StageOwned(int stage, int setting);

struct Choices {
    bool initialised = false;
    bool pick = false;
    int chara = 0;
    int costume = 0;
    int color = 0;     // the game's color id: 0.. the costume's colors, 100.. its EX colors
    int vskill = 1;    // from 1
    int vtrigger = 1;  // from 1
    int variant = 0;   // the costume's variant (dress state), 0: none
    int comment = 0;   // V-Comment: 0 Auto, n the n-th
    int elevenAs = -1; // Eleven: the fighter it copies (a table index), rolled when picked
    int stage = 1;     // 1 random, 2.. a stage
    int setting = 0;   // 0 the stage's default, n its n-th setting
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
    bool hud = false;                        // the player bars over the health bars
    ULONGLONG fadeAt = 0;                    // their fade at the match's end, played from here
    bool strip = false;                      // the corner line while the panel is closed
    bool desync = false;                     // the red banner
    bool side[kNumPlayers] = {false, false}; // P1's and P2's windows after a match
    int sides = 0;                           // 0 the rematch vote, 1 character select over the results
    int you = 0;                             // whose side is this player's
    int choice[kNumPlayers] = {0, 1};        // each side's vote: none, rematch, character select
    bool ready[kNumPlayers] = {false, true};
    bool relayed = true; // the link through Cloudflare's relay (else peer to peer)
    bool set = false;    // the side windows of a table's set (no vote)

    bool Any() const { return hud || strip || desync || side[0] || side[1]; }
};

inline Preview g_preview;

inline bool g_sampleToasts = false; // add a few toasts on the next frame

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
    ULONGLONG at = 0; // when it came (it slides in)
};

inline std::vector<Toast> g_toasts; // render thread

void AddToast(const std::string& text, ImVec4 color = ImVec4(1, 1, 1, 1), int seconds = 4);

bool OnMenuScreen();

inline std::atomic<bool> g_migrated{false}; // OverlaySetMigratedNotice: not toasted yet

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

inline std::string g_previewWords; // from OverlayPreview, applied on the render thread

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

inline bool g_benchReturn = false; // a run is on: settings and its result again at the menus

struct BenchLaunch {
    bool active = false;
    int step = 0;  // 0 reading the round, 1 loading, 2 starting, 3 fading into it
    int drawn = 0; // frames the modal has been shown (the read waits for one)
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

inline ProfileFavorites g_favorites; // read at the title's warm up

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
    Choices saved;                          // the pick in effect: the card's, B's on the grid
    bool fromProfile = false;               // nothing saved yet: the Fighter Profile's favorite
    bool stagePage = false;                 // on the stage select
    int stageBefore = 1, settingBefore = 0; // the stage as the stage select opened (its B)
    bool savedNow = false;                  // SAVED: A goes back to the room, B changes again
    int8_t delaySetting = kDelayAuto;       // the Input delay row (saved with the pick)
    int openedFrame = 0;                    // the Y that opened it is not read again here
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

inline bool g_roomsWhenReady = false; // Custom Rooms chosen before the lists were read: opens once they are

void DrawHubScreens(bool takeInput);

inline int g_hubScreen = -1; // what DrawHubScreens drew: 0 the hub, 1 the room browser, 2 a room, 3 settings, 4 My character

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

inline std::string g_shotPath; // asked for, taken on the next Present

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
