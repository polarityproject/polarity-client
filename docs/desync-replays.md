# Desync reports and deterministic replays

When a player reports a desync, we need to answer three questions without
having been in the match:

1. **Where** did the two machines first disagree: which frame, which part of
   the state, which bytes?
2. **Why**: a match setup that did not sync, a netcode bug (the peers
   simulated different inputs), or a nondeterministic simulation (same inputs,
   different result)?
3. **Who**: if the simulation was nondeterministic, which machine was wrong,
   and can we reproduce it locally?

Every netplay match produces a recording on each peer that answers these. The
same recordings double as replays and as regression tests for the rollback
integration.

## What each peer records (`.sfvr`)

| Chunk | Contents | Why |
| --- | --- | --- |
| META | mod version, game build id (exe timestamp + adapter version + state map fingerprint), random match id, local player, input delay, prediction window, last confirmed frame | Pairs the two halves of a report and refuses to compare different builds |
| SETP | `MatchSetup`: characters, costumes, colours, V-Triggers, stage, rounds, timer, seed | Everything needed to start the same match again |
| RGNS | state region table: name, size, pointer mask | Hashes are only comparable over the same layout and mask |
| INIT | one hash per region of the state before frame 0 | Separates "setup did not sync" from "simulation diverged" |
| INPT | both players' confirmed inputs for every frame (2 × u32) | Exact playback with no network |
| SUMS | the state's 32-bit checksum after every frame | Finds the first frame that differs, and lets `verify` check every frame |
| HWIN | one hash per region, taken right after the frame was simulated, for windows of frames; within a window delta-coded (only changed regions are stored) | Finds which regions differ |
| EVNT | rollbacks (frame, depth), desyncs (frame, both checksums), disconnects | Context: was the divergence right after a long rollback? |
| DUMP | full raw state: before frame 0, every 600 confirmed frames (up to 24), and the earliest desynced frame | Byte-level diff between the peers |
| PACK | any of the above, compressed (Windows' XPRESS Huffman codec, `core/compress.h`): the inner tag, its size, the bytes | Dumps shrink about 30 times |
| END | CRC-32 of the file | Rejects files damaged in transit |

This is format version 3. Version 2 files (a HASH chunk with every frame's
region hashes and no SUMS, nothing compressed) still read.

### Compact and full recordings

The mod saves **compact** recordings by default: region hashes only from
120 frames before each desync event to 30 after it (`kDesyncHashesBefore`,
`kDesyncHashesAfter` in `core/replay_file.h`), the checksum for every frame.
GekkoNet health-checks every confirmed frame, so a desync event marks the
first frame that differs, and the window around it holds what `diff` needs
to name the regions. A 2-minute match is about 270 KB instead of 7.8 MB; a
desync report about 250 KB. On the same desynced pair, compact and full
files give the same answer (frame, region and bytes).

**Full** recordings keep every frame's region hashes (several MB a match),
for desync hunting where GekkoNet saw nothing: `[netplay] full_recordings=1`,
or the `recordings full|compact` command for the next sessions. `polarity compact
<in> <out>` turns any recording, of either version, into a compact one.

On frames without region hashes the tools fall back to the checksum: `verify`
and the in-game replay still check every frame, but a divergence there names
no region; `diff` says when the regions are unknown; `changes`, `peers` and
`--first-change` look only at frames that have them.

The recorder overwrites a frame's inputs and hashes each time that frame is
resimulated, so the final contents are the confirmed timeline. Frames newer
than `prediction_window + 1` before the end may still hold predicted inputs;
the file marks that boundary (`finalThrough`) and tools only trust frames up
to it.

### The file's layout

A file is `SFVR` and a u32 version, then chunks of a u32 tag, a u32 size and
the payload, closed by an `END ` chunk holding the CRC-32 of every byte
before it. Readers skip chunks they don't know, so later versions can add
data without breaking older tools. META, SETP and INFO come first and are
never compressed, since lists read them from the file's start; the region
table after them ends the header. INFO has its own version (2: each round's
first action frame, at the end). Fields added to SETP go after its fixed
part, so older files without them still read. A file is written to a
temporary name and renamed, so a crash never leaves half a report.

Region hashes are delta coded. Most regions don't change every frame, so
each frame stores a bitmap of the regions whose hash differs from the frame
before, and only those hashes. In version 3, kept frames come in runs, each
starting from a zero row, so a run's first frame stores every region.

A chunk goes inside a PACK chunk (the tag, the size before compression, the
compressed bytes) when that saves a tenth; chunks under 512 bytes stay as
they are. Since every count comes from the file, readers set limits: at most
64 million region hashes (frames times regions, 512 MB), where recordings
hold 8.4 million at most, and 256 MB unpacked in all, where recordings
unpack to 25 MB at most.

A round's fight runs from the frame after the one whose tick turned it on to
the one whose tick turned it off (the KO).

### Compression

PACK chunks use Windows' XPRESS Huffman codec from `cabinet.dll`
(`core/compress.cpp`). Its functions are looked up at run time rather than
imported: Wine's `cabinet.dll` (Proton, the Steam Deck) had none of them
until 2026, and an import Wine lacks becomes a stub that ends the process
when called. Steam Deck players' games closed as their first match's
recording was saved, after its results screen (0.2.6). Without the codec,
chunks stay uncompressed.

Reading uses Windows' codec where there is one, and otherwise a decoder of
the mod's own, so recordings made on Windows (and the benchmark's round)
read on a Steam Deck too. The format ([MS-XCA] 2.2.4) is blocks of 65536
output bytes, each after a table of 512 4-bit code lengths (256 bytes, the
low nibble for the even symbol), then a bit stream read 16 bits (little
endian) at a time, most significant bit first, with the bytes of long match
lengths in between. The code is canonical: shorter codes first, then by
symbol. Symbols under 256 are bytes, and the others matches: the length in
the low 4 bits (plus 3; 15 means more in the bytes that follow) and the
offset's bit count in the next 4 (the offset is that many bits plus
1 << count). Data that would not get smaller is stored as it is, the same
size in as out, as Windows' own decoder reads it.

The hashes are per region (XXH64, pointer words masked), and their fold is the
32-bit checksum GekkoNet exchanges for live desync detection. Live detection
and offline analysis therefore always agree.

Files carry no Steam IDs, so players can post them publicly. The match id
pairs the two halves.

## Producing a report

On the first desync event the session waits 30 more frames for late,
reordered health checks, keeps a dump of the earliest desynced frame it learned
of, and raises `onDesync` (implemented and tested in `NetplaySession`). The
mod saves that recording to `Polarity\matches\`, and one at the end of every match
too, so a player can report a desync GekkoNet did not catch (a visual one) by
sending the match's files.

A full report is **both** players' files. One file still allows `verify`.

## The Polarity folder (what players send)

Everything a player sends sits in one folder next to the game,
`StreetFighterV\Binaries\Win64\Polarity\` (`src/mod/files.h`):

| Path | What it is |
| --- | --- |
| `logs\polarity.log` | the event log of this run (`polarity.<n>.log` for test instance n); its first line has the date, and a `---- <date>` line marks midnight |
| `logs\polarity_<start>.log` | earlier runs, the newest 20 |
| `matches\<name>.sfvr` | each match's recording, and a `_desync` one when a desync was detected (the newest 300) |
| `matches\<name>.log` | each match's log: who played what, the server and link, the match's numbers, then every event line from the session's start (or the previous match's end) to the match's end (the newest 200) |
| `Polarity-logs-<date>.zip` | what **Settings, Logs, Pack logs to share** writes: the event logs and the latest 10 matches' logs and recordings (the newest 5 zips) |

Match files are named
`<date>_<time>_<P1 name>-<P1 character>_vs_<P2 name>-<P2 character>_<match id>_<p1|p2|spec>[_desync]`,
for example `2026-10-29_13-28-59_Player1-RYU_vs_Player2-KEN_53acea3d_p1.sfvr`.
Names keep letters, digits and `-` (at most 20 characters); `_` only separates
the parts. The match id is the same on both PCs, so the two halves of a
report pair up by it. **Open folder** next to the pack button opens the
folder in Explorer.

## Analysing a report

```
polarity info   p1.sfvr                 # what was recorded
polarity diff   p1.sfvr p2.sfvr         # where the peers disagree
polarity verify p1.sfvr [--stress 7]    # does the simulation reproduce this file?
polarity inputs p1.sfvr 700 740         # what the players pressed around the frame
```

`diff` classifies the report:

| Result | Meaning | Next step |
| --- | --- | --- |
| match setups differ / state before frame 0 differs | the peers started different matches | fix lobby → `MatchSetup` sync; the listed regions show what differed |
| confirmed inputs differ from frame N | netcode or transport bug | GekkoNet/transport, not the game |
| state diverges at frame N with identical inputs | nondeterminism | the region list and byte ranges say where; then `verify` each file |

`diff` pairs regions by size, since the peers' region orders may differ: a
region mismatches when no region of the same size on the other side has its
hash, and within each size regions pair up greedily by fewest differing
words. A name such as `s128.0~s128.3` means the peers listed that region at
different places. The confirmed inputs are compared whatever the states do,
since they tell the netcode's part apart from the game's. For byte ranges it
prefers dumps of the first diverging frame, or else the earliest later frame
both peers dumped. Offline there is no process to ask whether a word is an
address, so a value above 1 TB, where 64-bit Windows puts heaps and images
(two packed int32 rarely get there), counts as one, and addresses on both
sides compare equal.

`verify` plays one file back deterministically: start the recorded setup, feed
the recorded inputs, compare every frame's region hashes.

| Result | Meaning |
| --- | --- |
| match | this machine's simulation was a pure function of the inputs |
| diverged at frame N | the recording machine simulated something the inputs do not explain: a machine-specific value, a different build, or a rollback that did not restore everything (its live timeline drifted) |
| rollback stress mismatch at frame N | loading frame N−d and resimulating d frames with the same inputs gives a different state than simulating once: state outside the snapshot, or code that behaves differently while rolling back |
| setup mismatch | this machine cannot even reproduce the starting state |

The usual pattern for a one-sided bug: one file verifies, the other diverges at
exactly the frame `diff` reported. For a rollback bug both files diverge
(each machine's own rollbacks corrupted its timeline) and `--stress`
reproduces it on any recording, including one made with no rollbacks at all.

Rollback stress with real recorded inputs is a stronger sync test than random
inputs: it replays the situation the player was in (same characters, same
moves at the same moments) with rollbacks at every frame.

## The polarity tool

`polarity` (`src/tools/polarity.cpp`) reads recordings and reports. Run it
with no arguments for the usage. Its commands:

| Command | What it does |
| --- | --- |
| `info <file> [-v]` | what was recorded: the setup, the regions, events and dumps |
| `inputs <file> [first] [last]` | each frame's inputs, the direction in numpad notation from the P1-left side, then the attacks (LP MP HP LK MK HK); other bits are shown raw in hex |
| `presses <file> <player>` | the player's button presses in order: the button bits (`0x0FF0`, without directions and Start) each time they go from none to some. `scripts\layout-test.ps1` compares them with each PC's layout. |
| `diff <a> <b> --all` | also every frame both files hold region hashes for whose regions differ, for two kept runs of one recording |
| `peers <a> <b>` | the words that differ between the two peers' dumps of the same frame, with identical inputs before it: values that only mean something in one process, such as truncated addresses, uninitialised buffers and engine clocks. `--write` merges them into a noise mask file (the `sfv_noise.txt` format), keeping its comment lines. `--initial` learns only from the state before frame 0, before any input mattered. `--pointers` learns only words that hold an address on both sides, from every dump: fields that are pointers, since a live process may test a dangling one as data; `--max-frame` stops at a frame before any divergence. Where the two masks differ it names the words masked on one side only, then compares with both. It also shows what changed since the first dump (the divergence, not the noise), how many regions of each size have no equal on the other side frame by frame, and with `--first-change` the frames where each region size first changes on each side, to tell which side reached an event first. |
| `changes <file> <region size>` | for each region of that size, on how many frames its hash changed, and the first and last such frame. A region that never changes is not touched by the simulation (an unused slot). |
| `find <file> <frame> <value>` | every int32 equal to the value in that frame's dump, by region and offset, with that word in the other dumps |
| `images <file> <frame or all> <base> <size>` | the words of a dump that point into a module (`[base, base + size)`, such as the game's image). They are equal between two processes on one PC, since a boot loads the module at one address, and differ between PCs. `--write` adds them to a noise mask file. |
| `region <file> <frame> <name>` | one region of a dump, word by word |
| `compact <file> <out>` | the recording in the current format, with region hashes only around its desyncs (what the mod saves for players) |
| `cut <file> <out> <frames>` | the first frames as a compact recording, with only the state before frame 0 (the benchmark's round, `data/benchmark.sfvr`) |
| `thin <file> <out> <every>` | the recording compact, with the state before frame 0 and a dump only every so many frames: the replay regression corpus (`tests/replays`). A replay starts from the first state, and on a build whose region table differs the dumps are what it is checked against. |
| `export <file> <dir>` | `regions.txt` (each region's name, size, offset in the dump, and the words left out of its checksum as a hex bit mask, or `-`) and every dump as `frame_<n>.bin`, for scripts that look for fields |
| `verify <file>` | plays the recording back. Recordings made with the mock fighter verify here; the game's recordings verify inside the game, whose playback runs the same verifier. |
| `demo <dir>` | mock-fighter recordings of each kind of bug (`clean`, `machine-value`, `outside-snapshot`, `effect-rng`) for both players |

## Replaying a recording in the game

Every netplay match leaves a recording on each PC in
`StreetFighterV\Binaries\Win64\Polarity\matches\` (named as above: `_p1` on the
host, `_p2` on the joiner; a desync also saves a `_desync` one). Any of
them can be played back in the game, at the main menu, with no network:

```
scripts\cmd.ps1 -Instance 0 -Commands "replay 2026-10-29_13-28-59_Player1-RYU_vs_Player2-KEN_53acea3d_p1.sfvr"
scripts\cmd.ps1 -Instance 0 -Commands "replay 2026-10-29_13-28-59_Player1-RYU_vs_Player2-KEN_53acea3d_p1.sfvr stress 8"
scripts\cmd.ps1 -Instance 0 -Commands "replay stop"
```

(a file name is looked up in `Polarity\matches\`; a full path works too). The replay
starts the recorded match setup the way a session does (a jump, neutral ticks
until the battle's objects exist, frozen at frame 0) and plays the recorded
inputs from that state. The recording's own bytes are never loaded into the
game: a state before frame 0 that differs from the recording's makes the run
out of sync from the start, and the log names the regions that differ. The
modes:

- **straight**: one frame per update, each frame's state compared with the
  recording's hashes. Answers "does this build reproduce the match?".
- **stress `<distance>`**: a GekkoNet stress session fed with the recorded
  inputs: every frame is also loaded from `<distance>` frames back and
  resimulated, and must equal its first run (and the recording). Answers
  "would a rollback anywhere in this match break it?", at exactly the
  recorded situation (characters, moves, corners, round ends), whether or not
  the recorded session rolled back there. A 17,000-frame match takes about 5
  minutes at distance 8.
- **watch `<from> <to> <region>`** (with either): logs, for those frames, the
  region's first 16 bytes, the round's end state (director, phase
  machine, the battle input recorder's slots, the mod's tick count) and the
  camera as drawn (location, rotation, field of view; the local player
  controller's camera manager), to compare straight and stress runs.
- **keep `<from> <to>`** (straight): saves the run as a full recording,
  `<name>_kept.sfvr` in `Polarity\matches\`, with region hashes for every frame
  and state dumps of frames `<from>` to `<to>`. A stress replay of the kept
  file then names the words that differ at those frames: the way from "a
  compact recording fails under stress at frame N" to the words involved.
- **trap `<frame> <region> <hex offset> <bytes>`** (repeatable, 8 bytes per
  debug register, 4 in all): write watches on those bytes from that frame's
  tick on; when the replay ends, the writers are logged (`watch: write before
  rip rva ...`, with their callers). The offset may run up to 0x1000 past
  the region's end, or be negative (`-5d0`, up to -0x1000), to watch a field
  of its owner that no region covers.
- **cold**: jumps without preloading the characters (they load while the
  battle is created, as when a session starts before a preload is done).

A replay preloads both recorded characters before it jumps, as a session's
lobby does. A compact recording keeps only a checksum per frame, so a
mismatch names no region; at its state dumps (every 600 frames) the replay
compares word by word and logs the regions and words that differ.

Skips (both players pressed Start in an intro, round call or win pose):
the battle reads the pads through a latch (InputUnit +0x128, rva 0x4440D0)
the game fills once per rendered frame, outside the battle tick. Ticks
replayed within one frame read the newest frame's latch, so a stress replay
skipped up to its distance early and left the recording from the intro on.
Since adapter-9 the mod latches after every driven tick and every load, so
stress replays of recordings from adapter-9 on follow them through skips. A
straight replay of an older recording keeps the game's own latch (its skips
land a tick later), and its stress replays, which need the mod's latch, stay
self-consistent ("resimulated differently") but leave the recording at the
first skip.

A battle whose game was busy loading while it was created can have its
objects after 3 neutral ticks instead of 6 to 19, and frozen there it goes on
differently from frame 0 (its camera did not follow the fight). It has happened
to replays sent right after a game start, and to a spectator of a session
started right after boot. The cause is open. Running more
ticks once the objects exist does not help: they start the round's flow
(director and phase machine, outside the snapshot), and loading the frozen
state back afterwards leaves that flow started (both tried).

The result is one log line, for example:

```
replay: ...eeafb041-p1-match.sfvr PASS, 17465 of 17465 frames checked, stress distance 8,
        0 frames differ from the recording (first -1), 283 presentation clock differences,
        0 frames resimulated differently
replay:   regions (frames): ...
replay:   resimulated differently (frames): ...
```

Regions are compared by content, not by name (the canonical order numbers
same-size regions by content, and a battle can bring a region more or less,
which shifts every name after it). The camera's and pawns' clocks
(`viewtarget.clock`, `pawn.clock`) follow how ticks fall into rendered
frames and are counted apart. Reading the tallies: one or two small regions
on many frames point at a presentation or loading-timing word; many regions,
or fighters (372 / 652 / 1692 bytes), at a real divergence. A known case: a
44-byte and a 136-byte effect pool can differ from the recording late in long
matches when replayed (a trail effect that waits for an asset loading in the
background), in straight and stress runs alike: loading, not rollback.

### Recording niche scenarios

Play the scenario in a normal session (a stage KO in the China corner, a
Start skip right as a round ends, a character with a special intro), note
roughly when it happened, and pass on the file name from `Polarity\matches\` (either
PC's file works; both halves of a desync are best). Then
`replay <file> stress 8` checks the whole match under constant rollbacks,
and `watch` around the moment shows what moved.

## Tested failure classes

`tests/test_netplay.cpp` runs full netplay matches between two `MockGame`s over a
lossy, jittery loopback network (≈670 rollbacks per 50-second match) and checks
that the tooling identifies each injected bug:

| Injected bug | Live | `diff` | `verify` |
| --- | --- | --- | --- |
| none | no desync | identical | both files match, also under stress |
| different character on one peer | desync | setup differs, state before frame 0 differs | – |
| machine-specific value in damage (P2 only) | desync | nondeterminism at the exact frame, `fighters+0x38` (P2 HP) | P1 matches, P2 diverges at that frame |
| camera outside the snapshot | desync | nondeterminism, `fighters` | both diverge; a no-latency recording passes straight and fails under stress |
| hit effect draws gameplay RNG only when not rolling back | desync | nondeterminism, `rng` | diverges under stress |
| recording from a different build | – | – | refused |
