# Polarity client docs

| Doc | What it covers |
| --- | --- |
| [testing.md](testing.md) | unit tests, two game instances on one PC, two PCs, the test scripts and commands |
| [desync-replays.md](desync-replays.md) | match recordings (`.sfvr`), desync reports, the `polarity` tool, replaying a recording in the game |
| [replay-versioning.md](replay-versioning.md) | which recordings play in the Battle Log, and the replay regression check |
| [rollback-presentation.md](rollback-presentation.md) | the round's stages and cinematic cameras under rollback, online and in the Battle Log |
| [input-delay.md](input-delay.md) | how a session's input delay is suggested |
| [frame-meter.md](frame-meter.md) | the frame meter: what it shows and how it reads the game |
| [hitbox-display.md](hitbox-display.md) | the hitbox display: where the boxes come from and how they're drawn |
| [updater.md](updater.md) | updates from the site, the release key, and making a release |

Building and the source layout are in the [README](../README.md).
Changes by version are in the [CHANGELOG](../CHANGELOG.md).

Some source comments cite design notes by file name (`custom-rooms-plan.md`,
`online-lobby.md`, "netplay-testing.md, finding N"). Those are the
maintainers' working notes and aren't published; what's still true from them
is in the docs above.
