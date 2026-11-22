# The replay driver

The replay driver plays a match recording back in the game. Every online
match leaves a recording (an `.sfvr` file) on each player's PC. The driver
plays one for three uses:

- the **Battle Log**, the hub's list of past matches, where a player watches
  a match again (the viewer below);
- the **benchmark** in Polarity settings, which plays a built-in round under
  heavy rollback and measures the frame rate;
- **test runs** that check this build plays a recording the same way it
  played when recorded (`replay <file> [stress <distance>] [mispredict
  <delay>]`, see [desync-replays.md](desync-replays.md), with the results
  in the log).

The code is
`src/mod/replay_*.cpp`, with the driver class in `replay_internal.h`:

| File | What it covers |
|---|---|
| `replay_driver.cpp` | starting a replay, the commands, the view the UI reads |
| `replay_runs.cpp` | the start from frame 0 and the run modes (straight, stress, mispredict) |
| `replay_viewer.cpp` | the Battle Log's playback: keyframes, seeks, round jumps, controls |
| `replay_takeover.cpp` | Replay Takeover |
| `replay_checks.cpp` | checks against the recording, watches and the end of a run |

[flow.md](flow.md) has the terms used here (tick, snapshot, rollback,
keyframe, the jump to a battle) and the game state a load restores beyond
the snapshot. A session is an online match between two players
([netplay.md](netplay.md)). A region is one of the memory blocks the
snapshot is made of, and a word one 8-byte value in it; each frame's
checksum covers every region, less the words a mask leaves out.

## Starting a replay

A replay starts the recorded match the way a session does. Both characters
are preloaded first, as they are before a session's match, since assets
that load while the battle runs can put effects out of step. Then the jump
writes the recorded setup and stage. When both players picked the same
character and colours, one of them is shown recoloured, and each PC
records which; the replay shows them as the recording PC did, so the state
before frame 0 matches the recording's. A recording made outside a session
uses the host's (player 1's) side. Neutral ticks run until the
battle's objects exist, and the battle is then frozen at frame 0. The
recording's own bytes are never written into the game: a start that
differs from the recording's is out of sync from frame 0.

A bare file name is looked for in `Polarity\matches`, then in the
`reports` folder older builds wrote next to the game's exe. A recording
from another PC can name a character whose pre-match comment texts are not
loaded yet; the start waits up to 3 s for them.

The game reads the pads once per rendered frame; the mod reads them once
per tick. Recordings from builds before adapter-9 (the adapter version a
recording carries) were made with the game's timing, so a straight run of
one uses it. Runs that roll back always use the mod's, since with the
game's a replayed tick would read a later frame's pad state.

## Run modes

- **Straight:** one frame per update, each compared with the recording.
- **Stress:** every frame loads the state from `distance` frames back and
  runs them again (`stress 8` is the usual test). The newest frame's state, after its resimulation, must
  equal the recording too.
- **Mispredict:** one player's inputs arrive `delay` frames late, as over a
  slow connection. A frame runs on the input the netcode library
  (GekkoNet) would predict: the last one that arrived, or neutral before
  any. When that frame's input arrives and the guess was
  wrong, the state before it is loaded, the game's post-load fix-ups run as
  in a session, and the frames since run again. A frame is checked once its
  input has arrived: live after a rollback, else by the checksum it got when
  it ran on the right input.

The camera's and the characters' animation clocks follow how ticks fall
into rendered frames rather than the ticks themselves, so their
differences are counted as presentation and do not fail a run. A stress
run reports the regions that came out different and on how many frames:
one or two small regions on many frames usually mean a value that only
affects presentation or load timing, many regions a real divergence.

## Checks against the recording

The recording's checksums cover this build's regions, with the recording's
checksum masks where they differ: a later build may leave more or fewer
words out of the checksum while the battle computes the same thing. A
region takes the mask the recording has for its size when there is one;
with several, the one at its place when both tables have the same sizes in
the same order. Regions a later build adds that the recording lacks are
left out of its checks.

A frame with a state dump (a compact recording's every 600 frames, a kept
run's chosen frames) is compared word by word. Each region pairs with the
same-size dump region it differs least from, masked words left out, since
names do not pair them: the canonical order numbers same-size regions by
content, and a battle with one region more or less shifts every name after
it. Regions wholly out of the checksum (pooled effects) pair last. A frame
without a dump is compared by its checksum only, which names no region.

The benchmark keeps the first frame that came out different (or the frame
asked for, `benchmark dump <frame>`) as a recording with one state dump;
`polarity export` reads it, and compared with a good run's dump of the same
frame it names the regions that differ.

## The Battle Log's viewer

The viewer plays a recording as it was, with pause, frame steps both ways,
round jumps and fast forward. It starts at round 1's call (the "ROUND 1"
banner): the intro runs muted and lean behind black, as a round jump's run
does. Skipping the intro with Start would play a different battle than the
recording's, since a Start press is an input.

Keyframes are taken every 60 frames as the replay first gets
there, just after each round's "FIGHT!" and just before each round's call. A step or a jump back loads the
nearest keyframe at or before the frame before the target, runs the frames
between as a rollback runs them (lean and muted), and runs the last one as
it played, so the picture is that frame's. Lean ticks leave out work only
the screen needs, as rollbacks do ([flow.md](flow.md)). The last few frames before the
target run in full, so the camera follows the fight as it played. A jump
forward runs on from where the replay is.

Which keyframes load:

- Any keyframe from the first "FIGHT!" on loads, across round ends, from
  between fights and after the match's last KO.
- Keyframes from the battle's opening, before the first "FIGHT!", do not:
  loaded after later rounds, the round's setup reads the fighters' health
  from game state a keyframe does not hold. A jump back there starts the
  battle again and runs to the target.
- From a round's KO on, the game has put its battle HUD away (the timer and
  the gauges; at the match's end, its results). A keyframe holds the fight's
  state, not the HUD's, so a jump back from there first runs on to the next
  fight, where the game brings the HUD back, then loads; with no fight
  ahead, the battle starts again.

A restart is a new driver with the same view, paused or not as before. It
first stops everything the old battle still has playing (its loops and its
ambience, which the game does not end here), or they play on over the new
one.

The controls (pad buttons; the stripe shows them):

- **Step back:** silent, and only as far as a keyframe of this round loads.
- **Step forward:** only through a round's action frames, never past its
  KO (the KO's own frame is the last). The round's end, the win pose and
  the next round's call play as they did, or a round jump skips them.
- **Round jumps (LB, RB):** a round starts at its call ("ROUND 2"), which
  then plays as it did. RB goes to the next call, while the round on screen
  is not the file's last. LB goes to this round's call, or to the previous
  round's while this one has just begun (its call, or the first second of
  its fight); before round 2, to the replay's start.
- **Fast forward:** only through action frames. The presentation (intro,
  round call, KO, win poses) plays at normal speed.
- **Play (A) at the end of the recording:** plays again from the replay's
  start.

A run behind the black that passes a round's end waits until the KO
screen, which plays in real time, is over. A run takes at most 16 ticks per
rendered frame. With 32 or more, a round jump on the Steam Deck parted from
the recording from one point of a fight on (the camera, the fighters' effect
lists and their simulation objects); 24 still matched. Lean ticks, the
camera rollback and the sound rollback are not the cause: with each turned
off it still parted. Which part of the game's once-a-frame work falls behind
is not known. The game-thread queue does not
run during a driven fight, so the driver runs it itself.

The HUD's banners and timer are movies that advance once per rendered
frame. When a run ticks more than once in a rendered frame, they get each
extra tick's time as well, so a banner plays out as it did. This happens
only where banners play: the round's call ("ROUND n") and the first 120
frames of its fight ("FIGHT!").

`hud_step.cpp` advances the Announce and CockpitTop movies through the
manager's own per-frame advance (rva 0x17CA7AC: the movie at the wrapper's
+0x70, the time, 2 catch-up frames, and no capture, since the manager's
own advance that frame captures for drawing). A movie is stepped only
while its player plays (the native player's wrapper at +0x150, its
playing flag at +0xA6, as `SetPause` at rva 0x17C72F0 uses them).
`hudstep 0|1|2` turns it off, on, or on for the Announce movie only.

The stripe is the viewer's bar along the bottom of the screen, with its
controls and a per-round counter. The counter counts action frames: 0 on a round's
first action frame, its last on the KO's. They are empty before a round's
action and full after its KO. A round whose end is not known yet runs
against the round timer's length. The title is cut short before the frame
counter is, so a long title never runs into the controls. Back shows or
hides the hitboxes and Start the frame meter; holding RT fast-forwards.

A round jump fades the picture out over 0.25 s, asks for the jump once the
screen is black, stays black until the replay has taken it and finished
seeking (30 s at most), then fades in over 0.35 s on the round's call. A
jump pressed while the last one fades back in goes out again from there.
While a jump fades, only B (leave) is taken. A replay's start fades in the
same way on round 1's call. A replay that stopped matching its recording
says so at the top of the screen, from the round where it parted.

## Replay Takeover

From a frame in a round's fight, with the replay paused or playing (not
seeking), a player can take over one side: X takes player 1, Y player 2. The state there is kept as a keyframe is, a 3 s countdown
runs, then each tick takes that side's input from the local pad 0 and
keyboard (with this player's Button Settings) and the other side's as
recorded, until the fight ends by KO or time. Start stops it early. Nothing of the takeover is
checked against the recording or kept as keyframes.

After the fight:

1. The KO screen plays as the game plays it (the slow motion, its screen,
   the voice) with no one's input, for as long as a rollback can still undo
   it: the copies a rollback restores from reach back 64 ticks.
2. The picture fades to black.
3. Behind the black, the replay runs on to the next fight if needed, since
   no keyframe loads the same from between fights. With no fight to come
   (the takeover decided the match), the battle starts again.
4. The KO's frame is loaded as a rollback loads it, undoing the round's end,
   and the takeover's sounds are stopped.
5. The replay goes back to where the takeover began as a step back does: the
   round's last keyframe before it, the recording run on to it, the last
   frames in full, the last as played. Without such a keyframe, the kept
   state itself loads. The replay waits there, paused.

Ticks after the takeover's fight take nobody's input: the recording's
inputs there would include presses (Start among them) the game would act
on.

## Research options

`rview` also sets research options for the viewer, each off unless noted:
`anykeys 1` lets every keyframe
load, `keysanywhere 0` limits loads to fight keyframes within their round
and round call keyframes, `crossrounds 1` lets those load across round
ends, `callkeys 0` keeps round jumps from loading a round call's keyframe,
`intro 1` plays the intro (`keysanywhere` and `callkeys` are on by
default), and `callpace <k>` with `callafter <n>` limit a
run to k ticks a rendered frame through a round's call and the first n
frames of its fight. The test commands (`hold`, `trap`, `watch`, `keep`,
`mispredict`, `forcerollback`) are in [commands.md](commands.md) and
[desync-replays.md](desync-replays.md).
