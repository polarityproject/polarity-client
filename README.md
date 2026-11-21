# Polarity

Polarity is unofficial online play for Street Fighter V. It's in public
beta: download it and read the install steps at <https://polaritymod.link>.
Once it's installed, Polarity offers each new version when the game starts.

Matches use rollback netplay. Polarity builds on the game's local Versus mode
and the rollback manager the game already has, runs the session with
[GekkoNet](https://github.com/HeatXD/GekkoNet), and finds players through
[Weyvelength](https://github.com/HeatXD/Weyvelength) rooms. Players connect
peer to peer, or through a TURN relay when they can't reach each other
directly.

## Features

- Custom rooms, public or private, with tables, rules (rounds, timer,
  stage), spectators, chat and moderation. You only see rooms on your own
  Polarity version. You can also Direct Match someone by code.
- Rollback netplay with a fixed input delay. Each player picks their own,
  and Polarity suggests one from the ping. Every frame is checksummed, and
  desyncs produce a report.
- The game's own select screen, intros, round calls, KOs and win poses,
  online. Start skips them once both players press it.
- A Battle Log of every match. Watch a match back, step through it, or take
  over the replay at any point.
- A frame meter and a hitbox display in Training and replays, and an online
  benchmark that checks your PC and your battle data.
- The game boots straight to its main menu, and your profile gets the DLC
  content your Steam account owns.

Polarity checks the game's exe and the code at every address it uses. If
anything doesn't match, it does nothing. To uninstall it, delete
`xinput1_3.dll` and `polarity.dll` from the game's `Binaries\Win64` folder.

## Repositories

| Repository | What's in it |
| --- | --- |
| [polarity-client](https://github.com/polarityproject/polarity-client) (this one) | the mod (`polarity.dll`) and its loader (`xinput1_3.dll`), the core library and its tests, the `polarity` command-line tool, the rooms keeper, test bots, and the test and release scripts |
| [polarity-installer](https://github.com/polarityproject/polarity-installer) | the Windows and Linux installers |
| [polarity-server](https://github.com/polarityproject/polarity-server) | the rooms server's Docker stack, the reports service and the rooms API |
| [polarity-web](https://github.com/polarityproject/polarity-web) | the website at polaritymod.link, and the update info the updater reads |
| [polarity-discord](https://github.com/polarityproject/polarity-discord) | the Discord bot that lists the public rooms |

The client and the server have to run the same Weyvelength commit. It's
pinned in two places: `GIT_TAG` in this repo's `CMakeLists.txt`, and
`WEYVELENGTH_COMMIT` in polarity-server's `docker/server.Dockerfile`.

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

## Building

You need Visual Studio 2022 Build Tools (C++ x64, CMake, Ninja). The first
configure also needs network access, because it fetches the dependencies at
pinned commits.

```bat
build.cmd
build\Release\sfvtests.exe
```

clang-format sets the layout of the C++ sources (`.clang-format`). It comes
with the Build Tools' "C++ Clang tools" component. `scripts\format.ps1`
formats `src\` and `tests\`, and `scripts\format.ps1 -Check` lists the files
that are not formatted.

To try a build in the game, copy `build\Release\xinput1_3.dll` and
`build\Release\polarity.dll` into `StreetFighterV\Binaries\Win64\`, next to
the 64 MB exe (not the launcher). Settings go in `polarity.ini` in that
folder, and `polarity.ini.example` lists them all. Logs, recordings and
reports end up in `Polarity\` beside it.

## Desync tools without the game

```bat
build\Release\polarity.exe demo demo
build\Release\polarity.exe diff demo\machine-value_p1.sfvr demo\machine-value_p2.sfvr
build\Release\polarity.exe verify demo\machine-value_p2.sfvr --mock-bug machine-value
```

`polarity diff` takes both players' recordings of one match and tells you the
first frame where they disagree, and which part of the game state differs.

## More

- [docs/](docs/README.md): testing, desync reports and replays, how the
  Battle Log decides which replays it can play, rollback of the round's
  stages, input delay, the frame meter, the hitbox display and the updater.
- [CHANGELOG.md](CHANGELOG.md): what changed in each release.

## License

Polarity is under the [MIT License](LICENSE). The libraries and fonts built
into it keep their own licenses, listed with their full texts in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which releases ship as
`polarity_third_party_notices.txt`.

Polarity isn't affiliated with or endorsed by Capcom.
