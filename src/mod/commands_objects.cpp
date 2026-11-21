#include "mod/commands.h"

#include "mod/local_input.h"
#include "mod/files.h"
#include "mod/flow.h"

#include "mod/log.h"
#include "mod/memscan.h"
#include "mod/ue4.h"
#include "mod/overlay.h"
#include "mod/pak_textures.h"

#include <d3d11.h>

#include <random>
#include <thread>

namespace sfv::mod {

namespace {

// Field offsets of the game's UE4 objects these commands read
// (docs/commands.md has what each holds).
namespace ufield {
constexpr uint32_t kNext = 0x28;
constexpr uint32_t kRaw = 0x30; // the words logged after a field's name
constexpr uint32_t kOffset = 0x4C;
constexpr uint32_t kChildren = 0x38; // a UStruct's first field
constexpr uint32_t kStructSize = 0x40;
constexpr uint32_t kInner = 0x70; // an ArrayProperty's inner property, a StructProperty's struct
constexpr uint32_t kReadable = 0x80;
} // namespace ufield

namespace uenum {
constexpr uint32_t kNames = 0x40;
constexpr uint32_t kCount = 0x48;
constexpr uint32_t kSize = 0x50;
} // namespace uenum

// ScaleformUtilGFxPlayer.
namespace gfxplayer {
constexpr uint32_t kMoviePath = 0x300, kMoviePathLength = 0x308;
constexpr uint32_t kAsset2 = 0x310;
constexpr uint32_t kMovie = 0x318;
constexpr uint32_t kDraw = 0x374;
constexpr uint32_t kAsset = 0x390;
constexpr uint32_t kNative = 0x398;
constexpr uint32_t kStepMode = 0x3A0;
constexpr uint32_t kSize = 0x3A8;
} // namespace gfxplayer

// KWUserProfileDetails.
namespace profile {
constexpr uint32_t kFlags = 0x28; // bit 0: the main profile
constexpr uint32_t kKeyConfig = 0xB0;
constexpr uint32_t kSize = 0xC0;
} // namespace profile

constexpr int kMembersBytes = 0x400;
// Where heap pointers lie (user space above the low 1 TB).
constexpr uint64_t kHeapLow = 0x10000000000ull, kHeapHigh = 0x800000000000ull;
constexpr uintptr_t kAllocationGranularity = 0x10000;

void UobjCommand(const std::string&, std::stringstream& ss) {
    std::string name, how;
    ss >> name >> how;
    RunOnGameThread([name, dump = how == "dump"] { LogObjectsNamed(name, dump); });
}

void MembersCommand(const std::string&, std::stringstream& ss) {
    unsigned long long address = 0;
    int bytes = kMembersBytes;
    ss >> std::hex >> address >> std::dec >> bytes;
    Log("members of %s:", ue4::Describe(reinterpret_cast<const void*>(address)).c_str());
    for (int off = 0; off < bytes; off += 8) {
        uint64_t v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + off), &v, 8, &got)) break;
        if (v < kHeapLow || v >= kHeapHigh) continue;
        const std::string d = ue4::Describe(reinterpret_cast<const void*>(v));
        if (d.find('?') == std::string::npos && d.size() > 3 && d.front() != ' ') Log("  +0x%x: %s (%llx)", off, d.c_str(), static_cast<unsigned long long>(v));
    }
}

void ClassesCommand(const std::string&, std::stringstream& ss) {
    std::string text;
    ss >> text;
    if (!ue4::EnsureObjects(ImageBase())) return;
    std::vector<const void*> classes;
    ue4::ForEachObject([&](const void* object) {
        const std::string kind = ue4::ObjectName(ue4::ClassOf(object));
        if ((kind == "Class" || kind == "BlueprintGeneratedClass") && ue4::ObjectName(object).find(text) != std::string::npos &&
            classes.size() < 60) {
            classes.push_back(object);
        }
    });
    for (const void* cls : classes) {
        int count = 0;
        const void* first = nullptr;
        ue4::ForEachObject([&](const void* object) {
            for (const void* c = ue4::ClassOf(object); c; c = ue4::SuperOf(c)) {
                if (c != cls) continue;
                if (!count++) first = object;
                break;
            }
        });
        Log("class %s (%p): %d objects, first %s (%p)", ue4::ObjectName(cls).c_str(), cls, count,
            ue4::Describe(first).c_str(), first);
    }
}

void PackagesCommand(const std::string&, std::stringstream& ss) {
    std::string what;
    ss >> what;
    if (!ue4::EnsureObjects(ImageBase())) return;
    std::map<std::string, int> packages; // name -> objects inside
    ue4::ForEachObject([&](const void* object) {
        const void* root = object;
        while (const void* outer = ue4::OuterOf(root)) root = outer;
        if (ue4::ObjectName(ue4::ClassOf(root)) != "Package") return;
        packages[ue4::ObjectName(root)]++;
    });
    static std::map<std::string, int> marked;
    if (what == "mark") {
        marked = packages;
        Log("packages: %zu marked", marked.size());
        return;
    }
    size_t added = 0, objects = 0;
    for (const auto& [name, count] : packages) {
        if (marked.count(name)) continue;
        Log("package+ %s %d", name.c_str(), count);
        added++;
        objects += count;
    }
    size_t gone = 0;
    for (const auto& [name, count] : marked) {
        if (packages.count(name)) continue;
        Log("package- %s %d", name.c_str(), count);
        gone++;
    }
    Log("packages: %zu now, %zu new (%zu objects), %zu gone since the mark", packages.size(), added, objects, gone);
}

void PreloadCommand(const std::string&, std::stringstream& ss) {
    std::string code;
    int costume = 0;
    ss >> code >> costume;
    PreloadCharacter(code, costume);
}

void VersionsCommand(const std::string&, std::stringstream&) {
    LogCharaVersions();
}

void TexturesCommand(const std::string&, std::stringstream& ss) {
    std::string filter;
    int limit = 80;
    ss >> filter >> limit;
    LogTextures(filter, limit);
}

void ShowtexCommand(const std::string&, std::stringstream& ss) {
    std::string which;
    ss >> which;
    if (which.empty() || which == "off") {
        SetOverlayTexture(nullptr, 0, 0, "");
        return;
    }
    RunOnGameThread([which] {
        const void* texture = FindOrLoadTexture(which);
        if (!texture) {
            Log("showtex: no texture %s", which.c_str());
            return;
        }
        const TextureView view = ViewOfTexture(texture);
        if (view.srv) SetOverlayTexture(view.srv, view.width, view.height, which);
    });
}

void PaktexCommand(const std::string&, std::stringstream& ss) {
    std::string what, path;
    uint32_t maxSide = 0;
    ss >> what >> path >> maxSide;
    if (what == "on" || what == "off") SetPakTexturesEnabled(what == "on");
    else if (what == "status") Log("%s", PakTextureStatus().c_str());
    else if (what == "dump") {
        std::thread([path] { DumpPakPackage(path, Utf8Path(DataDir()) + "\\textures\\" + path.substr(path.rfind('/') + 1) + ".uasset"); }).detach();
    } else if (what == "format") {
        RunOnGameThread([path] {
            const TextureView view = ViewOfTexture(FindOrLoadTexture(path + "." + path.substr(path.rfind('/') + 1)), true);
            if (!view.srv) return;
            auto* srv = static_cast<ID3D11ShaderResourceView*>(view.srv);
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            srv->GetDesc(&desc);
            Log("paktex: the game's view of %s: %ux%u, view format %d, %u mips", path.c_str(), view.width, view.height,
                int(desc.Format), desc.Texture2D.MipLevels);
            srv->Release();
        });
    } else if (what == "show" || what == "save") {
        std::thread([what, path, maxSide] {
            for (int i = 0; i < 200; i++) {
                bool failed = false;
                if (const TextureView* t = PakTexture(path, maxSide, true, &failed)) {
                    static_cast<IUnknown*>(t->srv)->AddRef();
                    Log("paktex: %s from the paks: %ux%u", path.c_str(), t->width, t->height);
                    if (what == "show") SetOverlayTexture(t->srv, t->width, t->height, path + " (paks)");
                    else SaveTextureLater(t->srv, "pak_" + path.substr(path.rfind('/') + 1));
                    return;
                }
                if (failed) {
                    Log("paktex: %s: the paks could not give it", path.c_str());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            Log("paktex: %s: not ready after 5 s", path.c_str());
        }).detach();
    }
}

void SavetexCommand(const std::string&, std::stringstream& ss) {
    std::vector<std::string> which;
    std::string word;
    while (ss >> word) which.push_back(word);
    RunOnGameThread([which] {
        for (const std::string& w : which) {
            const void* texture = FindOrLoadTexture(w);
            const TextureView view = texture ? ViewOfTexture(texture) : TextureView{};
            if (!view.srv) {
                Log("savetex: no texture %s", w.c_str());
                continue;
            }
            std::string name = w.rfind("/Game/", 0) == 0 ? w.substr(6) : w;
            name = name.substr(0, name.find('.'));
            std::replace(name.begin(), name.end(), '/', '_');
            SaveTextureLater(view.srv, name);
        }
    });
}

void MapatCommand(const std::string&, std::stringstream& ss) {
    std::string a;
    while (ss >> a) {
        const uintptr_t at = static_cast<uintptr_t>(std::stoull(a, nullptr, 16)) & ~(kAllocationGranularity - 1);
        void* p = VirtualAlloc(reinterpret_cast<void*>(at), kAllocationGranularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Log("mapat: %p %s", reinterpret_cast<void*>(at), p ? "committed" : "not free");
    }
}

void CharcacheCommand(const std::string&, std::stringstream&) {
    LogCharaCache();
}

void DatatableCommand(const std::string&, std::stringstream& ss) {
    std::string name, filter;
    int limit = 200;
    ss >> name >> filter >> limit;
    if (filter == "-") filter.clear();
    LogDataTable(name, filter, limit);
}

void VariantsCommand(const std::string&, std::stringstream&) {
    LogAllVariants();
}

void PresetsCommand(const std::string&, std::stringstream& ss) {
    std::string code;
    ss >> code;
    LogCostumePresets(code);
}

void CostumesCommand(const std::string&, std::stringstream& ss) {
    std::string code;
    ss >> code;
    LogCharaCostumes(code);
}

void CharasCommand(const std::string&, std::stringstream&) {
    std::string list;
    for (int i = 0; i < CharaCount(); i++) {
        list += " " + CharaCode(i) + "=" + CharaName(i) + "(" + std::to_string(CharaCostumeCount(i)) + ")";
    }
    Log("characters (%d):%s", CharaCount(), list.c_str());
}

void RelocateCommand(const std::string&, std::stringstream&) {
    const auto engine = *reinterpret_cast<uint8_t* const*>(ImageBase() + game::singleton::kEngineGlobal);
    ue4::LocateObjectsFrom(engine ? *reinterpret_cast<void* const*>(engine + game::singleton::kSingleton) : nullptr);
}

void SceneCommand(const std::string&, std::stringstream& ss) {
    std::string name, param;
    ss >> name >> param;
    ChangeScene(name, param);
}

void NamedCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    if (!ue4::EnsureObjects(ImageBase())) return;
    int shown = 0;
    ue4::ForEachObject([&](const void* object) {
        if (shown >= 20 || ue4::ObjectName(object) != name) return;
        shown++;
        Log("named %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::Describe(ue4::OuterOf(object)).c_str());
    });
}

void OfclassCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    int max = 60;
    ss >> name >> max;
    if (!ue4::EnsureObjects(ImageBase())) return;
    int shown = 0, total = 0;
    ue4::ForEachObject([&](const void* object) {
        for (const void* c = ue4::ClassOf(object); c; c = ue4::SuperOf(c)) {
            if (ue4::ObjectName(c) != name) continue;
            total++;
            if (shown < max && ue4::ObjectName(object).rfind("Default__", 0) != 0) {
                shown++;
                Log("ofclass %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::Describe(ue4::OuterOf(object)).c_str());
            }
            break;
        }
    });
    Log("ofclass %s: %d of %d shown", name.c_str(), shown, total);
}

void GfxCommand(const std::string&, std::stringstream& ss) {
    int words = 0, actorSlots = 0;
    ss >> words >> actorSlots;
    if (!ue4::EnsureObjects(ImageBase())) return;
    const auto rva = [](uint64_t v) { return ImageHolds(v) ? v - ImageBase() : 0; };
    if (actorSlots > 0) {
        const auto* actor = static_cast<const uint8_t*>(ue4::FindObject("Default__Actor", "Actor"));
        const auto* player = static_cast<const uint8_t*>(ue4::FindObject("Default__ScaleformUtilGFxPlayer", "ScaleformUtilGFxPlayer"));
        if (actor && player) {
            const auto* va = *reinterpret_cast<const uint64_t* const*>(actor);
            const auto* vp = *reinterpret_cast<const uint64_t* const*>(player);
            std::string w;
            for (int i = 0; i < actorSlots && IsReadable(va + i, 8) && IsReadable(vp + i, 8); i++) {
                if (va[i] == vp[i]) continue;
                char b[40];
                std::snprintf(b, sizeof(b), " %d:%llX/%llX", i, static_cast<unsigned long long>(rva(va[i])), static_cast<unsigned long long>(rva(vp[i])));
                w += b;
            }
            Log("gfx player virtuals (slot:actor/player):%s", w.c_str());
        } else Log("gfx: no Default__Actor (%p) or Default__ScaleformUtilGFxPlayer (%p)", actor, player);
    }
    ue4::ForEachObject([&](const void* object) {
        bool is = false;
        for (const void* c = ue4::ClassOf(object); c && !is; c = ue4::SuperOf(c)) is = ue4::ObjectName(c) == "ScaleformUtilGFxPlayer";
        if (!is || ue4::ObjectName(object).rfind("Default__", 0) == 0) return;
        const auto* p = static_cast<const uint8_t*>(object);
        if (!IsReadable(p, gfxplayer::kSize)) return;
        std::string path;
        const auto* text = *reinterpret_cast<const wchar_t* const*>(p + gfxplayer::kMoviePath);
        const int len = *reinterpret_cast<const int32_t*>(p + gfxplayer::kMoviePathLength);
        if (text && len > 0 && len < 400 && IsReadable(text, len * 2)) {
            for (int i = 0; i < len && text[i]; i++) path += static_cast<char>(text[i] < 128 ? text[i] : '?');
        }
        const auto* movie = *reinterpret_cast<const uint8_t* const*>(p + gfxplayer::kMovie);
        const auto* native = *reinterpret_cast<const uint8_t* const*>(p + gfxplayer::kNative);
        const uint64_t vt = native && IsReadable(native, 8) ? *reinterpret_cast<const uint64_t*>(native) : 0;
        const void* asset = *reinterpret_cast<const void* const*>(p + gfxplayer::kAsset);
        const void* asset2 = *reinterpret_cast<const void* const*>(p + gfxplayer::kAsset2);
        Log("gfx %s (%p): '%s' asset %s / %s, step %d, movie %s (%p), native %p vtable rva 0x%llX, object vtable rva 0x%llX, draw %d",
            ue4::Describe(object).c_str(), object, path.c_str(), ue4::Describe(asset).c_str(), ue4::Describe(asset2).c_str(), p[gfxplayer::kStepMode],
            ue4::Describe(movie).c_str(), movie, native,
            static_cast<unsigned long long>(rva(vt)), static_cast<unsigned long long>(rva(*reinterpret_cast<const uint64_t*>(p))),
            *reinterpret_cast<const int32_t*>(p + gfxplayer::kDraw));
        if (native && IsReadable(native, ufield::kReadable)) {
            std::string w;
            for (int i = 0; i < 16; i++) {
                char b[24];
                std::snprintf(b, sizeof(b), " %llx", static_cast<unsigned long long>(reinterpret_cast<const uint64_t*>(native)[i]));
                w += b;
            }
            Log("gfx   native words:%s", w.c_str());
        }
        if (words > 0 && vt && IsReadable(reinterpret_cast<const void*>(vt), words * 8)) {
            std::string w;
            for (int i = 0; i < words; i++) {
                char b[24];
                std::snprintf(b, sizeof(b), " %d:%llX", i, static_cast<unsigned long long>(rva(reinterpret_cast<const uint64_t*>(vt)[i])));
                w += b;
            }
            Log("gfx   native vtable:%s", w.c_str());
            words = 0; // once
        }
    });
}

void FindCommand(const std::string&, std::stringstream& ss) {
    std::string text;
    ss >> text;
    if (!ue4::EnsureObjects(ImageBase())) return;
    auto lower = [](std::string v) {
        for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    const std::string needle = lower(text);
    int shown = 0;
    ue4::ForEachObject([&](const void* object) {
        if (shown >= 60 || lower(ue4::ObjectName(object)).find(needle) == std::string::npos) return;
        shown++;
        Log("find %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::ObjectName(ue4::OuterOf(object)).c_str());
    });
    Log("find %s: %d shown", text.c_str(), shown);
}

void ClassinfoCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    if (!ue4::EnsureObjects(ImageBase())) return;
    const auto* cls = static_cast<const uint8_t*>(ue4::FindObject(name, "Class"));
    if (!cls) cls = static_cast<const uint8_t*>(ue4::FindObject(name, "BlueprintGeneratedClass"));
    Log("classinfo %s: %p, super %s, size 0x%zx", name.c_str(), cls, cls ? ue4::ObjectName(ue4::SuperOf(cls)).c_str() : "",
        cls ? static_cast<size_t>(*reinterpret_cast<const int32_t*>(cls + ufield::kStructSize)) : 0);
    auto fields = [](const uint8_t* owner, const char* indent) {
        const uint8_t* f = owner ? *reinterpret_cast<const uint8_t* const*>(owner + ufield::kChildren) : nullptr;
        for (int i = 0; f && i < 200 && IsReadable(f, ufield::kReadable); i++) {
            const auto* w = reinterpret_cast<const int32_t*>(f + ufield::kRaw);
            Log("%s%s: %x %x %x %x %x %x %x %x", indent, ue4::Describe(f).c_str(), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                w[7]);
            const std::string fieldClass = ue4::ObjectName(ue4::ClassOf(f));
            if (fieldClass == "ArrayProperty" || fieldClass == "StructProperty") {
                const auto* inner = fieldClass == "StructProperty" ? f : *reinterpret_cast<const uint8_t* const*>(f + ufield::kInner);
                if (inner && fieldClass == "ArrayProperty" && ue4::ObjectName(ue4::ClassOf(inner)) != "StructProperty") {
                    Log("%s  of %s", indent, ue4::ObjectName(ue4::ClassOf(inner)).c_str());
                }
                if (inner && ue4::ObjectName(ue4::ClassOf(inner)) == "StructProperty") {
                    const auto* st = *reinterpret_cast<const uint8_t* const*>(inner + ufield::kInner);
                    if (st && IsReadable(st, ufield::kStructSize + 8)) {
                        Log("%s  of %s, size 0x%x:", indent, ue4::Describe(st).c_str(), *reinterpret_cast<const int32_t*>(st + ufield::kStructSize));
                        const uint8_t* g = *reinterpret_cast<const uint8_t* const*>(st + ufield::kChildren);
                        for (int j = 0; g && j < 100 && IsReadable(g, ufield::kOffset + 4); j++) {
                            Log("%s    %s at +0x%x", indent, ue4::Describe(g).c_str(), *reinterpret_cast<const int32_t*>(g + ufield::kOffset));
                            g = *reinterpret_cast<const uint8_t* const*>(g + ufield::kNext);
                        }
                    }
                }
            }
            f = *reinterpret_cast<const uint8_t* const*>(f + ufield::kNext);
        }
    };
    fields(cls, "  ");
}

void PokeCommand(const std::string&, std::stringstream& ss) {
    unsigned long long address = 0, value = 0;
    int bytes = 4;
    ss >> std::hex >> address >> value >> std::dec >> bytes;
    SIZE_T done = 0;
    const bool ok = (bytes == 1 || bytes == 4 || bytes == 8) &&
                    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, bytes, &done);
    Log("poke %llx = %llx (%d bytes): %s", address, value, bytes, ok ? "ok" : "failed");
}

void FindbytesCommand(const std::string&, std::stringstream& ss) {
    std::string hex;
    ss >> hex;
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    FindBytes(bytes);
}

void LayoutCommand(const std::string&, std::stringstream& ss) {
    uint32_t* table = InputKeyTable();
    if (!table) return;
    std::string word;
    uint32_t values[16];
    int count = 0;
    while (count < 16 && ss >> word) values[count++] = static_cast<uint32_t>(std::stoul(word, nullptr, 16));
    if (count == 16) std::memcpy(table, values, sizeof(values));
    std::string text;
    for (int k = 0; k < 16; k++) {
        char hex[12];
        std::snprintf(hex, sizeof(hex), " %x", table[k]);
        text += hex;
    }
    Log("layout: pad 0 keys up..select give%s%s", text.c_str(), count == 16 ? " (set)" : "");
}

void KeysCommand(const std::string&, std::stringstream& ss) {
    std::vector<WORD> vks;
    int holdMs = 150;
    std::string word;
    while (ss >> word) {
        if (std::isdigit(static_cast<unsigned char>(word[0])) && word.size() > 1) holdMs = std::atoi(word.c_str());
        else if (word == "up") vks.push_back(VK_UP);
        else if (word == "down") vks.push_back(VK_DOWN);
        else if (word == "left") vks.push_back(VK_LEFT);
        else if (word == "right") vks.push_back(VK_RIGHT);
        else if (word == "enter") vks.push_back(VK_RETURN);
        else if (word.size() == 1) vks.push_back(static_cast<WORD>(std::toupper(static_cast<unsigned char>(word[0]))));
    }
    HWND w = InputGameWindow();
    auto realFront = [] { return InputRealForegroundWindow(); };
    for (int attempt = 0; w && attempt < 3 && realFront() != w; attempt++) {
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        const DWORD front = GetWindowThreadProcessId(realFront(), nullptr);
        const DWORD self = GetCurrentThreadId();
        const bool attached = front && front != self && AttachThreadInput(self, front, TRUE);
        INPUT alt[2] = {};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, alt, sizeof(INPUT));
        BringWindowToTop(w);
        SetForegroundWindow(w);
        if (attached) AttachThreadInput(self, front, FALSE);
        Sleep(300);
    }
    const bool inFront = w && realFront() == w;
    UINT accepted = 0;
    for (WORD vk : vks) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = vk;
        in[0].ki.wScan = in[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        accepted += SendInput(1, &in[0], sizeof(INPUT));
        Sleep(holdMs);
        SendInput(1, &in[1], sizeof(INPUT));
        Sleep(150);
    }
    Log("keys: %u of %zu pressed (game window %s)", accepted, vks.size(), inFront ? "in front" : "not in front");
}

void Input2Command(const std::string&, std::stringstream& ss) {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring path = std::wstring(local) + L"\\StreetFighterV\\Saved\\Config\\WindowsNoEditor\\Input2.ini";
    const std::wstring backup = path + L".sfvr-backup";
    std::string key, value;
    ss >> key >> value;
    if (key == "restore") {
        const bool ok = CopyFileW(backup.c_str(), path.c_str(), FALSE) && DeleteFileW(backup.c_str());
        Log("input2: %s", ok ? "restored from the backup" : "no backup to restore");
        return;
    }
    if (key.empty() || value.empty()) return;
    CopyFileW(path.c_str(), backup.c_str(), TRUE); // only the first time
    const bool ok = WritePrivateProfileStringW(L"AssignKeyboard", std::wstring(key.begin(), key.end()).c_str(),
                                               std::wstring(value.begin(), value.end()).c_str(), path.c_str());
    Log("input2: [AssignKeyboard] %s=%s %s (from the next start)", key.c_str(), value.c_str(), ok ? "set" : "failed");
}

void OwnerCommand(const std::string&, std::stringstream& ss) {
    unsigned long long address = 0;
    ss >> std::hex >> address;
    if (!ue4::EnsureObjects(ImageBase())) return;
    int found = 0;
    ue4::ForEachObject([&](const void* object) {
        const auto start = reinterpret_cast<uintptr_t>(object);
        const size_t size = ue4::ObjectSize(object);
        if (address < start || address >= start + size || size == 0) return;
        Log("owner %llx: %s +0x%llx (size 0x%zx)", address, ue4::Describe(object).c_str(),
            static_cast<unsigned long long>(address - start), size);
        found++;
    });
    if (!found) Log("owner %llx: no UObject holds it", address);
}

void EnuminfoCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    if (!ue4::EnsureObjects(ImageBase())) return;
    const auto* e = static_cast<const uint8_t*>(ue4::FindObject(name, "Enum"));
    if (!e || !IsReadable(e, uenum::kSize)) {
        Log("enuminfo: no enum %s", name.c_str());
        return;
    }
    const auto* pairs = *reinterpret_cast<const uint8_t* const*>(e + uenum::kNames);
    const int32_t count = *reinterpret_cast<const int32_t*>(e + uenum::kCount);
    if (!pairs || count <= 0 || count > 512 || !IsReadable(pairs, static_cast<size_t>(count) * 8)) {
        Log("enuminfo %s: unexpected layout (%p, %d)", name.c_str(), pairs, count);
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        const int32_t index = *reinterpret_cast<const int32_t*>(pairs + i * 8);
        Log("enuminfo %s: %d %s", name.c_str(), i, ue4::NameText(index).c_str());
    }
}

void KeyconfigsCommand(const std::string&, std::stringstream&) {
    if (!ue4::EnsureObjects(ImageBase())) return;
    ue4::ForEachObject([](const void* object) {
        if (ue4::ObjectName(ue4::ClassOf(object)) != "KWUserProfileDetails") return;
        const auto* p = static_cast<const uint8_t*>(object);
        if (!IsReadable(p, profile::kSize)) return;
        char hex[64] = {};
        for (int i = 0; i < 16; i++) std::snprintf(hex + i * 2, 3, "%02x", p[profile::kKeyConfig + i]);
        Log("keyconfigs: %s (%p) main %d: %s", ue4::ObjectName(object).c_str(), object, p[profile::kFlags] & 1, hex);
    });
}

void FindptrCommand(const std::string&, std::stringstream& ss) {
    unsigned long long v = 0;
    ss >> std::hex >> v;
    FindPointer(v);
}

void UfuncCommand(const std::string&, std::stringstream& ss) {
    std::string name;
    ss >> name;
    RunOnGameThread([name] { LogFunction(name); });
}

void FunctraceCommand(const std::string&, std::stringstream& ss) {
    std::string name, owner;
    uint32_t offset = 0;
    ss >> name >> owner >> std::hex >> offset;
    if (owner == "-") owner.clear();
    RunOnGameThread([name, owner, offset] { TraceFunction(name, owner, offset); });
}

void StagesCommand(const std::string&, std::stringstream&) {
    LogStageTable();
}

void WorldctxCommand(const std::string&, std::stringstream&) {
    LogWorldContext();
}

void PeekCommand(const std::string&, std::stringstream& ss) {
    unsigned long long address = 0;
    int count = 4;
    ss >> std::hex >> address >> std::dec >> count;
    for (int i = 0; i < count && i < 64; i++) {
        uint64_t v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 8 * i), &v, 8, &got)) break;
        const bool inImage = ImageHolds(v);
        Log("peek %llx: %016llx%s", address + 8 * i, static_cast<unsigned long long>(v),
            inImage ? (" (rva 0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v - ImageBase())); return std::string(b); }() + ")").c_str() : "");
    }
}

constexpr Command kCommands[] = {
    {"uobj", UobjCommand},
    {"members", MembersCommand},
    {"classes", ClassesCommand},
    {"packages", PackagesCommand},
    {"preload", PreloadCommand},
    {"versions", VersionsCommand},
    {"textures", TexturesCommand},
    {"showtex", ShowtexCommand},
    {"paktex", PaktexCommand},
    {"savetex", SavetexCommand},
    {"mapat", MapatCommand},
    {"charcache", CharcacheCommand},
    {"datatable", DatatableCommand},
    {"variants", VariantsCommand},
    {"presets", PresetsCommand},
    {"costumes", CostumesCommand},
    {"charas", CharasCommand},
    {"relocate", RelocateCommand},
    {"scene", SceneCommand},
    {"named", NamedCommand},
    {"ofclass", OfclassCommand},
    {"gfx", GfxCommand},
    {"find", FindCommand},
    {"classinfo", ClassinfoCommand},
    {"poke", PokeCommand},
    {"findbytes", FindbytesCommand},
    {"layout", LayoutCommand},
    {"keys", KeysCommand},
    {"input2", Input2Command},
    {"owner", OwnerCommand},
    {"enuminfo", EnuminfoCommand},
    {"keyconfigs", KeyconfigsCommand},
    {"findptr", FindptrCommand},
    {"ufunc", UfuncCommand},
    {"functrace", FunctraceCommand},
    {"stages", StagesCommand},
    {"worldctx", WorldctxCommand},
    {"peek", PeekCommand},
};

} // namespace

std::span<const Command> CommandsObjects() { return kCommands; }

} // namespace sfv::mod
