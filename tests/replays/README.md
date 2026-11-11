# Replay regression corpus

A recording plays in the Battle Log only on the same game exe and the same
**sim version** (`src/mod/sim_version.h`, the `/sim-<n>` part of a build id;
`core/build_id.h`). The sim version is bumped by hand, and only when the mod
changes what a battle computes from the same start and inputs. These
recordings check that nothing else does.

`sim-<n>/` holds the reference recordings for sim version n: P1's file of a
live match each, made by test players (no real players' names). sim-12 came
from two-PC matches; sim-13 (2026-11-11) from two game instances on one PC
in a session through the local rooms server, input delay 0, both mashing
(the time-over: both idle, so it has no rollbacks).
Together they cover every character, plus:
- Zangief's throw cinematics, with rollbacks during them;
- a game 2 of a Direct Match set (battle to battle, Laura vs Necalli: the stage transition KO);
- a time-over.

The files are thinned with `sfvr thin <in> <out> 3000`: the start state and a state dump every 3000 frames.

Two lists sit next to the recordings:
- `quick.txt`: the `-Quick` subset.
- `stress.txt`: the recordings `-Stress <n>` also runs under stress 8.

## Running it

```
.\scripts\replay-regression.ps1 -Token <agent token>           # all, about 45 minutes
.\scripts\replay-regression.ps1 -Token <agent token> -Quick    # about 16 minutes
```

The check runs on the second PC (TestPC's agent; `-Agent` for another).
- Each recording replays straight and must PASS against its own checksums.
- They are checked with the recording's masks, so a change to the masks is no FAIL.
- Results go to `build\regression\<time>\`.
- The script exits with 1 on any FAIL.

Run it before every release, and after any change near the battle setup,
the driver's ticks, seeding or input handling.

## When it fails

A FAIL on an unchanged sim version is one of two things:
- a regression (fix it);
- an intended change to what battles compute. Then:
  1. Bump `kSimVersion` and say why in its history line.
  2. Make a new `sim-<n+1>/` from fresh live matches: the two-PC pairs runs, every character, plus the cases above.
  3. Thin the files and write its `quick.txt` and `stress.txt`.
  4. Keep the old folder until no supported build has that sim version.

Players' recordings from before the bump then show "can't be played on this version" in the Battle Log, as they should.
