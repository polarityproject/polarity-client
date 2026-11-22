# Testing

How to run Polarity's tests: the unit tests, two game instances on one PC,
two PCs, and the replay regression check. For desync reports and replays,
see [desync-replays.md](desync-replays.md).

## Unit and end-to-end tests

```bat
build.cmd
build\Release\sfvtests.exe
```

The unit tests (`tests/`, one file per area, built as `sfvtests`) cover
the core library: recordings and their tools, the update info and its
signature, compression, input delay, rules and the keeper protocol. They
also play full netplay matches between two `MockGame`s
(`src/testbed/`), a deterministic stand-in fighter, over a lossy, jittery
loopback network. Each injected bug must be caught by the desync tooling.

## Two game instances on one PC

1. Build, then copy `build\Release\xinput1_3.dll` and
   `build\Release\polarity.dll` to `StreetFighterV\Binaries\Win64\`, with a
   `steam_appid.txt` holding `310950` next to them.
2. In `polarity.ini` there, set `multi_instance=1`, `virtual_pads=2` and
   `always_active=1` (see `polarity.ini.example`). The test scripts set these
   for their run themselves and put the player's values back afterwards.
3. Start a local rooms server: `scripts\weyve-server.ps1` (`-Stop` ends it).
4. `scripts\up.ps1 -Restart` starts both games and waits for their main
   menus. Each instance takes the lowest free number N: its window title
   shows `[sfv N]`, it logs to `Polarity\logs\polarity.N.log` (no number
   for instance 0) and reads commands from `sfv_cmd.N.txt`.
5. Send commands with `scripts\cmd.ps1 -Instance N -Commands "..."`, for
   example `host` on instance 0 and `join <code>` on instance 1.

### Commands

The mod reads commands from `sfv_cmd[.N].txt` next to the game. Separate
several with `;`. The common ones:

| Command | What it does |
| --- | --- |
| `host [random [costumes]]`, `join <code>`, `spectate <code>`, `leave` | sessions; from commands both sides are ready at once |
| `delay <frames>` | this player's input delay for the next session |
| `pick <code> <costume> <color> <comment> <vskill> <vtrigger>` | this player's pick |
| `ready [0\|1]`, `choose rematch\|select` | in the lobby; after a match |
| `netsim <latency ms> <jitter ms> <loss %>` | simulated conditions on what this side sends |
| `mash <seed> [from frame]` | the local player mashes (0 = off) |
| `battlenow`, `setup ...`, `nextstage <code>` | straight into a local battle, with a match setup and stage |
| `replay <file> [stress <n>]`, `replay stop` | play a recording (see desync-replays.md) |
| `preload <code> [costume]` | load a character ahead of a battle |
| `pad <user\|both> <buttons> [hold ms] [gap ms]` | virtual pad presses |
| `menu` | back to the main menu from anywhere |
| `echo <text>` | a marker in the log, for scripts waiting on earlier steps |
| `update check`, `reports send`, `crash <kind>` | the updater, report uploads and crash reports, for testing |
| `badpick <field\|off> [message]` | what this side sends carries a pick or stage no game has, so the other side must refuse it and end the session. Fields: `character`, `costume`, `color`, `vskill`, `vtrigger`, `comment`, `variant`, `stage`. Messages: `state`, `session`, `welcome`, `loaded`, `matches` (every one when left out) |
| `pickcheck` | runs every pick the select screen offers, and every selectable stage, through the check on other PCs' picks, and logs any it refuses |

[commands.md](commands.md) has the full list, including the research
commands the other docs mention.

### Scripts

Scripts share `scripts/lib.ps1`. Its helpers throw rather than let a script
go on with a stale value:
- `Use-TestSettings` applies the test ini values for a block and restores
  them, even on failure;
- `Start-Games` starts the games and restarts one that exits before the menu;
- `Send-Cmd -Wait` returns once the commands have run;
- `Start-Host` returns the room code, and `Start-Join` waits for the session.

| Script | What it does |
| --- | --- |
| `session.ps1 [-Choices rematch,select] [-Spectate 2]` | one session of several matches between instances 0 and 1, both mashing, with an optional spectator; prints each side's course and any desync |
| `fixed-match.ps1` | one Ryu vs Ken match; where the two recordings first differ |
| `nettest.ps1 -Rounds N -Seconds S` | random setups and network conditions per side; diffs each pair of recordings |
| `loop-matches.ps1 -Count N` | short matches in a row, checking each start and return to the menu |
| `rematch-test.ps1` | the post-match vote, with screenshots |
| `scan-loadouts.ps1` | learns checksum noise for every character and V-Skill / V-Trigger |
| `scan-characters.ps1` | per character, a local battle and a netplay match: did the starting states match |
| `load-times.ps1`, `load-profile.ps1` | battle load times; file reads and CPU per thread |
| `symbolize.py <dll> <base> <address>...` | function and line for crash stack addresses |

## Two PCs

`scripts\agent.ps1 -Token <secret>` on the second PC lets scripts on the
main PC drive its game over the LAN (`scripts\remote.ps1`). The token is a
shared secret you choose per test run; don't commit it. Then:

- `two-machine.ps1 -Agent <ip> -Token <secret> -Server <this PC's ip> [-Deploy]`
  plays a session between the two PCs and compares each match's recordings
  with `polarity peers`. `-Deploy` copies this build to the other PC first.
- `layout-test.ps1` checks that each PC's own button layout reaches the
  other as that player pressed it.

On SteamOS, `scripts/agent_steamos.py` speaks the same protocol.

## The determinism test

`synctest <frames> [seed]` (`synctest.cpp`) runs a stretch of battle twice
from the same saved state and compares each frame's hashes. Pass 1 records
them; pass 2 restores the state, treats the restored tick as frame 0, and
replays the same inputs. Each pass starts and ends with 12 neutral frames,
so the pads hold the same state when each pass starts and edge detection
outside the snapshot sees the same history. Every pass-1 state is kept for
byte diffs only in tests of 240 frames or fewer. Each tick should see
exactly one pad read.

The inputs come from the seed: directions, and a third of the time an
attack (LP, MP, HP, LK, MK on the default layout, HK on the right
trigger). Seed 0 keeps both pads neutral, which separates the state the
inputs drive from the rest. At the first frame that differs the test lists
the regions that differ, then shows how the difference evolves at a few
sample frames (the next two, frames 30 and 100, and the last), each with
the bytes from 8 before the difference in both passes.

## Rooms without the game

`polarity-bot` (`src/tools/polarity_bot.cpp`) is a headless rooms player. It
finds the lobby channel, makes or joins a custom room and runs its script,
without the game:

```
polarity-bot --name Bot1 [--server 127.0.0.1:5555] [--auto] [--set-ms 3000] [--wins p1|p2|random]
             [--version v] [--hub | --browse] [--script "create 2 1 0; wait 1000; sit 0 0; ..."]
```

With `--auto` it answers its table calls (Ready, after a short random
pause), opens a made-up match room when it is player 1, and reports the
set's result `--set-ms` later, so bots cycle a table's rotation by
themselves. `--version` sets the version it gives as member data (`''` for
none).

Between rooms it waits at the keeper's desk by default, where it can ask for
rooms. `--hub` and `--browse` wait in no room at all, as the game does:
`--hub` polls the desk's listing every 5 s (the hub open), and `--browse`
every 2 s, listing the public rooms whenever the desk's revision moves (the
rooms screen open, without its desk). It asks for one listing at a time, a
second apart, which is the server's limit.

Commands come from the script, then from stdin, so a test can drive a bot
live; `;` or a newline separates them:

| Command | What it does |
| --- | --- |
| `create <tables> <public 0\|1> <title>` | makes a room, for example `create 5 1 3,RYU,KEN` |
| `join <code>`, `joinpublic`, `leave` | joins by code, joins the first room the server's listing has (the keeper's order), leaves |
| `sit <table> <seat 0\|1\|-1>`, `stand`, `queue <table>`, `unqueue`, `ready <table>`, `decline <table>`, `rules <table> <rules...>` | table requests, sent as typed: what the keeper answers to one it refuses is the test |
| `kick <member id>`, `kickname <name>` | the owner's kicks |
| `chat <text>`, `flood <n>` | a chat line, or n lines at once (10 by default) |
| `chara <code>` | the character others see beside the name (member data `ch`) |
| `setdata <key> <value>` | any member data, to test what the server refuses (such as `@cc`) |
| `forge` | a room of its own, listed with a real room's signed listing copied on. The game must not show it, since the signature names the other room. It needs a room list first (`joinpublic` or `--browse`). |
| `watch <table>`, `unwatch` | a headless viewer: each set of the table through the keeper's relay room, from its first game, played at 60 frames a second. Like the game's table viewer it waits for 120 frames in hand before playing, and again after running dry, and logs each game and the set. |
| `status`, `wait <ms>`, `quit` | the bot's state, a pause, the end |

The bot logs one line per happening on stdout (`bot Bot1: ...`). It answers
players' pings as the game does, since the room's player list shows the
round trip, and its room carries its version as a game's does.

`weyve_probe` (`src/tools/weyve_probe.cpp`) checks a Weyvelength server and
the peer to peer path without the game. `weyve_probe host [server[:port]]`
creates a room, prints its code and echoes every datagram back until the
room empties. `weyve_probe join <code> [server[:port]]` joins, pings the
host 20 times over the peer to peer link and prints the round trips. The
server defaults to `127.0.0.1:5555`. Both raise the timer resolution to
1 ms, as the mod's network thread has it.

## The replay regression check

`scripts\replay-regression.ps1` replays the reference recordings in
`tests/replays/sim-<n>/` on the second PC, straight and under rollback
stress. See [replay-versioning.md](replay-versioning.md) and
[tests/replays/README.md](../tests/replays/README.md).

## Fuzzing

`scripts\fuzz.ps1 [-Seconds 300] [-Target replay|xpress|all]` builds the
libFuzzer targets in `tests/fuzz/` with MSVC's AddressSanitizer and runs
each for the given time. `replay` reads recordings from any bytes, starting
from the reference recordings; `xpress` runs the XPRESS Huffman decoder,
starting from their packed chunks. An input that crashes is saved in
`build\fuzz\<target>\` as a `crash-*` file. It needs the Build Tools' C++
AddressSanitizer component.
