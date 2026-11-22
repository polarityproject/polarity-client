# Local input

How the game reads the local player's input, and how the mod's
`src/mod/local_input.cpp` steps in: virtual pads for tests, the input a
netplay match sends, the pause filter and the game's window.

## The game's input manager

The input manager (rva 0x3A750C0) holds two pads of 0x200 bytes behind the
pointer at its +8. Once per frame its update (rva 0x4DC7D0) maps the
keyboard and the pads through the player's Button Settings. Per pad:

| Offset | What it holds |
| --- | --- |
| +0x04 | the pad keys down: bit k is `EKWKeyConfigPadKeyId` k (up 1, down 2, left 4, right 8, sticks included; X 0x10, Y 0x20, A 0x40, B 0x80, LB 0x100, RB 0x200, LT 0x400, RT 0x800, Start 0x4000, Back 0x8000) |
| +0x08 | the keys pressed this frame |
| +0x0C | the keys released this frame |
| +0x14 | the value the battle reads (rva 0x4CED60 through 0x4CEDF0; the InputUnit's values come from it, rva 0x443D20) |
| +0x20, +0x24 | the stick |
| +0x30 | the live Button Settings: 16 words, what each pad key gives as default-layout keys with shortcuts expanded (default X 0x10, LB 0x230 for 3P) |

The value at +0x14 is the keys after the player's Button Settings, each
function as the default layout's keys, shortcuts expanded (3P on LB is
X+Y+RB). A netplay match sends this value, as the battle reads it offline.
The keys at +0x04 skip the Button Settings, so shortcut keys and custom
layouts (keyboard players have both) would come out wrong online. The local
player plays with pad 0, keyboard included.

## A netplay match

The battle must see only the injected inputs: the game reads local input
past the InputUnit, and each PC would see its own player's pad. So while a
match runs the manager's pads are cleared right after each update and put
back just before the next one, because the update derives pressed and held
from them.

The frame's pacing wait (pacing.h) runs just before the pads are read, so
it adds no input lag.

While the overlay takes the input (its panel, the post-match vote), the
match gets none of it, and the game's menus behind it see none either. The
pads' keys for the overlay come from the input manager, so every pad the
game reads works there (XInput, DirectInput, Steam Input), not only XInput
pads.

### Start and the pause

A pause on one side of a match would desync it. During a match Start and
Back are taken out of every pad's state, and the window procedure swallows
Enter and Escape. Start still reaches the match as this player's input,
from any pad or the keyboard's Enter: the intro skip needs both players to
press it.

## Virtual pads

Tests press virtual pads (`pad` in [commands.md](commands.md)). They are
composed with the real pads in the XInput hook: their buttons are added,
the triggers take the larger value and the stick its own value. A scripted
pad (the stress and sync tests) replaces a pad's state, and expires 500 ms
after its last update so a stopped driver cannot keep pressing.

The XInput hook sits where the game's import leads (the proxy's export),
ahead of the Steam overlay's patch there: Steam answers for its own pads
without calling on, so hooked further in they never showed. Without the
import it hooks the real XInput.

The proxy (`src/proxy/xinput_proxy.cpp`) exports `XInputGetState` and
`XInputSetState` by ordinal (2 and 3) as `XINPUT1_3.dll` does. That DLL is
not a KnownDLL and no system DLL preloads it, so the copy next to the exe is
the one the loader binds. Every export passes the call on to the real XInput
at its original ordinal (`real_xinput.h`: System32's `xinput1_3`, or Wine's
`xinput1_4`), the unnamed ordinals 100 to 103 included (`XInputGetStateEx`
with the guide button, the guide button wait and its cancel, a
controller's power off). An export the real XInput lacks answers "not
connected". They were linker forwards to System32 until Proton loaded that
one as the useless native copy. A call that comes back into the proxy while
one is still running on the thread came through a hook on the real XInput
(Special K's), and goes to the fallback; a third time round it goes nowhere
("not connected").

The game only polls pads it has seen connected. After a hot reload it has
seen the virtual pads disconnect, so a device change message makes it
enumerate them again.

## The window

The mod's window procedure runs ahead of the game's:

- the overlay first: what it takes, the game does not see (raw input still
  goes to `DefWindowProc`, which releases it);
- the game captures the mouse at start and lets it go when it sees the mouse
  move. With the overlay up it sees none, so the overlay's mouse releases
  the capture, or a click on the title bar reaches the game;
- with `always_active`, deactivation messages are dropped and the focus
  functions answer with the game's window, so two instances on one desktop
  both keep reading their pads;
- on close the session says Bye at once, instead of the others waiting for
  the silence timeout, and a fault from then on is the game's exit, not a
  crash to report.

The game sets its own window title for a while after starting, so the mod
sets its own again every second.

The window's title shows the instance and the mod's status
(`StreetFighterV [sfv N] ...`), converted from UTF-8 so names in any script
show. It is read with `InternalGetWindowText`, since `GetWindowText` would
send `WM_GETTEXT` to the game thread, which may be waiting for the status
lock, and set with `SendMessageTimeout`, since the window thread may be
busy.
