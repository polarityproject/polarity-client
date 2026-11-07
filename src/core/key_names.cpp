#include "core/key_names.h"

#include <cstring>

namespace sfv {

uint32_t PadKeyOfSlot(int slot)
{
    static const uint32_t kBits[kKeyboardSlots] = {0x1,   0x2,   0x8,   0x4,   0x40,   0x80,   0x10,   0x20,
                                                   0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};
    return slot >= 0 && slot < kKeyboardSlots ? kBits[slot] : 0;
}

int VirtualKeyOf(const std::string& name)
{
    // Letters and digits as themselves ("A", "One" is Unreal's 1).
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0];
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return name[0] - 'a' + 'A';
    static const char* const kDigits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    for (int i = 0; i < 10; i++) {
        if (name == kDigits[i]) return '0' + i;
        if (name == std::string("NumPad") + kDigits[i]) return 0x60 + i; // VK_NUMPAD0..9
    }
    // F1..F12.
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F') {
        int n = 0;
        for (size_t i = 1; i < name.size(); i++) {
            if (name[i] < '0' || name[i] > '9') return 0;
            n = n * 10 + (name[i] - '0');
        }
        return n >= 1 && n <= 12 ? 0x70 + n - 1 : 0;
    }
    struct Named {
        const char* name;
        int vk;
    };
    static const Named kNamed[] = {
        {"Enter", 0x0D},        {"Escape", 0x1B},       {"SpaceBar", 0x20},     {"BackSpace", 0x08},    {"Tab", 0x09},
        {"LeftShift", 0x10},    {"RightShift", 0x10},   {"LeftControl", 0x11},  {"RightControl", 0x11}, {"LeftAlt", 0x12},
        {"RightAlt", 0x12},     {"CapsLock", 0x14},     {"Up", 0x26},           {"Down", 0x28},         {"Left", 0x25},
        {"Right", 0x27},        {"Insert", 0x2D},       {"Delete", 0x2E},       {"Home", 0x24},         {"End", 0x23},
        {"PageUp", 0x21},       {"PageDown", 0x22},     {"Multiply", 0x6A},     {"Add", 0x6B},          {"Subtract", 0x6D},
        {"Decimal", 0x6E},      {"Divide", 0x6F},       {"Semicolon", 0xBA},    {"Equals", 0xBB},       {"Comma", 0xBC},
        {"Hyphen", 0xBD},       {"Underscore", 0xBD},   {"Period", 0xBE},       {"Slash", 0xBF},        {"Tilde", 0xC0},
        {"LeftBracket", 0xDB},  {"Backslash", 0xDC},    {"RightBracket", 0xDD}, {"Apostrophe", 0xDE},   {"Quote", 0xDE},
    };
    for (const Named& n : kNamed) {
        if (name == n.name) return n.vk;
    }
    return 0;
}

} // namespace sfv
