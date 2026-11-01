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

// Calls into the real XInput on this thread that have not returned. One
// already running means the call came back into the proxy through a hook on
// the real XInput (Special K's): it goes to the fallback (real_xinput.h)
// instead; a third time round, nowhere ("not connected").
thread_local int t_depth = 0;

struct Depth {
    Depth() { t_depth++; }
    ~Depth() { t_depth--; }
};

template <typename Fn> Fn Target(Fn real, const char* name)
{
    if (t_depth == 0) return real;
    if (t_depth > 1) return nullptr;
    HMODULE m = sfv::proxy::FallbackXInput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

} // namespace

#define EXPORT(name, ord) __pragma(comment(linker, "/export:" #name "=Proxy" #name ",@" #ord))
#define EXPORT_ORDINAL(ord, fn) __pragma(comment(linker, "/export:Ordinal" #ord "=" #fn ",@" #ord ",NONAME"))

extern "C" {

DWORD WINAPI ProxyXInputGetState(DWORD user, XINPUT_STATE* state)
{
    static const auto real = Real<decltype(&ProxyXInputGetState)>("XInputGetState");
    const auto fn = Target(real, "XInputGetState");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, state);
}

DWORD WINAPI ProxyXInputSetState(DWORD user, XINPUT_VIBRATION* vibration)
{
    static const auto real = Real<decltype(&ProxyXInputSetState)>("XInputSetState");
    const auto fn = Target(real, "XInputSetState");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, vibration);
}

DWORD WINAPI ProxyXInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    static const auto real = Real<decltype(&ProxyXInputGetCapabilities)>("XInputGetCapabilities");
    const auto fn = Target(real, "XInputGetCapabilities");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, flags, caps);
}

void WINAPI ProxyXInputEnable(BOOL enable)
{
    static const auto real = Real<decltype(&ProxyXInputEnable)>("XInputEnable");
    const auto fn = Target(real, "XInputEnable");
    if (!fn) return;
    const Depth depth;
    fn(enable);
}

DWORD WINAPI ProxyXInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture)
{
    static const auto real = Real<decltype(&ProxyXInputGetDSoundAudioDeviceGuids)>("XInputGetDSoundAudioDeviceGuids");
    const auto fn = Target(real, "XInputGetDSoundAudioDeviceGuids");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, render, capture);
}

DWORD WINAPI ProxyXInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info)
{
    static const auto real = Real<decltype(&ProxyXInputGetBatteryInformation)>("XInputGetBatteryInformation");
    const auto fn = Target(real, "XInputGetBatteryInformation");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, type, info);
}

DWORD WINAPI ProxyXInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE* keystroke)
{
    static const auto real = Real<decltype(&ProxyXInputGetKeystroke)>("XInputGetKeystroke");
    const auto fn = Target(real, "XInputGetKeystroke");
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, reserved, keystroke);
}

// The unnamed ones: XInputGetStateEx (the guide button too), the guide button
// wait and its cancel, a controller's power off.
DWORD WINAPI ProxyOrdinal100(DWORD user, void* state)
{
    static const auto real = Real<decltype(&ProxyOrdinal100)>(MAKEINTRESOURCEA(100));
    const auto fn = Target(real, MAKEINTRESOURCEA(100));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, state);
}

DWORD WINAPI ProxyOrdinal101(DWORD user, DWORD flags, void* event)
{
    static const auto real = Real<decltype(&ProxyOrdinal101)>(MAKEINTRESOURCEA(101));
    const auto fn = Target(real, MAKEINTRESOURCEA(101));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user, flags, event);
}

DWORD WINAPI ProxyOrdinal102(DWORD user)
{
    static const auto real = Real<decltype(&ProxyOrdinal102)>(MAKEINTRESOURCEA(102));
    const auto fn = Target(real, MAKEINTRESOURCEA(102));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user);
}

DWORD WINAPI ProxyOrdinal103(DWORD user)
{
    static const auto real = Real<decltype(&ProxyOrdinal103)>(MAKEINTRESOURCEA(103));
    const auto fn = Target(real, MAKEINTRESOURCEA(103));
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    const Depth depth;
    return fn(user);
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
