# The battle driver

The battle driver takes over the battle's frame loop. The game's
`BattleFrame::Update` runs once per rendered frame on the game thread and runs
that frame's ticks. With a frame driver set, the hook calls the driver
instead. The driver runs ticks itself through `SfvGame::Advance`, one tick
with given inputs each. It loads and saves state between them, or runs none
to stall. [netplay.md](netplay.md) and [replays.md](replays.md) describe the
drivers; [flow.md](flow.md) defines ticks, snapshots and rollback.

The code is `src/mod/battle_*.cpp`, with the shared state in
`battle_internal.h`:

| File | What it covers |
|---|---|
| `battle_driver.cpp` | the frame hook, the input record hook, setting and detaching the driver |
| `battle_random.cpp` | random state in driven ticks |
| `battle_phases.cpp` | the battle update's phases and jobs: the job order, phase costs |
| `battle_sound.cpp` | capturing and stopping the battle's sounds |
| `battle_trace.cpp` | research logs: the damage gauge, bones, actors, cinematics, cameras |
| `battle_regions.cpp` | research commands over the snapshot regions |

Addresses are offsets in the game image (rva) or in an object (+). Driven
ticks are the ticks a frame driver runs.

## The frame and the inputs

A driver's own ticks come back through the frame hook (through
`SfvGame::Advance`), and the hook runs them as they are. Their time counts as
the driver's in the frame-drop log. After the driver's frame, the camera
requests that the frame's resimulations dropped are undone
([rollback-presentation.md](rollback-presentation.md)). Then the hitbox
display and the frame meter read the frame about to be drawn.

`InputUnit::RecordFrame` records a tick's inputs. With a driver in charge
only its ticks record. A call from anywhere else would record a local input
that the other machine never sees, and advance the input unit's frame
counter on one machine only. Such calls are skipped.

`BattleDetachFrameDriver` clears the driver, then waits up to 5 s for a frame that
read it before to finish. On the game thread itself there is nothing to wait
for.

## Random state

The game draws random numbers from several places outside the snapshot.
Left alone, two games of one match draw different numbers. A rolled-back
game would also draw again while it resimulates, and two replays of one
recording would differ. In driven ticks the mod seeds each draw from the
match setup's seed (`BattleSetMatchDrawSeed`), the tick, and something about the
draw that both games share. Those keys are the same on both machines and in
any resimulation.

**Fighters' streams.** Every fighter draws one number per tick from its own
random stream (rva 0x37D642 stores it at the fighter's +0xC50). Moves with a
random outcome use it, Dan's among them. The draw (rva 0x32B2B0) steps the
state at the stream's +0x8: `state * 0x19660D + 0x3C6EF35F`. The streams
start per process. Each draw's state is set from the drawing object's slot
(+0xF8 of the owner, 0 to 3: P1, P2, then the helpers) and the draw's count
on that stream in the tick. The stream sits at its owner's +0xC40; owners
are the fighters and helpers, whose vtable is at rva 0x2C521F8. Two other
keys did not work:

- The draw's place in the tick: the objects that draw (four with R. Mika vs
  Akira) update in an order that changed between a tick and its
  resimulation, and they swapped numbers.
- The stream's last number: it is noise-masked, so the copy of the start
  state leaves it per process.

Another object's stream (the battle camera's shake, a Rose mirror match) is
keyed by the draw's call site and the count of draws there in the tick. The
order of all such draws did not work: effects a rollback loses moved the
camera's numbers.

**Rose's soul level.** Rose's soul level (her entity's +0x444, 2 to 5, which
she changes during the intro and later) is picked by rva 0x3AD7E0 from such
a stream. While that picker runs, each draw is seeded from the battle's tick
count (the battle frame's +0x1B8) and her current level.

**The global word.** A second random state is one global word (rva
0x3966540). Eight functions draw it with inlined copies of the same step:
the range pick at rva 0x391C30 and seven weighted picks next to it. They are
reached through a table of selector classes. Battle scripts pick move
variants with them (R. Mika vs Akira: 0x368, 0x36A, 0x36C, 0x36E at the
fighter's +0x3E30). Nothing seeds the word, so it holds whatever the process
drew before. Seeding it once per tick did not work: other draws earlier in a
resimulated tick (effects, which rollbacks lose) moved it. In driven ticks
every pick sets the word first from the call site and its count there in
the tick.

**The C runtime's rand().** The engine's `rand()` (MSVCR120.dll, state per
thread) is called in battle ticks (rva 0xEB2D64, 0xEE7F6E, 0x1E288CA,
0x1E48A27). Each driven tick seeds the game thread's state from the match
seed and the tick (`BattleSeedTickRandom`).

The counts per stream and call site start again with each driven tick. The
counters are keyed by address, which only tells them apart and never reaches
a seed.

## Phases and jobs

`BattleFrame::Update` has parts: the update's start (rva 0x4DC2D0), the
scheduler's phases 1, 2, 4 and 8 (rva 0x4CD890) and the tick proper (rva
0x4D2850). Phases 4 and 8 are presentation: animation, effects, the camera.
When the game's own update runs several ticks in a frame, it runs them only
for the last one. `resimskip 1` does the same for replayed ticks. `phasecost
1` logs each part's time per driven tick every 600 ticks, normal and
replayed apart.

A scheduler phase queues each of its tasks as a job on one global queue (rva
0x3A750E0). It runs them with rva 0x4CD420(queue, 1). With the queue's +0x74
at 0 the runner calls the jobs one after another on its own thread, in the
order they were queued. That order is the scheduler's map order: priority,
then registration (the key at the task node's +0x20, the counter at the
scheduler's +0x1C; rva 0x4CC560). Otherwise (+0x74 is 2) the game thread and
the battle's worker threads take them as they come.

Fighters' jobs write into each other's state. A hit's stun goes into the
defender's, events go into the defender's buff conditions, and the round
director tallies hits ([flow.md](flow.md)). So which job ran first could
differ between PCs. A defender's 10 % stun buff (kind 13 at its buff table's
+0x120) counted on one side and not on the other. Its conditions are
cleared by its own job (rva 0x33AAD0) and set by events (rva 0x32964F). The
desyncs showed up as stun +10 against +9, then a stun gauge one point apart
for thousands of frames. Driven ticks run their jobs in order on the game
thread: one order on every PC. The cost is the same: 434 to 483 us per tick
serial against 459 to 537 us on the threads. `serialjobs 0` gives the jobs
back to the threads.

## Sounds

The CriWare plugin's play request (rva 0xEE7F10) takes a player, the cue
sheet, the cue id and one more int, and returns a handle (-1: nothing
started). The game thread queues it (a type-0 record pushed by rva
0xEE8BE0), and the plugin's sound thread plays it later (rva 0xEE8830, then
0xEE8F40). This is where a resimulated tick asks for a sound again. The stop
request (rva 0xEE8710) takes a player, the handle and 1 to stop without
release time. It queues a type-2 command whose handler (rva 0xEE9080) finds
the handle's playback in the player's table and stops just that one. A
handle whose sound has ended finds nothing there.

**The capture.** Between `BattleBeginSoundCapture` and `BattleStopCapturedSounds` the
handles of the sounds asked for are kept, and the latter stops them. The
mod's muted runs use it. Cue sheet 58, the stage's ambience, is never
captured. It is a loop started at the battle's start and again at each
round's set-up, and it is meant to go on. Stopped, the paused bed went from
rms 0.0085 to 0.0018. Threads marked exempt (the mod's own menu sounds) are
not captured.

**A viewed replay's battle.** While `BattleRecordSounds` is on, every sound
the battle asks for is kept, the newest 16,384. `BattleStopSounds` stops
them before the battle is started again, since its loops would play on. It
also stops the capture's sounds while their players still exist, and ends
the capture. A stop on a player freed with the old battle would crash. The
restarted view begins its own capture.

## The damage gauge

A fighter's damage gauge grows by an amount per hit taken. The gauge object
holds the value at +0x170, its maximum at +0x174 and its level at +0x178, up
to 5, all 16.16. The add (rva 0x346A20) is called at the hit (rva 0x36D478)
with about 1000 / max vitality (+0xC4) * damage (the caller's r14) / the
battle's round count. The round count is the battle data's
`battledata::kSpecRounds` (r13 + 0xE8, then getter rva 0x2F85B0, then the
word at +8), outside the snapshot. Two testers' games kept the count from
their own settings, 20 on one machine and 59 on the other, so the gauges
differed. Match setups write the count now.
Every add is logged. A stub saves the caller's r13 and r14, still the
caller's on entry, before the detour runs.

## Research hooks

- **Bones** (`bonelog`). A fighter's bone world position (rva 0x3919D0),
  from its skeletal mesh (the fighter's +0xA90) through rva 0xE58F10. Battle
  code asks for it: Vega's knocked-off claw and mask launch from his hand or
  head (rva 0x3A4B74). The mesh is posed once per rendered frame, outside
  the snapshot, so a resimulated tick reads the newest frame's pose.
- **Actors** (`actorlog`, `cinelog`, `ftrace`). They read the actors as
  `battle_actors.h` describes them. Script names come from the move file's
  tables: `#BAC`, the table count (u16) at +12, the tables' offsets from the
  u32 at +16. Each table has an i16, a u16 count, then the offsets of its
  scripts and names (u32 each, from the table). A loaded move file object
  (KWBAC, KWBCH, KWBCM) holds its bytes at +0x28 and their count at +0x30.
- **The camera drawn.** The PlayerCameraManager (+0x390) of the controller
  with a local player (+0x360). Its view (position, rotation, field of view)
  is at +0x370.
- **Region owners.** A region's owner is the nearest word at or before it
  that points at a vtable in the game's .rdata, one whose first entry points
  into .text.

The commands are in [commands.md](commands.md).
