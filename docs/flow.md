# The game's flow

The mod follows the game's scenes and battles through hooks on the game's
own functions, and drives them: it jumps from the menus straight into a
battle, preloads characters, runs a battle's ticks itself under rollback and
reads the game's tables of stages, characters and costumes. This page
describes the parts of the game that work relies on. The code is
`src/mod/flow*.cpp`, with the shared state in `flow_internal.h`:

| File | What it covers |
|---|---|
| `flow.cpp` | the hooks: scene changes, flow events, the round director |
| `flow_jump.cpp` | the game-thread queue, jumps to a battle, the way back to the menu |
| `flow_preload.cpp` | the character cache, preloads, character versions, voice language |
| `flow_rounds.cpp` | the phase machine and round director under rollback, keyframe state |
| `flow_tasks.cpp` | the battle's task calls: tasks replayed ticks leave out, task costs |
| `flow_tables.cpp` | stage, character and costume tables |
| `flow_specs.cpp` | data tables (specs, comments), stage, skill and V-Comment names |
| `flow_research.cpp` | research commands (traces, dumps) |

Addresses are offsets in the game image (rva) or in an object (+). TArray,
FString and FText are Unreal Engine 4's array, string and localized text
types.

## Terms

- **Tick:** one step of the battle's simulation, 60 a second. In a session
  the mod runs the ticks itself ([netplay.md](netplay.md)).
- **Snapshot:** the memory regions the mod saves after each tick and loads to
  go back to an earlier tick. Game state outside those regions keeps
  whatever value it last had unless the mod restores it some other way;
  most of this page is about such state.
- **Rollback:** a load of an earlier tick's snapshot, then the ticks since
  run again (replayed, or resimulated) with corrected inputs.
- **Lockstep:** a stretch both games play only on confirmed inputs, so it is
  never rolled back.
- **Keyframe:** a snapshot the Battle Log's replay viewer keeps so that a
  step back or a jump to a round can load it (`OutsideState` holds what it
  needs beyond the snapshot).
- **Game singleton:** the game's top object. The engine's global (rva
  0x3D878C8) holds it at +0x1E0; the battle flow, the stage table and the
  character table hang off it.

## Order of events

At the main menu the mod reads the game's tables and preloads the
characters the player has picked. A session's match starts with a jump: the
mod sets up the battle and switches to the battle scene. In the battle the
flow events mark each stage: the intro, each round's call, "FIGHT!", the
KO, the round's end and the match's end. After the match the results screen
follows, and the mod jumps again for the next match or goes back to the
menu.

## Scenes and the game thread

The scene manager's actor tick (`GameScene::Tick`) runs once per frame in
every scene, loading screens included. It runs the mod's game-thread queue
(`FlowRunOnGameThread`). The game's pad poll runs the queue too. The two
come within a few milliseconds of each other, so the queue runs at most once
per 10 ms.

`SceneManager::ChangeSceneState` switches scene states (boot, title, main
menu, battle). Each state runs one step function at a time (+0x338), each
after a delay in frames (+0x378).

Flow events reach a battle node's event handler with their code at +0xC:
1 starts the battle, 2 and 3 are the intro, 4 and 5 the round call, 6
"FIGHT!", 7 the round's end (KO or time over), 8 and 9 follow it and 12 ends
the match. The handler also sees the round timer's events, whose codes
overlap with these. The round call plays in lockstep, so the mod tracks it
with a plain flag, set at 5 and cleared at 6.

## Jumping to a battle

The battle flow (singleton +0xB8) holds the setup steps each game mode
registers (shared pointers from +0x0), then the battle loader (+0x10).
`EnsureLoader(flow, type)` keeps the loader if it has the given mode type,
else makes one bound to that mode's registered step; Versus is type 0xE. The
Versus loader's per-frame state machine runs phases 1 to 8: 2 loads the
stage (the loader's step 0), 6 sets up the players (step 2), 8 launches the
battle. Phase 0 is idle; character select starts it at 1.

Netplay and replays write the battle's setup (characters, stage, rules)
before they jump. A bare jump (`battlenow`) writes Ryu against Ken in local
Versus, since the setup the game builds by default is not a Versus battle
and its HUD reads an object a jump from the menu does not have.

A jump runs on the game thread, step by step:

1. It waits for a preload to end, since the preload holds the asset queue the
   battle loads with.
2. It waits for a scene change to settle. Switching again right after one
   crashes the game.
3. Outside Versus there is no loader for it. The jump switches to the Versus
   battle setup state, which makes one, and gives it a few frames to load
   character select. A loader that belonged to the previous battle is not
   used.
4. It writes the stage code into the loader's stage request, an FString at
   +0x10 of the object the stage holder's virtual function at slot 0x70
   returns. A new loader's request can have no buffer to write into; the
   jump then points it at a buffer of the mod's own for the request and
   puts the game's pointer back after, so the game only ever frees its own
   memory.
5. It requests the stage as stage select does and switches to the battle once
   the stage is loaded.

Each jump has a generation number. Every step checks it and stops when a
newer jump or `FlowCancelBattleJump` has started a newer generation,
whichever step it waits in. A scene change while the stage loads also stops
the jump: switching to the battle then would start a battle with no setup.

From outside Versus, the scene being left resets the battle flow a few
frames after the switch (`BattleFlow::Reset`), which drops the loader. The
mod hooks that reset and makes the Versus loader once more after it. The
loader starts once the battle state has run a few frames with it: a loader
started in the same frame as the switch never completes its stage request.

## The character cache and preloads

The character cache keeps characters' assets loaded in slots. The slots in
use (a TArray at +0x1D0) are ordered least recently used first; free slots
are at +0x1E0. A slot holds its load queue (+0x0), the character code
(+0x8), the costume (+0x18), the version (+0x1C) and a battle parameter y
(+0x20). `Request(cache, code, costume, color, x, y)` moves a matching slot
to the end, or takes a free one or the oldest, and loads that character's
asset paths there. x and y come from the battle setup (in Versus, the stage
code and -1). Battle creation requests every player's character, and a slot
only matches when all of these match, so a preload passes the same x and y
as the last battle.

The asset manager (global at rva 0x3A91FA8) has its load queue at +0x108:
a set of asset paths and a state (+0x8, 0 when idle). A preload loads one
character through that queue as battle setup step 2 does, then, once the
queue is idle, asks the cache for it. The cache only takes a character whose
assets are already loaded, which is why the load comes first.

Preloads cover the characters picked for the next match. They run at the main menu, once the
picks have not changed for a second, one at a time and never while a jump is
on its way. Each character is tried twice at most. While a session's
results screen is held open between matches, a new pick is not preloaded:
the jump loads it with the battle. The netplay driver asks
`FlowPreloadsDoneFor` while holding its own lock, so the preload reads the
picks without holding the preload's lock.

## Character versions

Each character has a list of balance versions; a battle plays one of them.
The game singleton's +0x60 holds the lists. Per character (rva 0x57F030),
+0x80 lists entries of 0x18 bytes, each starting with a version number. A
battle given version -1 plays the latest.

Sessions play each character's final balance version: the last entry of its
list on an up-to-date install. The host writes these into the match setup,
so both games play the same versions even where an install takes another
version as its latest. The mod reads the local game's lists once on the game
thread, after the game has loaded its character table.

## Voice language

Each character's voice language goes into the battle's setup. The voice
settings are `KWOptionSettingsSave` in `GameSystemSave`:
`VoiceLanguageMode` (+0x40: Japanese, English or Custom) and
`CharacterVoiceLanguage` (+0xC0, one byte per character by unique id), which
character select passes to the battle.

## The round director

The round director is a battle task object whose state (+0xF8) says whether
the battle is stopped: a stop freezes the fighters for a moment, as on a KO
or at time over. Its check (rva 0x485240) runs once per rendered frame
outside the battle tick and only acts in state 1 with no stop recorded
(+0x108 is 0). It advances four timers, looks at the fighters for a KO or
time over, records the stop (+0xFC to +0x130, its kind at +0x108) and moves
to state 2, setting a stop bit in the battle frame object (+0x190, bit 0x20).
The battle tick clears the bit and moves the director to state 1, or to 3
after a stop that ends the round.

The director is outside the snapshot. A load can bring back a set stop bit
while the director has already moved past state 2, and then nothing clears
the bit. In a session the mod therefore runs the check after each tick,
with the tick, and keeps the director's fields with the round's state.

The director also counts three kinds of hits for the profile's fight stats
(+0x11130 to +0x11138). The battle runs the fighters' jobs on worker threads
as well as the game thread, and two hits landing on the same tick could each
update a count at once and lose one. The mod lets one count update run at a
time.

## The phase machine

The battle's phase machine (rva 0x3DD890, a task in the tick) moves the
battle through its stages, from the intro to the match's end; each switch
posts the flow event whose code is the new phase's id. It holds its phases
as shared pointers from +0xF8, the current phase at +0x120 (object) and
+0x128 (reference block), its index at +0x130 and the requested one at
+0x134. Phase objects are 0xC0 bytes for the round end's phase classes and
0xA0 for the rest. Each phase has a timer whose bound argument is a 16-byte
heap object, freed when the phase is left.

## Rolling back across a round's end

The round's end changes two things outside the snapshot:

- The phase machine switches from the fight to the round's end. After every
  tick the mod runs, it keeps a copy of the machine and its phases in a ring
  of 64 ticks, keyed by a tick count that is in the snapshot. A load puts
  back the copy of its tick. Timers are rebuilt with a fresh argument
  through the timer's own clone function, and the current phase is swapped
  with its shared pointer's reference count kept right.
- The round over (director state 2 to 3) adds to the round count and writes
  the round's record into a history in the battle context (+0x1904 the
  round's time, +0x1908 rounds recorded, records of 0x10FC8 bytes from
  +0x1910, the results at +0x78788). Before each round over the mod saves
  those ranges, and a load to a tick before it writes them back.

With these restores turned off, netplay delays the round's end until the
KO is older than any rollback can reach.

When a copy is put back, each 8-byte word in it is restored only if it is
data, or a pointer equal to the live one. A word counts as a pointer when it
points into the game image or at an object whose first word is one of the
game's vtables. Timer rates and small packed fields are data, and so is a
value with an empty upper half (a float, for instance), even when something
is mapped at that address.

The state at tick 0 (the freeze, before a session's first tick) is kept too:
the ring only has copies from the first tick on, and a load to tick 0 must
put the machine back in its first phase so the first tick switches phases
and posts its flow events once more.

## The round end wait

The round's end phase waits until an object's mode (+0x104) is 6 or 7, then
for its one-tick latch (+0x100). The object (vtable rva 0x2C602F8, 0x140
bytes) is outside the snapshot. The KO's flow event arms it, a stage
transition KO reads it as it plays, its own code sets the mode, and the
round's end events reset it. Its last 0x48 bytes, from +0xF8, go into the
per-tick copies and come back with a load.

Those bytes hold a shared pointer (+0x110 the object, +0x118 its reference
block). Each per-tick copy holds a reference of its own through the game's
assign function (rva 0x2F02D0), and a load puts the pointer back through it,
so the object's reference count matches the pointers to it. The mod lets go
of a copy's reference on the game thread, since letting go of the last one
destroys the object.

## The input recorder

The game's battle input recorder (global at rva 0x39DA0D0, 0x79580 bytes,
outside the snapshot) drives the round markers above the health bars. It
holds its last message (+0x8), per-round records (+0x18) and 4 slots of 0xD0
bytes from +0xE0, each with a state (1 or 2 while recording, 0 after the
round's end) and a round number. Its first 0x420 bytes go into the per-tick
copies, restored by the same data-or-pointer rule as the phases.

## Keyframes

A keyframe keeps the round director's fields, the round history and the
latest round's record (the one at +0x787AC of the battle context). The fight
phase's tick reads that record's +0x787B8 to decide when the round's end
starts, so a keyframe taken before a KO and loaded after it must bring it
back. The history is kept in 4 KB chunks, each shared with the previous
keyframe's while unchanged. A keyframe holds no references, unlike the
per-tick copies: a load from one leaves the round end wait's shared pointer
as it is.

## Sounds under rollback

The battle's sound manager has rollback handling of its own, built for the
game's own netcode. Its post-restore callback (rva 0x4B6230) moves the
sounds played after the restored tick into a list of candidates and sets a
replay mode. In replay mode a sound request is queued with its tick instead
of played. A task the scheduler runs in its phase 4 (rva 0x4AFEF0) then ends
replay mode and settles the queue against the candidates: a sound played
before is not played again, a new one plays and a candidate nobody asked
for again is stopped.

The game's netcode ran all replayed ticks and then that task once. The mod
runs the scheduler's phase 4 on every tick, so replayed ticks leave that
task out and the newest tick settles them all. The research command
`soundrollback 0|1` turns this off and on.

## Tasks left out of replayed ticks

The battle calls every task through one thunk (rva 0x4C1D10), with a closure
of the task's function, a this-adjust (+0x8) and its object (+0x10). To make
rollbacks cheaper, replayed ticks leave out some tasks of the scheduler's
phase 4 whose work only the newest tick needs ("lean" replayed ticks). A task
is named by its function's rva or, for the generic task that calls its
object's virtual function +0x38, by the object's class vtable. Each one left
out gives the same result as a straight run in full-match stress replays.
These stay in: the fighters' box ring task (rva 0x37D170), the tasks at rva
0x37D1C0 (its state differs in replayed frames) and 0x37CF60, and the phase
machine's task (rva 0x3DD1B0).

The phases and the director reach the battle context, the battle's main
object, through their task's +0xE8 (rva 0x307F90 locks that weak pointer).

## The game's tables

Each list below is a TArray at +0x28 of its data asset.

- **Stages:** `KWStageEntryDataAsset` (singleton +0x68), entries of 0x50
  bytes: five FStrings, the stage code, then level and asset paths. The
  third is the stage's `KWStagePersonalDataAsset`, whose name is an FText at
  +0x28. A stage is selectable when it has personal data of its own and a
  name. Story variants of a stage use their base stage's personal data;
  RND is random select. Stages whose personal data the game has not loaded
  (Ring of Arcade) have their names loaded by the mod.
- **Stage settings:** `DA_StageArrange`, entries of 0x18 bytes: the base
  stage id and a list of 0x30-byte items whose first int is the stage that
  setting plays (The Grid Alternative for The Grid, and so on).
- **Characters:** `KWCharaEntryDataAsset`, a field of the singleton found by
  its class. Entries of 0xE8 bytes: the code, the prog id (+0x10), the unique
  id (+0x14), personal data (+0x18, its English name an FText at +0x50) and
  the costumes (+0x50).
- **Costume colours:** a costume names its colour preview in the entry's
  list at +0x70 ("RYU01" for `DA_RYU_CostumeColorPreview_01`). The preview
  lists colours at +0x28 and EX colours at +0x38 as pairs of FColor (BGRA).
  Costumes 30 to 37 name a preview the game lacks; their colour clusters hold
  2 colours.
- **Costume variants:** a costume's variants (a second look the menus give
  for buttons held at load) come in two kinds. Most are a dress state asset
  next to the costume's battle data, asked of the paks with
  `FPackageName::DoesPackageExist` (rva 0x11EE840) without loading it; most
  are named after the costume's asset, a few after the character's folder.
  The others are a material preset's variant entry, which only shows once
  the costume is loaded, so the mod lists them in `variant_table.h`. Urien's
  and Akira's variants are a third kind, unique battle parameters
  (`battle_setup.cpp`).
- **CharaSpec:** the costume names and colours the menus offer. A row per
  character (prog id), costume and colour id (0 and up for the costume's
  colours, 100 and up for its EX colours); the costume's first row carries
  its name.
- **StageSpec:** a row per stage the game sells and offers, by index.
- **Battle Lounge comments:** `FixedPhraseCommentSpec`, the fixed phrases
  the game's lounges use, in the game's language. The mod uses them for room
  titles and reads them once the table is loaded at the main menu.
- **V-Skill and V-Trigger names:** `DA_VSkillNameAsset` and
  `DA_VTriggerNameAsset`, entries of 0x20 bytes: the character's code and a
  list of FTexts (0x28 bytes each).
- **V-Comments:** `DA_<code>_Subtitle`, a list of FTexts at +0x48. The game only loads it
  with the character's battle data, so the mod loads it when the menus need
  it first.

The specs and comments are UDataTables. A UDataTable has its row struct at
+0x28 and its rows' elements (0x18 bytes: name, row, hash links) at +0x40.
A struct's properties start at +0x38, each with the next at +0x28 and its
offset in the row at +0x4C; the mod finds columns by property name.

Looking up an object by name walks every object (about 130,000). Lookups
that recur go through a cache (`CachedObject`). `KnownObject` answers from
the cache alone, from any thread. Lookups made while drawing (names,
counts) and from the network thread only use what is cached, and start the
walk or the load on another thread.

## Research tools

The research commands that read the game's objects (`phases`, `roundinfo`,
`ufunc`, `functrace`, `costumes`, `datatable`, `presets`, `phasecost` and
`tasklist`) are listed in [commands.md](commands.md).
