// xinput1_3.dll proxy: loads the mod and passes every XInput call on to the real XInput (docs/local-input.md).

#include "proxy/load_mod.h"
#include "proxy/real_xinput.h"

#include <xinput.h>

namespace {

constexpr WORD kGetStateEx = 100, kWaitForGuideButton = 101, kCancelGuideButtonWait = 102, kPowerOffController = 103;

template <typename Fn>
Fn Real(const char* name) {
    HMODULE m = sfv::proxy::RealXInput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

thread_local int t_depth = 0;

struct Depth {
    Depth() { t_depth++; }

    ~Depth() { t_depth--; }
};

template <typename Fn>
Fn Target(Fn real, const char* name) {
    if (t_depth == 0) return real;
    if (t_depth > 1) return nullptr;
    HMODULE m = sfv::proxy::FallbackXInput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

} // namespace

#define EXPORT(name, ord) __pragma(comment(linker, "/export:" #name "=Proxy" #name ",@" #ord))
#define EXPORT_ORDINAL(ord, fn) __pragma(comment(linker, "/export:Ordinal" #ord "=" #fn ",@" #ord ",NONAME"))

extern "C" {
DWORD WINAPI ProxyXInputGetState(DWORD user, XINPUT_STATE* state) {
    static const auto real = Real<decltype(&ProxyXInputGetState)>("XInputGetState");
    const auto fn = Target(real, "XInputGetState");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, state);
}

DWORD WINAPI ProxyXInputSetState(DWORD user, XINPUT_VIBRATION* vibration) {
    static const auto real = Real<decltype(&ProxyXInputSetState)>("XInputSetState");
    const auto fn = Target(real, "XInputSetState");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, vibration);
}

DWORD WINAPI ProxyXInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    static const auto real = Real<decltype(&ProxyXInputGetCapabilities)>("XInputGetCapabilities");
    const auto fn = Target(real, "XInputGetCapabilities");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, flags, caps);
}

void WINAPI ProxyXInputEnable(BOOL enable) {
    static const auto real = Real<decltype(&ProxyXInputEnable)>("XInputEnable");
    const auto fn = Target(real, "XInputEnable");
    if (!fn) return;
    const Depth depth;
    fn(enable);
}

DWORD WINAPI ProxyXInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    static const auto real = Real<decltype(&ProxyXInputGetDSoundAudioDeviceGuids)>("XInputGetDSoundAudioDeviceGuids");
    const auto fn = Target(real, "XInputGetDSoundAudioDeviceGuids");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, render, capture);
}

DWORD WINAPI ProxyXInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info) {
    static const auto real = Real<decltype(&ProxyXInputGetBatteryInformation)>("XInputGetBatteryInformation");
    const auto fn = Target(real, "XInputGetBatteryInformation");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, type, info);
}

DWORD WINAPI ProxyXInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE* keystroke) {
    static const auto real = Real<decltype(&ProxyXInputGetKeystroke)>("XInputGetKeystroke");
    const auto fn = Target(real, "XInputGetKeystroke");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, reserved, keystroke);
}

DWORD WINAPI ProxyOrdinal100(DWORD user, void* state) {
    static const auto real = Real<decltype(&ProxyOrdinal100)>(MAKEINTRESOURCEA(kGetStateEx));
    const auto fn = Target(real, MAKEINTRESOURCEA(kGetStateEx));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, state);
}

DWORD WINAPI ProxyOrdinal101(DWORD user, DWORD flags, void* event) {
    static const auto real = Real<decltype(&ProxyOrdinal101)>(MAKEINTRESOURCEA(kWaitForGuideButton));
    const auto fn = Target(real, MAKEINTRESOURCEA(kWaitForGuideButton));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, flags, event);
}

DWORD WINAPI ProxyOrdinal102(DWORD user) {
    static const auto real = Real<decltype(&ProxyOrdinal102)>(MAKEINTRESOURCEA(kCancelGuideButtonWait));
    const auto fn = Target(real, MAKEINTRESOURCEA(kCancelGuideButtonWait));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user);
}

DWORD WINAPI ProxyOrdinal103(DWORD user) {
    static const auto real = Real<decltype(&ProxyOrdinal103)>(MAKEINTRESOURCEA(kPowerOffController));
    const auto fn = Target(real, MAKEINTRESOURCEA(kPowerOffController));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user);
}
}

EXPORT(XInputGetState, 2)
EXPORT(XInputSetState, 3)
EXPORT(XInputGetCapabilities, 4)
EXPORT(XInputEnable, 5)
EXPORT(XInputGetDSoundAudioDeviceGuids, 6)
EXPORT(XInputGetBatteryInformation, 7)
EXPORT(XInputGetKeystroke, 8)
EXPORT_ORDINAL(100, ProxyOrdinal100)
EXPORT_ORDINAL(101, ProxyOrdinal101)
EXPORT_ORDINAL(102, ProxyOrdinal102)
EXPORT_ORDINAL(103, ProxyOrdinal103)
