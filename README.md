# Polarity

Polarity is unofficial online play for Street Fighter V: rollback netplay
built on the game's local Versus mode and its own rollback manager, using
[GekkoNet](https://github.com/HeatXD/GekkoNet) for the session and
[Weyvelength](https://github.com/HeatXD/Weyvelength) rooms for finding
players, with peer-to-peer links (or TURN relays when two players can't link
directly). The game's online code (ProudNet, Capcom's servers, CFN) is never
used.

It's in public beta. Download it and read the install steps at
<https://polaritymod.link>. Installed copies update themselves from there:
Polarity offers each new version when the game starts.

## What it does

- **Custom rooms**: public or private rooms with tables, rules (rounds,
  timer, stage), spectators, chat and room moderation, listed per Polarity
  version. Direct Match by code, too.
- **Rollback netplay** with a fixed input delay each player chooses (with a
  suggestion from the ping), checksums every frame and desync reports.
- **The game's own select screen and presentation online**: intros, round
  calls, KOs and win poses, with Start to skip once both players press it.
- **Battle Log**: every match is recorded. Watch it back, step through it
  and take over a replay at any point.
- **Training and replay tools**: a frame meter, a hitbox display, and an
  online benchmark that checks your PC and battle data.
- **Offline boot**: the game goes straight to its main menu with no Capcom
  login. The profile gets the DLC content the Steam account owns.

Polarity checks the game's exe and the code at every address it uses, and
does nothing on a mismatch. Delete `xinput1_3.dll` and `polarity.dll` from
the game's `Binaries\Win64` folder to uninstall it.

## Repositories

| Repository | What it holds |
| --- | --- |
| [polarity-client](https://github.com/polarityproject/polarity-client) (this one) | the mod (`polarity.dll`), its loader (`xinput1_3.dll`), the core library and its tests, the `polarity` command-line tool, the rooms keeper, test bots and the test and release scripts |
| [polarity-installer](https://github.com/polarityproject/polarity-installer) | the Windows and Linux installers |
| polarity-server | the rooms server's Docker stack, the reports service and the rooms API |
| polarity-web | the website at polaritymod.link and the update info the updater reads |
| polarity-discord | the Discord bot that lists the public rooms |

The Weyvelength commit is pinned in two places that must match: `GIT_TAG` in
`CMakeLists.txt` here and `WEYVELENGTH_COMMIT` in polarity-server's
`docker/server.Dockerfile`.

## Layout

```
src/core/      game-agnostic: IGame, netplay session, GekkoNet bridge,
               .sfvr recordings, verifier, diff, update info
src/mod/       polarity.dll: the game hooks, SfvGame, sessions, rooms,
               the overlay UI, Battle Log, frame meter, updater
src/proxy/     xinput1_3.dll (and version.dll), which loads polarity.dll
src/keeper/    polarity-keeper: the server-side table caller for rooms
src/testbed/   MockGame (a deterministic fighter with switchable bugs) and
               a loopback network with latency, jitter and loss
src/tools/     the polarity CLI, polarity-bot (headless room members),
               weyve_probe
tests/         unit and end-to-end tests; tests/replays/ is the replay
               regression corpus
scripts/       PowerShell helpers: two instances on one PC, two PCs, soak
               tests, the replay regression check, packaging and releases
data/          the checksum noise mask, battle data hashes and the
               benchmark round, all built into the DLL
```

## Build

Needs Visual Studio 2022 Build Tools (C++ x64, CMake, Ninja) and network
access on the first configure (dependencies are fetched at pinned commits).

```bat
build.cmd
build\Release\sfvtests.exe
```

To try a build in the game, copy `build\Release\xinput1_3.dll` and
`build\Release\polarity.dll` to `StreetFighterV\Binaries\Win64\` (next to the
64 MB exe, not the launcher). Settings live in `polarity.ini` there;
`polarity.ini.example` lists them. Logs, recordings and reports go to
`Polarity\` in the same folder.

## Desync tooling without the game

```bat
build\Release\polarity.exe demo demo
build\Release\polarity.exe diff demo\machine-value_p1.sfvr demo\machine-value_p2.sfvr
build\Release\polarity.exe verify demo\machine-value_p2.sfvr --mock-bug machine-value
```

`polarity diff` compares two players' recordings of one match and names the
first frame and state region that differ.

## Changes

See [CHANGELOG.md](CHANGELOG.md).

Polarity isn't affiliated with or endorsed by Capcom.
