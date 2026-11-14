# Replay versioning and the regression check

A match recording plays in the Battle Log only where the build computes the
same battle from it. This doc covers how that's decided and how the replay
regression check guards it.

## Which recordings play

| Part | What it is |
| --- | --- |
| Build id | `sfv-<exe>/adapter-<n>/sim-<n>/map-<hash>/noise-<hash>` (`SfvGame::BuildId`) |
| Playable when | same `sfv-<exe>` and same sim version (`ReplayCompatible` in `src/core/build_id.h`) |
| Sim version | `kSimVersion` in `src/mod/sim_version.h`. It's raised by hand, and only for a change to what a battle computes from the same start and inputs; the header lists what counts |
| Older ids | ids with no `sim-` part use their adapter version (adapter-12 = sim 12) |

The checksum mask parts (`map-`, `noise-`) can differ, so a release that
only changes masks keeps older recordings playable. The Battle Log checks
`ReplayCompatible` in its list, the detail panel and Watch.

## Checking against the recording's masks

When the id's mask parts differ, the replay driver hashes each frame with a
copy of the live region table carrying the recording's masks
(`ReplayDriver::CheckRegions`):
- a region takes the recording's mask for its size when there's only one;
- with several, the one at its place when the sizes line up.

Named regions that a later build adds, and the recording doesn't have, are
left out of the checks, since the recording's checksums were made without
them.

## Out of sync in the Battle Log

When a viewed replay's checksum stops matching, the viewer shows "Out of
sync with the recording from round N" under the round timer
(`ReplayView::outOfSync`). A missed sim version bump is the usual cause.
`rview outofsync` forces the notice, for testing.

## The regression check

`scripts/replay-regression.ps1` plays the reference recordings in
`tests/replays/sim-<n>/` (see [tests/replays/README.md](../tests/replays/README.md)):
- one recording per character, plus Zangief's throw cinematics with
  rollbacks, a game 2 of a set with a stage-transition KO, and a time-over;
- each straight, and a set under rollback stress (every frame loaded from 8
  frames back and replayed);
- `quick.txt` and `stress.txt` pick the `-Quick` and `-Stress` sets.
  `-StressAll` and `-AllMode '<mode>'` run the whole corpus under stress or
  with late inputs.

The files are thinned (`polarity thin <in> <out> 3000`) to keep the corpus
small. Run it before a release that changes anything a battle touches; a
FAIL means either a bug or a sim change that needs a version bump and a new
corpus.
