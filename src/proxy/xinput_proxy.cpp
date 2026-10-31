// xinput1_3.dll proxy, the mod's loader. StreetFighterV.exe imports
// XInputGetState and XInputSetState by ordinal (2 and 3) from XINPUT1_3.dll,
// which is not a KnownDLL and not preloaded by system DLLs, so a copy next to
// the exe is the one the loader binds. Every export passes the call on to the
// real XInput (real_xinput.h: System32's xinput1_3, or Wine's xinput1_4) at
// its original ordinal, including the unnamed ordinals 100-103; one the real
// XInput lacks answers "not connected". They were linker forwards to
// System32 until Proton loaded that one as the useless native copy.

#include "proxy/load_mod.h"
#include "proxy/real_xinput.h"

#include <xinput.h>

namespace {

// The real export by name or ordinal, looked up once per export.
template <typename Fn> Fn Real(const char* name)
{
    HMODULE m = sfv::proxy::RealXInput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

} // namespace

#define EXPORT(name, ord) __pragma(comment(linker, "/export:" #name "=Proxy" #name ",@" #ord))
#define EXPORT_ORDINAL(ord, fn) __pragma(comment(linker, "/export:Ordinal" #ord "=" #fn ",@" #ord ",NONAME"))

extern "C" {

DWORD WINAPI ProxyXInputGetState(DWORD user, XINPUT_STATE* state)
{
    static const auto fn = Real<decltype(&ProxyXInputGetState)>("XInputGetState");
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyXInputSetState(DWORD user, XINPUT_VIBRATION* vibration)
{
    static const auto fn = Real<decltype(&ProxyXInputSetState)>("XInputSetState");
    return fn ? fn(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyXInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    static const auto fn = Real<decltype(&ProxyXInputGetCapabilities)>("XInputGetCapabilities");
    return fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI ProxyXInputEnable(BOOL enable)
{
    static const auto fn = Real<decltype(&ProxyXInputEnable)>("XInputEnable");
    if (fn) fn(enable);
}

DWORD WINAPI ProxyXInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture)
{
    static const auto fn = Real<decltype(&ProxyXInputGetDSoundAudioDeviceGuids)>("XInputGetDSoundAudioDeviceGuids");
    return fn ? fn(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyXInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info)
{
    static const auto fn = Real<decltype(&ProxyXInputGetBatteryInformation)>("XInputGetBatteryInformation");
    return fn ? fn(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyXInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE* keystroke)
{
    static const auto fn = Real<decltype(&ProxyXInputGetKeystroke)>("XInputGetKeystroke");
    return fn ? fn(user, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

// The unnamed ones: XInputGetStateEx (the guide button too), the guide button
// wait and its cancel, a controller's power off.
DWORD WINAPI ProxyOrdinal100(DWORD user, void* state)
{
    static const auto fn = Real<decltype(&ProxyOrdinal100)>(MAKEINTRESOURCEA(100));
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyOrdinal101(DWORD user, DWORD flags, void* event)
{
    static const auto fn = Real<decltype(&ProxyOrdinal101)>(MAKEINTRESOURCEA(101));
    return fn ? fn(user, flags, event) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyOrdinal102(DWORD user)
{
    static const auto fn = Real<decltype(&ProxyOrdinal102)>(MAKEINTRESOURCEA(102));
    return fn ? fn(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI ProxyOrdinal103(DWORD user)
{
    static const auto fn = Real<decltype(&ProxyOrdinal103)>(MAKEINTRESOURCEA(103));
    return fn ? fn(user) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"

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
