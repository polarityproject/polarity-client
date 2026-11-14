# The round's stages and cameras under rollback

A battle's simulation is in the game's rollback snapshot, but a round also
has stages run by objects outside it: the intro, the round call up to
"FIGHT!", the KO or time-over, stage-transition KOs, the reset between
rounds, and the match's last KO and win poses. Cinematic cameras live
outside it too. This doc covers how Polarity keeps them right when a
rollback loads an earlier frame, both in online sessions and in the Battle
Log viewer.

Menus after the match (results, rematch) are out of scope: they read each
game's own pads and the session treats them as decided.

## What lives outside the snapshot, and how it follows a load

| State | Where | Follows loads by |
| --- | --- | --- |
| Round director (state, stop record, timers, counts) | director task object | the `flow.round` snapshot region |
| Battle phase machine and its phases, with each phase's timer | task at rva 0x3DD890 | per-tick copies (64 ticks), Battle Log keyframes |
| Round-over writes (round time, count, history, results) | battle context +0x1904 to +0x78798 | round-over journal (sessions), keyframes |
| Latest round's record (+0x787AC, 0x10FC8 bytes). The fight phase starts the round's end when its +0x787B8 is set | battle context | round-over journal (sessions), keyframes |
| Round-end wait object: armed by the KO, read by a stage-transition KO as it plays | KO-wait object | per-tick copies of its tail (+0xF8 to +0x140) |
| Input recorder head | global | per-tick copies, keyframes |
| Start skip presses | `flow.round` | the snapshot |
| Whether the round call is running | phase machine | read from the phase machine, so a load leaves it right |
| Camera view targets (cinematic and KO cameras) | camera managers | the camera journal, below |
| Banners (ROUND 1, FIGHT, KO, TIME OVER) | HUD | read each frame from the round's state |
| Sounds and the announcer | sound manager | the game's own sound rollback |

## Online sessions

Two settings under `[netplay]` in `polarity.ini` decide where sessions
predict and roll back:

| Setting | Default | Effect |
| --- | --- | --- |
| `lockstep_round_transitions` | 1 | no prediction from a KO to the next round's FIGHT, nor in round 1's call. A rollback across them re-ran the round's end under the HUD, which isn't rolled back: round win markers doubled or landed on the wrong side, and the FIGHT banner snapped back to "ROUND n". 0 lets them roll back |
| `lockstep_round_stages` | 0 | 1 puts every stage outside the fight in lockstep, paced by the round trip |

With the defaults, the fight and the match's opening intro roll back, and
the round transitions run in lockstep.

## Cinematic cameras

Throws with a camera (SPD, Borscht Dynamite) and Critical Arts move the view
to a camera actor with a timed blend, and back to the fighter with another.
The game's own load callback (rva 0x330DB0) cancels any view-target blend in
progress after every snapshot load and puts the view straight back on the
fighter. A load during a blend therefore cut the camera at once. The ticks
replayed after the load don't undo it: a blend started before the loaded
frame is never asked for again, and one asked for again starts over.

The fix is in `src/mod/camera_rollback.{h,cpp}`:
- `APlayerCameraManager::SetViewTarget` is hooked, and 0x330DB0 is left out
  of loads.
- Requests made during the driver's ticks are journaled by tick. A load sets
  aside those after the loaded tick.
- When a replayed tick makes the same request again (same tick, target and
  transition), it is let off while its effect is still on screen. Otherwise
  it is made again, with its blend moved on by the ticks since.
- Requests that aren't made again are forgotten. The game's camera task
  itself asks again for the view the loaded state has, which is how a throw
  that didn't happen goes back to the fight camera.

`[netplay] camera_rollback=0` (or the `camrollback 0` command) restores the
game's own behaviour. `camlog 1` logs every request.

## The Battle Log viewer

- **Keyframes** hold what a load needs beside the snapshot: the phase
  machine, the round-end wait, the input recorder's head and the battle
  context's round history and latest record, in 4 KB chunks shared between
  keyframes while unchanged (`OutsideState` in `src/mod/flow.h`).
- **Keyframes from the first FIGHT on load from anywhere:** across round
  ends, between fights and after the match's last KO. Seeks, round jumps and
  Takeover rewinds load them instead of restarting the battle.
- **Keyframes from the opening** (before the first FIGHT) restart the battle
  instead. Loaded after later rounds were played, the round's setup filled
  both fighters' vitality from a source the keyframe doesn't hold, for one
  tick. Sessions never load that far back.
- **A replay starts at round 1's call.** The intro runs muted behind black,
  then the picture fades in on the call. This isn't a Start skip, which
  would play a different battle from the recording's.
- **Round jumps:** RB goes to the next round's call. LB goes to the previous
  round's call from this round's call or its fight's first second, and to
  this round's call otherwise.
- **HUD banners:** the battle HUD is Scaleform. The game advances each
  movie once per rendered frame by real time, so ticks run several to a
  frame through a round's call left its banner on screen. The viewer
  advances the banner and gauge movies by 1/60 s before each extra tick
  through a round's call and its fight's first 120 frames
  (`src/mod/hud_step.cpp`).

## Tests

- Every regression corpus recording passes under rollback stress 8, and with
  P2's inputs 7 frames late (`replay-regression.ps1 -StressAll`,
  `-AllMode 'mispredict 7 remote 2'`).
- Viewer seeks into every stage of a full recording: no frame differs.
- Live sessions between two PCs at 110–200 ms with rollbacks through the
  round stages, rematches with a late spectator, and time-overs: no desyncs.
- Camera values with late inputs match the straight replay, apart from under
  2 FOV degrees for a few frames.

## Research commands

| Command | What it does |
| --- | --- |
| `watchphase`, `watchreport` | a hardware write watch on the phase machine's requested phase, and who wrote it |
| `watchname <region> <hex offset> <len>` | a write watch on a snapshot region by name |
| `cinelog 1\|2\|0` | fighters' scripts and cinematic flag per tick; the drawn camera per frame |
| `camtrace <from> <to> [class]` | camera objects' bytes per tick and frame |
| `rview outofsync`, `rview anykeys`, `rview keysanywhere` | viewer research switches |
| `hudstep 0\|1\|2` | the HUD stepping: off, on, banners only |
