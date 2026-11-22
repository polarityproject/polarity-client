# Polarity client docs

| Doc | What it covers |
| --- | --- |
| [testing.md](testing.md) | unit tests, two game instances on one PC, two PCs, the test scripts and commands |
| [commands.md](commands.md) | the mod's commands for tests and research, by area |
| [netplay.md](netplay.md) | how a session runs, from the lobby to the end |
| [overlay.md](overlay.md) | the mod's screens: drawing into the game's frames, input, the hub and rooms |
| [local-input.md](local-input.md) | how the game reads the local input; virtual pads, the pause filter and the game's window |
| [battle-driver.md](battle-driver.md) | the battle's frame loop under a driver: random state, the job order, sounds |
| [snapshot.md](snapshot.md) | the game state the mod saves and loads: its regions, what it adds, what it masks |
| [flow.md](flow.md) | the game's scenes, battles and tables as the mod follows and drives them |
| [desync-replays.md](desync-replays.md) | match recordings (`.sfvr`), desync reports, the `polarity` tool, replaying a recording in the game |
| [keeper.md](keeper.md) | polarity-keeper: rooms, the public list, tables, spectator relays |
| [rooms-client.md](rooms-client.md) | the hub's rooms connection: listings, the desk, rooms, table sets, watching |
| [replays.md](replays.md) | the replay driver: run modes, checks, the Battle Log's viewer, Replay Takeover |
| [replay-versioning.md](replay-versioning.md) | which recordings play in the Battle Log, and the replay regression check |
| [rollback-presentation.md](rollback-presentation.md) | the round's stages and cinematic cameras under rollback, online and in the Battle Log |
| [input-delay.md](input-delay.md) | how a session's input delay is suggested |
| [frame-meter.md](frame-meter.md) | the frame meter: what it shows and how it reads the game |
| [hitbox-display.md](hitbox-display.md) | the hitbox display: where the boxes come from and how they're drawn |
| [updater.md](updater.md) | updates from the site, the release key, and making a release |
| [startup.md](startup.md) | the start: loading, the offline start, settings, moving an SFVR install, Steam content, crash reports, report uploads, logs |

Building and the source layout are in the [README](../README.md).
Changes by version are in the [CHANGELOG](../CHANGELOG.md).

Some source comments cite design notes by file name (`custom-rooms-plan.md`,
`online-lobby.md`, "netplay-testing.md, finding N"). Those are the
maintainers' working notes and aren't published; what's still true from them
is in the docs above.
