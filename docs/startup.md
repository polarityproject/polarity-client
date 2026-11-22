# Startup, settings and reports

How the mod starts with the game, the files it reads, and what it reports when a run ends badly.

## Loading

The loader proxy loads `polarity.dll` while the game's exe resolves its imports, before the SteamStub entry point has run. `DllMain` starts `ModMain` on a thread of its own, so hooks and file I/O happen off the loader lock. `steam_api64.dll` is delay-loaded by the game's own code, so the mod waits for it (up to 2 minutes): its arrival means the stub is done and the game is running.

`ModMain` then, in order:

1. Works out the instance. Several game instances can share the folder. Instance 0 uses the plain file names and instance N appends `.N`.
2. Moves an SFVR install's files to Polarity's names (below).
3. Opens the event log in the `Polarity` folder, with the last run's log moved aside first.
4. Reports the last run if it ended badly, from its log before the log moves aside. Then this run counts as running.
5. Reads the mod paks' indexes (a few ms each) to learn which of them change battle data.
6. Merges the content the Steam account owns into the profile as it loads (below).
7. Installs the hooks. The netplay session and the replay check drive the game through `SfvGame::BeginMatch`, `ReadLocalInput` and `Advance`, which need the input and tick hooks. Until those are in, the mod stays in probe mode.

The noise mask that desync checks apply is built in, so every copy of a build masks the same words. `[mod] noise_file` swaps in a file, for learning new words (`stress` and `learnnoise` write to it). Checksums use no address test: whether a value is an address in use is a property of one process, so a value equal on both machines could be masked on one side only. Pointer fields are masked by the shared noise file instead (`polarity peers --pointers`), the same way on both machines.

When the game exits or the DLL unloads, the run counts as ended well, unless a fault came just before.

### Hot reload

The loader proxy asks before a hot reload. It is refused while a netplay session is on, since the reload would end it for both players. It is also refused while a frame driver still runs the battle's frames, from a leave until the game is back at the menus: the game thread can be inside the old driver's frame longer than the shutdown waits, and it went on in the freed driver after the reload (a crash, 2026-10-28).

Before the unload, the mod's threads and frame driver stop while the hooks still run. Then the hooks go off and the mod waits up to 5 seconds for threads in the middle of a walk over every UE4 object (about a second each), plus 50 ms for a detour that has returned from its guard but not yet from its function. After that no Present runs the mod's code, and ImGui and its D3D objects can go.

## The offline start

The game's first scene, `InitialState`, runs one step function at a time (at `+0x338`, after `+0x378` frames). Its steps prepare locally (`0x600D20` to `0x6014A0`), then `0x601820` starts the login to Capcom's servers and waits for it, about 30 seconds when they can't be reached. The mod waits once per frame for `InitialState` to reach that step, then replaces it with the scene's own "continue offline" step (`0x5E4ED0`). That step clears the online mode flag (global, rva `0x3A91FC1`, via `0x69D800`), marks the scene manager (`+0x451 = 0`) and switches to the main menu. The scene schedules it itself when it gives up on a login. `OnlineSubsystemKiwi +0x1022` is the flag the login dialog's "offline" choice sets to 0.

Network calls during the start are logged with how long they took.

The login watch reads the login state through the Blueprint `GetLoginState`'s native body: online subsystem, then identity, then `GetLoginStatus` for the local user (`0x73BB90`). That getter takes the identity interface (rva `0x73BEC0`) without a null check (`0x73BBCB`). Early in a start the online subsystem can have none yet, and the watch's call ended the game there: an access violation reading 0, 5 to 50 seconds after the start, in six crash reports from 0.2.6.1 to 0.3.1. The call now runs under the crash guard, and a fault reads as status -1.

The "free play period has ended" check is forced to pass. `freeplay off` lets the game decide, and `freeplay fail` makes it say the period ended (research).

## Settings

`polarity.ini` (or `polarity.<n>.ini` for instance N) holds the settings, and the panel's file holds the player's choices made there.

Values are read as UTF-8 from the file itself. Windows' ini functions read a file without a byte order mark in the system's code page, which mangles UTF-8, the encoding Notepad saves. A UTF-16 file (with its BOM) works too. The panel keeps a name outside ASCII as hex (`name_utf8`), since Windows' ini functions write a file without a BOM in the system's code page.

The player's choices in `[netplay]`: the pick with its variant, the stage, the input delay and the player's own frame rate in the HUD; from the panel also the name and spectators. Read from the overlay's file, a missing key keeps the value read before.

- `fixed_delay` is `auto` or 0 to 8 frames. Before it, `delay_setting` had 0 for Auto and 1 to 8; those carry over.
- The stage is a stage code (`CHN`), `random`, or nothing, which keeps the stage the game has prepared. It is resolved against the stage table when hosting. The stages Random leaves out (`CHN,S25`) are kept once the player has a list.
- Each character's loadout is kept for when it is picked again: costume, color, comment, V-Skill, V-Trigger and variant, as `KEN=3 5 0 2 1 0`.
- The Debug tab's server switch keeps the servers used before.

Updates replace the DLLs and never the ini, so old keys stay in players' files. Some keys changed name for that reason:

- An install from 0.2.x keeps `[weyve] server=159.69.32.141:5555`, the public server before 0.3.0 (kept up for 0.2.x only). A saved choice of it means the public server, which is the rooms VPS now (`kDefaultServer`). It is dropped from the recent servers.
- `lockstep_round_stages` replaces `lockstep_presentation`. Every release's ini had `lockstep_presentation=1`, so the old key is left unread and the round's stages roll back by default.
- The save state hotkeys (F9 and F10 load states in any battle, offline ones too) are off unless asked for. Releases up to 0.1.3 shipped `savestate_hotkeys=1`, so that key is no longer read.

## Moving an SFVR install

Before it had its name, the mod was SFVR. At start, `migrate` moves what the `SFVR` folder has and the `Polarity` folder lacks; folders both have are merged, and a file Polarity already has byte for byte counts as moved. The bundled font compares the same way, since a test script can put it in the Polarity folder before the first start. What stays is listed in the note `Moved to Polarity.txt`, and a later run deletes an earlier run's note.

- The ini files `sfv_rollback.ini`, `sfv_rollback.<n>.ini` and `sfv_rollback.panel[.<n>].ini` get Polarity's names. In a moved ini, the settings whose SFVR defaults named what Polarity renamed (`[update] url`, `[mod] report_dir`) are cleared, so Polarity's defaults apply.
- In `Polarity\`, the event logs and the packed-logs zips get Polarity's names, so rotation and pruning see one set.
- SFVR's DLL and the files that came with it are removed once Polarity's DLL is there and Polarity's loader, which loads it, runs.

Instances that start together (`multi_instance=1`) share the move: one moves the files, and the others wait for it (up to 30 seconds) and then find nothing left to do.

## The profile and Steam content

The game's save path, at rvas of the final Steam build:

| rva | What |
| --- | --- |
| `0xE138F0` | a save slot's loader (virtual; `this` is the slot, `+0x41` its loaded flag). It reads the file and decodes it, or, when there is none, builds a default profile and writes it. |
| `0xE18D10` | decodes a file's bytes (`TArray<uint8>`) in place, after the read |
| `0x113C220` | `FMemory::Realloc(ptr, size, alignment)` |

A slot's file name is an FString at `+0x48`. The save system keeps the file at `%LOCALAPPDATA%\StreetFighterV\Saved\SaveGames\<name>.sav`.

### The owned content

The profile keeps the content it owns in `EntitlementsData`: `EntitlementVersion`, then `AuthorizedEntitlementIds`. The game keeps that list only at the final data's version, 9. A profile saved with another version, or none, has its list replaced by the free content when it loads.

The mod merges in, after the decode, the codes the Steam account's apps grant. Ownership comes from `ISteamApps::BIsSubscribedApp` (vtable slot 6) for every app in the grants table; the Steam client answers from its license cache, with no server involved. The free content is the `EntitlementSpec` rows (`DA_Spec`) with `DefaultValue` set. Every profile also gets content only a CFN feature unlocked, which no one can get any more: the Dojo stage (StageSpec 50, `DOJ`) came with joining a clan on CFN. `[mod] steam_entitlements=0` turns the merge off.

The codes go in as new array elements at the end of the list, or as a new array tag before the object's `None`. Nothing changes until all of them are in, so a failed insert leaves the save as it was. When the list is at another version, it goes to the current one; if that fails, the codes are in but the game replaces the list with the free content this time.

A fresh profile has no name, since CFN gave it the fighter ID. The Steam persona name goes in as `MainUserProfileDetails.ProfileDataset.FightersId`. The default profile leaves `ProfileDataset` out (all its fields at their defaults), so the struct goes in with that one field. With no save file, the loader builds a default profile and writes it without decoding anything; loading that file back (the loaded flag cleared) sends the fresh profile through the decode like a saved one.

### A save's plaintext

After decoding, a profile save is `GPRG`, an int32, then sections (a four-character code and an int32 version) of tagged properties. Each tag list ends with `None` and an int32. A tag is an FString name, an FString type, an int32 size and an int32 index, then by type:

| Type | Then |
| --- | --- |
| `StructProperty` | FString struct name |
| `BoolProperty` | the value byte |
| `ByteProperty` | FString enum |
| `ArrayProperty` | FString inner type |
| `MapProperty` | two FStrings |

Then come `size` bytes of value. An `ObjectProperty`'s value is the object's own tag list. An array of structs is an int32 count, then each element's tag list. FStrings in a save are ANSI with the NUL counted; UE4 writes UTF-16 with the length negated when a string has characters outside ASCII.

### Empty saves

The game decodes whatever the read gave, and the decoder writes through the array's data even when the file was empty (0 bytes, from a write that failed or had not finished): a null pointer then. An empty array is left alone, and the loader takes it as a file with nothing in it. The slot then holds nothing from its file: the file is deleted if it is still empty, so the game builds and writes a default one, and the slot loads again, once. `[research] empty_save_test=<slot name>` takes that slot's first read as empty, to test this without an empty file.

### UE4 layouts read here

A `UDataTable` has its `RowStruct` at `+0x28`, the row map's elements (0x18 bytes each: an FName, then the row) at `+0x40` and their count at `+0x48`. A struct's first property is at `+0x38`; a property's next is at `+0x28` and its offset in the row at `+0x4C`.

`KWUserProfileDetails`, read for the player's favorites:

| Offset | Field |
| --- | --- |
| `+0x28` | bit 0: the main user |
| `+0x68` | `KWFavoriteBattleSettings`: `FlowCharaCode` (FString) |
| `+0x90` | `FlowStageCode` (FString) |
| `+0xA0` | `CharaSettings`, a TArray of 0x38-byte entries |
| `+0x108` | `KWRandomStageSelectSetting`: `MaskStageCodes`, a `TArray<FString>` |

The profile classes are looked up by name once: a name lookup per object held the game about 180 ms.

## Crash reports

A vectored exception handler catches faults. The faulting thread copies the fault and waits while a writer thread, with room on its own stack, writes a minidump and a note into `Polarity\crashes`. A run writes at most 5 dumps; the latest stays. The note has the exception, the address read or written, and the stack, walked through the x64 unwind data as `module+offset` (a leaf function has its return address at the stack's top). A fault while walking is the writer's own business and isn't reported.

When Wine lacks a function (an import it left as a stub), it raises `0x80000100` and the process ends right after. The record names the DLL and the function, or the ordinal for an import by ordinal; the note quotes both.

Exceptions that are messages and not errors (debug output, thread names) are ignored. Any other exception the handler does not count as a fault (an engine error raises one before the engine ends the game) is kept in memory with its stack, on the raising thread's own copy, and quoted if the game then ends itself within 10 seconds.

### How a run ended

Each run leaves a mark (the running file) that a clean exit removes. The next start reports a run whose mark stayed.

- The game ending itself while the player is not quitting is a crash the engine handled itself: an error code (a failed check, a lost graphics device: exit code 3), or an exit right after an exception the handler doesn't count as a fault. Who called and that exception go to the log and to the running file, which stays. 0.2.6 counted those as clean exits, so a Steam Deck that crashed on a rematch sent nothing.
- An exit within 15 seconds of a fault belongs to that fault, and its report says so.
- A run with no fault that was quitting already (its window closed, after which the process ends in a way the hooks don't see) isn't a crash. Neither is a run that ended seconds after starting, before the exit hooks were in: the game started outside Steam, which starts it again and ends this one.
- The mod hooks `TerminateProcess`, `ExitProcess` and the `exit` of the game's runtime (Visual C++ 2013, `msvcr120`). That `exit` runs the static destructors, where the game faulted now and then; the hook marks the run as quitting first, so such a fault isn't reported. Once the hooks are in, an exit is seen, and a run that ends sooner is reported only if it faulted.

A crash the game handled itself leaves its own minidump (`%LOCALAPPDATA%\StreetFighterV\Saved\Logs\Dump<n>.dmp`). The newest one written since the run started goes with the report when the mod has none. The report also carries the last 256 KB of the run's log.

### Hangs

Once a second the writer thread checks the game thread, the one that ticks the scene. A game thread that has not ticked for 20 seconds is stuck. A freeze that is later ended by force otherwise shows only as "no clean exit", so where the thread stands goes to the log, which that run's report quotes. So do the other threads with a frame in the game or the mod (up to 40 distinct stacks): a game thread waiting on another (the render thread, the loader) shows the wait, and only those threads show what it waits for. Threads with the same stack, such as an engine pool waiting for work, share a line, in the order first seen. A thread is suspended only to read its registers; its stack, which a stuck thread leaves as it is, is walked after it goes on, so nothing waits on a lock the stuck thread could hold.

## Report uploads

Desync reports and crash reports go to the report server (`[report] url`, else `kDefaultReportUrl`) unless the player turns uploads off.

- A match's files are grouped by side: `<...>_<match id>_<p1|p2|spec>[_desync].<sfvr|log>`. A side is ready once its match log is there. Without one the match may still be on, unless the files are from before this run (the game closed or crashed in the match); after 30 minutes a report of this run goes without its match log.
- Crash reports are `<date>_<time>_crash_<id>.<log|dmp>`, each report's `.log` and `.dmp`.
- Files over 16 MB (the server's limit, such as a full recording made in testing) stay home, and so do reports older than 14 days.
- `uploaded.txt` lists the files sent. `kept.txt` lists files made while uploads were off, which are never sent.
- The first look comes 30 seconds after the mod's start, then every 5 minutes while something is left (the server not reached, a match log to come), but not during a match, where the upload would share the line with it.
- A file the server refuses for what it is (400, 404, 411, 413) is marked sent, since sending it again would not help. A server that can't be reached or is busy ends the round; the rest waits for the next.

## Logs

The `Polarity` folder next to the game (`Binaries\Win64\Polarity`) holds what a player shares. Each run's event log moves aside at the next start, named by when the run started: its first line, else when it last wrote. A log whose first line is `SFVR log started` is from before Polarity (the migration renames them).

"Pack logs to share" writes `Polarity\Polarity-logs-<date>.zip`, with its entries stored, not compressed, since the recordings already are. It holds the event logs (this run's, still open for writing, and the earlier ones kept), the benchmark's results and the state dumps of its latest failed runs, the latest crash reports (notes and minidumps), and the latest matches: their logs and every recording with the same match id, the match's own and its desync reports. Match file names are `<date>_<time>_<P1>_vs_<P2>_<id>_<side>[_<kind>]`, with `_` only between the parts. The log lists the game's paks (`pakchunk<digits>-WindowsNoEditor.pak`) as a count and size, and every other `.pak` by name.
