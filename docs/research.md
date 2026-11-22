# The game's objects, textures and paks

How the mod reads the engine's objects, textures and pak files, and the probes it uses to study the game. [commands.md](commands.md) lists the research commands.

## Objects and names

`ue4.cpp` walks the engine's objects. `GUObjectArray` keeps them in a `TArray<UObject*>` (data, int32 count, int32 max) in the image, at rva `0x3978730` in this build. The engine reallocates it as it grows, so its data pointer is read for every walk; an address found once goes stale. Freed slots are null or hold a stale object, and a walk skips them. On an image that is not the known build, the array is found by scanning writable memory for a known object (the engine's game singleton, `GEngine` at rva `0x3D878C8`, `+0x1E0`) and trying the strides UE4 builds use. That copy is valid until the engine reallocates the array, and since its count is unknown, a long run of empty slots ends the walk.

Names come from the `GNames` chunks (rva `0x3A75490`, 16384 entries a chunk), found by scanning the image's writable sections on another build: the first two entries are `None` and `ByteProperty`. An `FNameEntry` is an int32 index, a `HashNext` pointer, then the text; an odd index marks a wide (UTF-16) entry.

| Offset | Field |
| --- | --- |
| `UObject +0x0C` | its index in the object array |
| `UObject +0x10` | its class |
| `UObject +0x18` | its name (`FName`: index, number) |
| `UObject +0x20` | its outer |
| `UStruct +0x30` | `SuperStruct`, after `UField` (UObject's 0x28 bytes, then `Next`) |
| `UStruct +0x40` | the size of an instance |

Every read goes through a guard that fails instead of faulting: the engine can free or move what is read (its object array grows while a battle loads on another thread), so a check before the copy is not enough. A host crashed in a walk over the objects that way on 2026-10-30.

## Textures

A texture's render resource and its RHI texture are native objects, with their vtables in the game image, since the D3D11 RHI is linked in. The RHI texture holds the D3D11 objects, and the one that is a shader resource view is what ImGui draws. `textures.cpp` looks along that chain twice. The first pass takes only an object with the vtable of the device's shader resource views, learned from the device. The second pass, on Windows only, takes any object whose vtable lies in `d3d11.dll` and asks it whether it is one. Under Wine `d3d11.dll` is DXVK, whose own C++ objects have their vtables there too, and asking one of those called a null slot (Steam Deck, 0.2.5, crash df1a180a).

`savetex` writes a texture's top mip as a DDS file: the magic, the 124-byte header, a DX10 header when needed, then the rows. Uncompressed textures are written as 32-bit RGB with alpha by channel masks, since DX10 headers of the typeless formats the game uses are not read widely. BC1 to BC3 get their legacy four-character codes, which readers know best; other compressed formats get a DX10 header.

### Textures from the paks

The mod reads textures from the paks on its own threads (2 workers) through the engine's platform file chain: `FPlatformFileManager::Get()` (rva `0x1131AD0`), then `FindPlatformFile(manager, L"PakFile")` (rva `0x1133090`). The pak platform file's vtable (rva `0x33C49D8`) gives `GetName` (slot 8), `FileSize(path)` (slot 10) and `OpenRead(path, allowWrite)` (slot 19); the handle gives `Read(dst, bytes)` (slot 4) and its scalar deleting destructor (slot 0). When this build's platform file is not the pak file, the game's own loader draws instead.

A texture path such as `/Game/Chara/RYU/Hud/PartCharacter` maps to its package's name in the paks. A file over 64 MB is not read. The look of an image is worked out from its largest mip, since BC images are not shrunk otherwise. Images read wait for the render thread, which makes a few textures a frame, since each costs a copy into the device; those not reached wait for the next frame. A new device gets nothing from the last one: textures are made again on request, and a view still drawn this frame stays valid, since it holds its texture. The batch's log line gives the frames presented while it loaded, the longest, and how many took over 20 ms.

`core/texture_package.cpp` reads a texture's package without the engine. It
looks for every `PF_` format name that is an FString with a plausible header
before it (the name table has the same strings without one), tries the
game's own layout first (a `bCooked` flag, no `SizeZ`), then the others.
Each mip is a `bCooked` int32 where the layout has one, the bulk data, then
its sizes. Offsets of data at the end of the file count from where the
package's bulk data starts: the end of the file less all of it, before the
package tag that closes the file. Each mip halves the last, down to 1;
block formats stop at one 4x4 block, so the pad glyphs' 2x2 and 1x1 mips say
4x4. A zlib-compressed mip is not read. BC1 and BC3 decode to BGRA here,
premultiplied so clear pixels don't darken what they touch. The noise
texture is made rather than read: xorshift32, two bytes averaged so the
grain is softer, and lighter or darker than what is under it, never greyer.

### The mod paks' indexes

`core/pak_index.cpp` reads a pak's index to learn which files it holds, such as battle data. The footer is the magic, the version, the index offset, the index size and the index hash (44 bytes); versions 1 to 3 are read. The game's own paks have an encrypted-index flag byte before the footer. Paks made with older tools (u4pak, UnrealPak before 4.16) end their index right at the footer, and reading the flag as given turned every mod pak of a 0.4.2 host into "index out of the file" (2026-11-13), so the flag is read only when there is room for it. An index entry is a path (an FString: its length with the terminator, negative for UTF-16), the offset, the size, the uncompressed size, the compression, a timestamp in version 1, a 20-byte hash, and in version 3 the compression blocks, the encrypted flag and the block size. Paths are made relative (`../../../StreetFighterV/Content/Chara/...` becomes `StreetFighterV/Content/Chara/...`), and battle data is found by its place: `Chara/<C>/BattleResource/[<v>/]<file>`, and stage data at `Chara/<C>/DataAsset/StageData/DA_<C>_Stage_<S>`.

## The start probe

`startprobe` answers questions in a local Versus match:

1. Is `RollbackUnit` enabled offline, and does `Pre` open snapshot frames? `Pre` takes its frame id from the online sync object and opens nothing offline; the log shows `open=0` then.
2. How deep is the ring, how big is the state, how many regions are there, and how many of their words look like pointers?
3. Does restoring work? F5 and F9 copy the regions out and back, as the netplay session does; F6 and F10 use the game's own ring and `Restore`. With `force_offline_frames=1` the probe opens frames itself, with its own id, so the game's Store tasks run offline. The keys work only with `[probe] debug_hotkeys`; scripts' commands always work.

The probe's hook runs at the start of a battle tick, before the game opens the tick's frame, so the state is the end state of the previous tick and nothing is mid-update. It also logs the SSE control word (MXCSR) whenever it changes. Float results depend on it (rounding, flush-to-zero, denormals-are-zero), and overlays and drivers injected into the process can change it on one machine only, which desyncs any float math in the simulation. The default is `1F80`; the low six bits are sticky exception flags, not settings.

## The outside probe

`outsidetest [ticks] [all]` looks for state outside the snapshot. It saves the snapshot, runs ticks with neutral inputs, loads the snapshot back, and compares memory with a copy taken before, leaving out what changes anyway without ticks. By default it compares the memory of every object in the object array and lists the objects changed and not restored. It shows every word for the game's own blueprint objects (`KAB_` animation, `KBP_` battle) and a few for the rest. With `all` it covers all writable memory instead: hashes of the process's committed private read-write pages (4 KB each, without its own thread's stack) find the pages the ticks changed and the snapshot did not restore, and a second run compares those pages' bytes, naming the object that holds each word. The probe runs for one frame and then removes itself.

## The call tracer

`trace <rva>...` hooks game functions with small stubs that count calls and keep the last `rcx` and return address; `tracereport` logs them. Each stub increments its count (`lock inc`), stores `rcx`, loads the return address from the stack and stores it, then jumps to the original function. The stubs are allocated near the image so MinHook's relative jumps reach them.
