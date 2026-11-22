# The snapshot

The snapshot is the game state the mod saves after each tick and loads to
go back to an earlier tick ([flow.md](flow.md) defines ticks and rollback).
It starts from the game's own rollback state: the battle registers its
state regions with a rollback manager, and Capcom's netcode saves them.
The mod adds regions the game leaves out, gives the regions an order and
names that are the same on every machine, and masks the words that differ
between machines without meaning anything. The code is
`src/mod/sfv_game*.cpp`:

| File | What it covers |
|---|---|
| `sfv_game.cpp` | a driven tick, the inputs, warm-up, loading a snapshot |
| `sfv_game_regions.cpp` | the region list, names, masks, the state map, layout logs |
| `sfv_game_camera.cpp` | the battle camera's start state and the start probe (research) |

Addresses are offsets in the game image (rva) or in an object (+).

## Region hashes

Region hashes are Hash64 with every byte of a masked word read as zero,
which gives the same value as hashing a copy with those words cleared,
without the copy, which doubled the cost over SFV's 436 KB of mostly masked
regions. The mask is read 4 words at a time (half a mask byte), since most
stripes have no bit set. The regions lie all over the heap, mostly out of
cache, so the start of the next ones is fetched while one hashes. The fold
into the frame's checksum is order-independent, since peers may list the
same regions in a different order.

## Region order and names

The game registers its regions in an order that differs between battles,
and so between the two games of a session. The mod sorts them by size, then
by contents with pointer-like words left out, and names them by size and
place: `s372.0`, `s372.1`. Pooled effects (below) come first of their size
whatever they hold, so the regions after them get the same names on both
machines. The list is rebuilt only when the game's table changes, at a new
battle.

A word looks like an address when it is 4 GB or more, below the user-space
limit, and in memory in use. The mod remembers each reservation it finds in
use, since heaps commit more of their reservations as they grow. It
remembers free 64 KB granules only for 250 ms: memory allocated since must
not stay "not an address" on one machine only.

## Regions the mod adds

- **The input unit's counters** and the held-input capture (Urien's thong
  reads it), so a rollback runs the capture again on the corrected inputs.
- **The battle frame's state** (its update counters) and the game
  singleton's tick counters.
- **The pawns' and view target's animation clocks** (presentation).
- **The round's flow phase and a copy of the round director**
  ([flow.md](flow.md)). The director itself is only known once its task
  runs, and a region appearing mid-session would change the state's size
  under the session.
- **Rose's soul entity's tail.** The game's region is the entity's
  +0xF0..+0x463 (884 bytes; vtable rva 0x2C5E2A8). The countdown that
  applies a new soul level (+0x61C; rva 0x3C14F9 and 0x3C1990), the level
  table and its counters lie after it, at +0x5C8..+0x66F. A rollback left
  them running on, and the level came a frame early in the resimulation:
  every Rose match desynced on both PCs.
- **The camera shake's tail.** The game snapshots 528 bytes of the battle
  camera from +0x100 (its sub-object with vtable rva 0x2C4F4A0 sits at
  +0xE8). The shake's flip period, counter and stop flag (+0x310, +0x314,
  +0x318; rva 0x339514, 0x33E0A3) lie after it, so after a rollback the
  shake flipped direction on another frame. The mod adds +0x310..+0x31F.
- **A fighter's event flag.** The game snapshots a fighter's 372-byte
  vitality block from +0xC0 (the block's owner, vtable rva 0x2C51718, at
  +0xB0). +0x338 is a per-tick event flag (cleared with +0x334 at rva
  0x362A64, set at 0x328E13) that no region covers. Rose's soul entity reads
  it (rva 0x3C10D0, 0x372940) before the fighter's tick clears it, so it
  carries from one tick to the next. After a rollback it held the newest
  tick's value, and Rose's entity set the fighter's +0x110 bit differently.
- **A fighter's four effect switches,** records of 0x48 bytes from +0x340.
  Each tick rva 0x362520 picks the entry type a switch shows, by its kind at
  +0x28 or its override at +0x40 when that is not -1. It turns off the type
  shown before (+0x3C) when the pick changed, and turns the pick on in the
  fighter's effect list (s1624 +0x0, its count at +0x2C0; rva 0x364800).
  Move scripts set the override (rva 0x35EB40) and the level (+0x38, rva
  0x35EB80), and can install a record (rva 0x35EBF0). The game's regions
  cover only +0x2C..+0x33 and +0x44 of each record. After a rollback the
  override and the type shown kept the newest tick's values; the
  resimulated tick took its pick as already shown and added it without
  turning the old one off (an effect count of 2 against 1). A warm-up that
  set them left both games apart from frame 0. The mod adds +0x28..+0x43 of
  each record.
- **The effect slots' owner value.** The game snapshots a fighter's effect
  slots (392 bytes) from the owner's +0x6D8 (the owner's vtable rva
  0x2C53598 at +0xB0). The owner's +0x108 is set each tick from the fighter
  (rva 0x3A00C5; kept while rva 0x377710 holds), and no region covers it.
  Poison's unique part (Z33, `CharaUniquePart_035`) draws slot 0 or 1 from
  it (rva 0x3BDA38), so after a rollback the slot states came out
  differently.

The fighters' added regions are named after the region they sit next to
(`fighter.event.<n>`, `fighter.switch<k>.<n>`, `effects.owner.<n>`), since
the game registers the fighters in a different order on each machine.

## Words out of the checksum

Some words are restored with the rest on a rollback but left out of the
desync checksum, because they differ between machines with no effect on
the fight:

- **The input ring's reset tag** (input unit +0x217C), which the logic only
  compares with itself. The other half of its word (+0x2178) is forced at
  each driven tick.
- **The animation clocks** of the camera and the pawns. They follow how
  ticks fall into rendered frames, which differs on each machine, more so
  with time sync stretching frames. In intros, round calls and win poses
  they parted by a few microseconds while the rest of the state agreed:
  400 false desyncs a match.
- **The screen shake** (camera +0x2E0..+0x31F: amplitude, offset, frame
  counter and length, flip period and count; rva 0x33A7C0 starts a shake,
  0x33E090 runs and ends it). Stages start shakes too: an animation notify on
  an SX1 prop runs its Blueprint's `ShakeCamera` (rva 0x531870) on the
  prop's world-time animation, so on another frame on each PC whenever a
  frame hitches. Only the drawn view reads the shake.
- **Rose's soul entity's +0x620..+0x62F,** left from the last match (floats,
  at times an address), constant through the next and different on each
  machine. None of the soul code writes them.
- **Pooled effects** (below), whole.
- **Noise words,** listed by region size and offset: words that differ when
  a frame is resimulated, from engine-side transforms, and pointer fields.
  The list is built into the mod (from `data/sfv_noise.txt`), so every copy
  of a build masks the same words. `[mod] noise_file` swaps in a file, for
  learning new words; peers then need the same file.

## Pooled effects

Visual effects the game keeps in pools: motion trails, effect objects, an
effect pool and the scripts' effect records. Which pool object an effect
gets is decided outside the snapshot (the trail manager's pending requests,
rva 0x484C70). After a rollback across a request, the same effect can land
in another object, and the objects it did not take keep a finished
effect's leftovers, so the two games hold the same effects in different
objects. A reused effect record goes on counting from an old effect, and a
record can stay live in one game and be reused in the other. Nothing in a
fight reads them back, and a divergence in the fight still shows in the
fighters' regions.

Vega's knocked-off claw and mask are drawn the same way. They fly from his
hand or head, which his mesh's pose gives (rva 0x3A4B74, outside the
snapshot and posed once per drawn frame, so a resimulated knock-off launches
from a later frame's pose), land and fade. In tester matches the two games'
items parted for thousands of frames and the fighters never did.

Each such region is known by its size and where its owner's vtable sits
before it:

| Size | Owner's vtable | Before the region | What it is |
|---|---|---|---|
| 44 | 0x2C6FC18 | 0x10 | trail object (vtable 0x2C6FB88) +0xC0: timer, state |
| 136 | 0x2C6FC18 | 0x40 | trail object +0xF0: transforms |
| 40 | 0x2C6EAD0 | 0x10 | effect object: flags (a slot's live flag) |
| 68 | 0x2C6EAD0 | 0x90 | effect object |
| 208 | 0x2C6EAD0 | 0xE8 | effect object: transforms |
| 164 | 0x2C6EAD0 | 0x1C8 | effect object: handles |
| 15808 | 0x2C6E6D8 | 0x18 | effect pool |
| 224 | 0x3231618 | 0x30 | script effect record |
| 208 | 0x2C54490 | 0x40 | Vega's `CharaUniquePart_021` +0xF0: his claw and mask |

## A driven tick

Start reaches the game only when both players pressed it outside the fight
([flow.md](flow.md)), so one player cannot skip the intro or pause the
fight. The input unit's round parameter is set at round start from a
machine-specific global (rva 0x438EBD), which differs between peers; the
mod sets it to 0 before every tick. Between ticks the presentation (intro
and round call skipping, once per rendered frame) reads the current input
values. Rendered frames and ticks line up differently on each machine, so
the values are cleared after each driven tick: a skip pressed during an
intro would otherwise start the fight on a different tick on each machine.

The next tick reads the pads' values for its frame from a latch the game
fills from the input ring once per rendered frame (the intro's Start
check). The mod latches after every driven tick and after a load, so a
replayed tick sees what it saw the first time.

`BattleFrame::Update` runs 1 + [+0x1A8] ticks; the mod sets that to 0 for
one tick. A replayed tick runs the update's parts without scheduler phase
8, and phase 4 without the tasks replayed ticks leave out
([flow.md](flow.md)). The newest tick of the frame is a whole update and
runs them. What is left out is presentation the newest tick makes up for,
and stress replays showed every frame's state equal to a straight run's.
Phase 1 (the round state's clocks, rva 0x4BB540), phase 2 (the battle) and
the fighters' phase-4 tasks (their box ring, rva 0x386950, and rva 0x37D1C0,
which costs more once after many ticks than once per tick) run every tick.
The update's own counters move as a whole update moves them.

The battle's round state is the one 228-byte region: +0xB4 the round, +0xC4
and +0xC8 the matches each side won since character select, one more as the
match is decided.

## Warm-up

A battle's state can still be registering as its first regions show up. A
host whose objects came after 3 ticks had 369 regions and its joiner 549:
90 objects of a pool of 128 (vtable rva 0x2C4E598, one 4-byte and one 8-byte
region each) were not there yet, and nine matches between them parted from
frame 0. So the freeze before frame 0 waits until the region count has held
for 12 steps, without ticking, at most 240 steps. More ticks would move the
battle: they move the fighters, the round's flow and the camera (40 regions
differ after 2), and a session started that way desyncs from frame 0.

## Loading a snapshot

After the regions are copied back, the mod runs the game's post-restore
callbacks as the game's own restore does (the loop at rva 0x48A45C). Each
callback gets the id of the restored frame: the game's own, read from the
input unit's counters (restored with the snapshot), as its restore passes
it. Two callbacks compare it with frames the game stamped from the input
unit's +0x2180: the sound manager's (rva 0x4B6230; sounds played after that
frame are replay candidates) and rva 0x4D5820 (it flags the items of its
objects that started after it).

About 90 % of the callbacks' time after a load (0.9 to 1.2 ms a load on the
test PC) went to three: the fighters' animation re-sync (rva 0x3587D0) and
two at rva 0x448B30 and 0x448B40. The ticks resimulated after the load set
the same state, and two-PC sessions and stress replays with them skipped
had no desync, so loads skip them. The camera views' reset is replaced too
([rollback-presentation.md](rollback-presentation.md)). Every 200 loads the
log lists the callbacks by cost.

## The state map

A state map (`sfv_statemap.bin` beside the game, made by the learning mode)
marks, per region, the words that held a pointer when it was learned, so
that hashes can leave pointers out. Without one, hashes include pointers.
A map that does not fit the battle's regions is not used. Whether a value is
an address in use is a property of one process, so the checksums never test
it live: a value equal on both machines could be masked on one side only.
