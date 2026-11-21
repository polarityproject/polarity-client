#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

float UiScaleFor(float height) { return std::max(1.0f, height / 720.0f); }

ImVec4 Rgb(int r, int g, int b, float a) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

void ApplyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.WindowBorderSize = s.FrameBorderSize = s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
    s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Rgb(242, 240, 230);
    c[ImGuiCol_TextDisabled] = Rgb(140, 138, 126);
    c[ImGuiCol_WindowBg] = Rgb(20, 20, 18, 0.97f);
    c[ImGuiCol_ChildBg] = Rgb(0, 0, 0, 0.25f);
    c[ImGuiCol_PopupBg] = Rgb(26, 26, 22, 0.98f);
    c[ImGuiCol_Border] = Rgb(216, 212, 178, 0.45f);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = Rgb(42, 42, 34);
    c[ImGuiCol_FrameBgHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_FrameBgActive] = Rgb(72, 71, 55);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Rgb(10, 10, 10);
    c[ImGuiCol_MenuBarBg] = Rgb(10, 10, 10);
    c[ImGuiCol_ScrollbarBg] = Rgb(10, 10, 10, 0.5f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(90, 89, 72);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(120, 118, 95);
    c[ImGuiCol_ScrollbarGrabActive] = kGoldLine;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGoldLine;
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_Button] = Rgb(46, 46, 37);
    c[ImGuiCol_ButtonHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_ButtonActive] = Rgb(72, 71, 55);
    c[ImGuiCol_Header] = Rgb(57, 57, 43);
    c[ImGuiCol_HeaderHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_HeaderActive] = Rgb(88, 86, 66);
    c[ImGuiCol_Separator] = Rgb(216, 184, 91, 0.45f);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = kGoldLine;
    c[ImGuiCol_ResizeGrip] = Rgb(216, 184, 91, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(216, 184, 91, 0.6f);
    c[ImGuiCol_ResizeGripActive] = kGoldLine;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = Rgb(30, 30, 25);
    c[ImGuiCol_TabHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = kOlive;
    c[ImGuiCol_TabSelectedOverline] = kGoldLine;
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(216, 184, 91, 0.5f);
    c[ImGuiCol_TableHeaderBg] = Rgb(30, 30, 25);
    c[ImGuiCol_TableBorderStrong] = Rgb(216, 212, 178, 0.35f);
    c[ImGuiCol_TableBorderLight] = Rgb(216, 212, 178, 0.18f);
    c[ImGuiCol_TableRowBg] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TextSelectedBg] = Rgb(216, 184, 91, 0.35f);
    c[ImGuiCol_TextLink] = kGold;
    c[ImGuiCol_DragDropTarget] = kGold;
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_NavWindowingHighlight] = kGold;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = Rgb(0, 0, 0, 0.5f);
}

std::pair<void*, int> EmbeddedFont(int id) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EmbeddedFont), &self);
    HRSRC found = self ? FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)) : nullptr;
    HGLOBAL loaded = found ? LoadResource(self, found) : nullptr;
    void* data = loaded ? LockResource(loaded) : nullptr;
    return {data, data ? static_cast<int>(SizeofResource(self, found)) : 0};
}

const std::vector<uint8_t>& CjkFont() {
    static const std::vector<uint8_t> data = [] {
        std::vector<uint8_t> out;
        const std::wstring path = DataDir() + L"\\fonts\\NotoSansCJK-Medium.ttc";
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            std::fseek(f, 0, SEEK_END);
            out.resize(static_cast<size_t>(std::max(0L, std::ftell(f))));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
            std::fclose(f);
        }
        Log("overlay: CJK font %s", out.empty() ? "missing (names in Chinese, Japanese or Korean show boxes)" : "loaded");
        return out;
    }();
    return data;
}

int CjkFace() {
    const LANGID lang = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(lang) == LANG_KOREAN) return 1;
    if (PRIMARYLANGID(lang) != LANG_CHINESE) return 0;
    const WORD sub = SUBLANGID(lang);
    if (sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU) return 4;
    return sub == SUBLANG_CHINESE_TRADITIONAL ? 3 : 2;
}

void BuildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::vector<uint8_t>& cjk = CjkFont();
    const int face = CjkFace();
    auto add = [&](int id, float px) -> ImFont* {
        const auto [data, size] = EmbeddedFont(id);
        if (!data) return nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(data, size, px * scale, &cfg);
        if (font && !cjk.empty()) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.FontDataOwnedByAtlas = false;
            merge.FontNo = face;
            io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(cjk.data()), static_cast<int>(cjk.size()), px * scale, &merge);
        }
        return font;
    };
    ImFont* body = add(SFV_FONT_BARLOW_REGULAR, 17);
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 13 * scale;
        body = io.Fonts->AddFontDefault(&cfg);
    }
    g_fontHeading = add(SFV_FONT_BARLOW_BOLD_ITALIC, 20);
    if (!g_fontHeading) g_fontHeading = body;
    g_fontBig = add(SFV_FONT_BARLOW_SEMIBOLD, 40);
    if (!g_fontBig) g_fontBig = body;
    g_fontSmall = add(SFV_FONT_BARLOW_REGULAR, 14);
    if (!g_fontSmall) g_fontSmall = body;
    g_fontTiny = add(SFV_FONT_BARLOW_SEMIBOLD, 12);
    if (!g_fontTiny) g_fontTiny = g_fontSmall;
    g_fontHuge = add(SFV_FONT_ARCHIVO_BLACK, 64);
    if (!g_fontHuge) g_fontHuge = g_fontBig;
    g_fontBold = add(SFV_FONT_BARLOW_BOLD, 21);
    if (!g_fontBold) g_fontBold = g_fontBig;
    g_fontBody = body;
    io.FontDefault = body;
    ApplyTheme(scale);
    g_builtScale = g_uiScale = scale;
}

std::string Upper(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

ImVec4 PingColorVec(unsigned ms, unsigned jitterMs) {
    return ImGui::ColorConvertU32ToFloat4(ui::PingColor(ms, jitterMs));
}

ImVec4 PathColor(const NetView& v) { return v.linkPath == NetView::Path::Relayed ? Rgb(255, 210, 80) : kDim; }

void DrawLinkIcon(ImDrawList* d, ImVec2 at, float size, lobby::Link link, ImU32 color) {
    const float line = std::max(1.0f, size * 0.11f);
    if (link == lobby::Link::WiFi) {
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.86f);
        constexpr float kPi = 3.14159265f;
        for (float r : {0.3f, 0.55f, 0.8f}) {
            d->PathArcTo(c, size * r, -kPi * 0.75f, -kPi * 0.25f, 12);
            d->PathStroke(color, ImDrawFlags_None, line);
        }
        d->AddCircleFilled(c, size * 0.09f, color);
    } else if (link == lobby::Link::Wired) {
        const ImVec2 a(at.x + size * 0.2f, at.y + size * 0.12f), b(at.x + size * 0.8f, at.y + size * 0.62f);
        d->AddRect(a, b, color, 0, 0, line);
        for (float x : {0.38f, 0.5f, 0.62f}) {
            d->AddLine(ImVec2(at.x + size * x, a.y + size * 0.1f), ImVec2(at.x + size * x, a.y + size * 0.26f), color, line * 0.8f);
        }
        d->AddRectFilled(ImVec2(at.x + size * 0.36f, b.y), ImVec2(at.x + size * 0.64f, b.y + size * 0.12f), color);
        d->AddLine(ImVec2(at.x + size * 0.5f, b.y + size * 0.12f), ImVec2(at.x + size * 0.5f, at.y + size * 0.98f), color, line);
    }
}

void DrawRelayIcon(ImDrawList* d, ImVec2 at, float size, ImU32 color) {
    auto p = [&](float x, float y) { return ImVec2(at.x + size * x, at.y + size * y); };
    d->AddRectFilled(p(0.12f, 0.56f), p(0.88f, 0.84f), color, size * 0.14f);
    d->AddCircleFilled(p(0.33f, 0.58f), size * 0.2f, color);
    d->AddCircleFilled(p(0.56f, 0.46f), size * 0.27f, color);
    d->AddCircleFilled(p(0.76f, 0.62f), size * 0.19f, color);
}

std::string Roman(int n) {
    static const char* kRoman[] = {"I", "II", "III", "IV", "V"};
    return n >= 1 && n <= 5 ? kRoman[n - 1] : std::to_string(n);
}

int SmoothedFps(int seat, int fps) {
    struct Sample {
        ULONGLONG at;
        int fps;
    };

    static std::vector<Sample> samples[kNumPlayers];
    auto& s = samples[seat];
    const ULONGLONG now = GetTickCount64();
    if (fps > 0) s.push_back({now, fps});
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [now](const Sample& x) { return now - x.at <= 3000; }));
    if (s.empty()) return fps;
    double sum = 0;
    for (const Sample& x : s) sum += x.fps;
    return static_cast<int>(sum / s.size() + 0.5);
}

bool SetUp(IDXGISwapChain* swapChain) {
    ReleaseImGui();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
        Log("overlay: the swap chain has no D3D11 device");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    NoteShaderResourceViewKind(g_device);
    SetPakTextureDevice(g_device);
    g_window = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_Init(g_window);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui_ImplDX11_Init(g_device, g_context);
    BuildFonts(UiScaleFor(static_cast<float>(desc.BufferDesc.Height)));
    g_imgui = true;
    g_swapChain = swapChain;
    g_inputWindow = g_window;
    Log("overlay: set up on swap chain %p (window %p, %ux%u)", static_cast<void*>(swapChain), static_cast<void*>(g_window),
        desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

void Act(std::function<void()> action) {
    if (g_previewing) return;
    std::thread([action = std::move(action)] {
        ThreadGuard running;
        action();
    }).detach();
}

} // namespace sfv::mod::overlay
