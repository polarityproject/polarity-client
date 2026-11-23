# Commands

The mod reads commands from `sfv_cmd.txt` next to the game (`sfv_cmd.N.txt`
for instance N) and deletes the file once read. Each line holds one
command; `;` separates several on one line in the scripts, and lines
starting with `#` are skipped. Pad steps and waits run in order on a thread
of their own; a new file is read at once, and its steps queue behind those
still running. Game-thread requests wait for the next battle tick, except
`watchaddr`, `watchreport` and `unwatch`, which act at once on any thread,
also outside battles. Every other command runs on the command thread. [testing.md](testing.md) shows how
the scripts use them.

Commands from scripts run a session as a test does: the host and the joiner
are ready at once, and the player is named "Player <instance + 1>" unless
`name` sets another.

## Steps and game-thread requests

| Command | What it does |
| --- | --- |
| `pad <user\|both> <buttons> [hold ms] [gap ms]` | presses virtual pad buttons, joined by `+`: `up down left right start back lb rb a b x y lt rt`, `sup sdown sleft sright` (the left stick), `none`. A hold of -1 keeps them pressed until the next pad step; `both` presses pads 0 and 1 together |
| `wait <ms>` | waits before the next step |
| `connect <user>`, `disconnect <user>` | a virtual pad's presence |
| `setup <code> <costume> <color> <comment> <vskill> <vtrigger>` (twice) `<stage>` | the next battle's setup, as the menus show the values; `<code>:<n>` also sets the costume's variant |
| `showsetup`, `clearsetup` | logs the battle setup now; drops the pending one |
| `save`, `load` | a copy-mode savestate and its load |
| `ringsave`, `ringload` | the game's own ring savestate and its load |
| `synctest <frames> [seed]` | the in-game determinism test (synctest.h) |
| `stress <frames> <distance> [seed]`, `learnnoise <frames> <distance> [seed]` | the rollback sync test, and the same while learning the checksum noise mask |
| `watch <region> <offset> [len]`, `watchaddr <hex address> [len] [rw]`, `watchreport`, `unwatch` | hardware write watches (watch.h); `rw` traps reads too |
| `scan <int>`, `rescan <int>`, `scanshow`, `scansmall`, `scandelta <n>`, `scanrecord <hex>` | the value scanner (memscan.h) |
| `trace <rva>...`, `tracereport` | the call tracer (tracer.h) |

## Sessions, screens and other tests

| Command | What it does |
| --- | --- |
| `host [random [costumes]] [manual]` | hosts a session; `random` picks random characters (and costumes), `manual` leaves the lobby's ready to the player |
| `join <code> [manual]`, `spectate <code> [manual]` | joins a session as the other player or as a spectator |
| `ticket host \| join <code> \| watch <code>` with `token <n>`, `set`, `rules <rules>` | a session from a match ticket, as tables start them: the P2 seat's token, a table's set instead of a vote, and the set's rules (core/rules.h) |
| `leave` | leaves the session |
| `ready [0\|1]`, `choose rematch\|select` | the lobby's ready; the post-match choice |
| `delay <frames>` | this player's input delay for the next session |
| `name [text]` | the name test sessions use; none: back to "Player <instance + 1>" |
| `pick <code> <costume> <color> <comment> <vskill> <vtrigger>` | this player's pick; `<code>:<n>` also sets the costume's variant. `ELV` plays Eleven: a random fighter in silver, with V-Skill and V-Trigger at random |
| `stage <code> \| <table index>`, `stage` | the host's stage, also in a session for its next match; none: what the game has prepared |
| `nextstage <code>` | the stage the next battle requests |
| `setstage <index>` | writes the battle data's stage index now |
| `battlenow`, `battle` | jumps straight into a local battle; logs the battle's state and frame rate |
| `menu` | back to the main menu |
| `park` | leaves a running battle for stage select (the pause menu) |
| `canceljump` | calls off the jump under way, as a session going back to the lobby mid-jump does |
| `netsim <latency ms> <jitter ms> <loss %>` | simulated network conditions on what this side sends |
| `netstatus` | logs the session's state |
| `mash <seed> [from frame]`, `mash loop <buttons>:<frames>...` | the local player mashes from a seed, or plays a loop of inputs (buttons as for `pad`) |
| `dumps <every N frames> [max]` | state dumps in match recordings |
| `recordings full\|compact` | whether saved recordings keep every frame's region hashes or only those around desyncs |
| `server [host:port [also port]]` | the rooms server for the next session, in this game only; the second port is tried at the same time. None: the default |
| `transport udp\|weyve` | the next session's transport |
| `relayonly on\|off` | the next sessions' links go only through TURN |
| `presence open\|status\|close` | a second Weyvelength connection beside the session's, with a room of its own, logging what it sees |
| `rooms open \| close \| status \| create [tables] [public 0\|1] [title] \| join <code> \| leave \| chat <text>` | the hub's presence connection (rooms_client.h), driven by hand; `open` and `create` work from the keeper's desk, as the rooms screen does |
| `rooms table <seat\|stand\|queue\|unqueue\|ready\|decline\|rules\|kick> [args]`, `rooms watch <table \| -1>`, `rooms mute <member id>` | a table request; watching a table (tables count from 0), as X on the room screen; muting a member, as A on their row |
| `matchchat <text>` | chats into the session's match room |
| `watchmatch <n \| live>` | a spectator watches the n-th kept match (1: the oldest) or the newest from its start |
| `watchchat toggle \| type <text> \| say <text>` | the spectators' chat box: shows or hides it, starts typing that text, sends it |
| `startskew` | this side's next frozen start hashes differently, so the session must end before the match |
| `setrounds <n>` | the battle data's round count (+0x08) that setups write, as a game that kept another count would; 0: the match's own |
| `badpick <field\|off> [message]` | what this side sends carries a pick or stage no game has (core/bad_pick.h): `character`, `costume`, `color`, `vskill`, `vtrigger`, `comment`, `variant`, `stage`, in `state`, `session`, `welcome`, `loaded` or `matches` (all when left out) |
| `pickcheck` | runs every pick the select screen offers, and every selectable stage, through the check on other PCs' picks (core/pick_check.h), and logs what it refuses |
| `randomoff [codes]` | the Random stages list's stages left out, for sessions hosted from now on (not saved) |
| `randomstages [n]` | n random stage draws, counted, as the host draws them (owned stages from the Random stages list, and from any stage) |
| `countryflag on\|off`, `wordfilter on\|off` | the Country flag and Word filter settings for this run (not saved) |
| `voicelang ja\|en\|auto` | every character's voice in the next battles; `auto`: each one's from the Sound options |
| `vsync [on\|off]` | the swap chain's vsync, then logs its state |
| `freeplay on\|off\|fail` | the free play check: passed by the mod (`on`), the game's own answer (`off`), or answered "ended" to show its dialog (`fail`) |
| `overlay show \| hide \| toggle \| preview <words> \| select [stage\|grid\|loadout\|ready] [spectate] \| rooms \| hub \| battlelog` | the overlay and its screens; on the hub the pad's A goes to the tile under the mouse |
| `settings [row] \| settings <row> type [text] \| settings close` | the settings screen: on a row, typing text in it |
| `wheel <x> <y> <notches>` | the mouse wheel over a spot of the mod's screens next frame (design coordinates, 1600 x 900; up is positive) |
| `uistart [ms] [buttons]` | buttons (default `start`) on the game's own pad input |
| `uipads 0\|1` | 1 lets the game see the local pads (with Start and Back) during a netplay match; 0 isolates them again |
| `uisound <name \| cue number>` | plays one of the mod's menu sounds (ui_sound.h) |
| `shot <path> [scale]` | the next frame as shown, to a PNG (path relative to `Win64`) |
| `shotburst <dir> <frames> [scale]`, `burston <code> <dir> <frames> [scale] [count]` | a burst of frames to PNGs, now or on each of the next `count` flow events `code` |
| `echo <text>` | logs the text once every step before it has run; scripts wait for it |
| `replay view <file> [hold <frame> <seconds>]` | the Battle Log's playback of a recording (`rview` controls it) |
| `replay <file> [stress <distance>] [watch <from> <to> <region>] [keep <from> <to>] [cold] [trap <frame> <region> <hex offset> <bytes>]... [hold <frame> <seconds>] [mispredict <delay> [remote 1\|2]] [latch tick\|game]`, `replay stop` | plays a recording back in the game ([desync-replays.md](desync-replays.md)) |
| `rview pause \| play \| toggle \| step <n> \| seek <frame> \| round <1\|-1> \| fast <n> \| leave \| status` | the viewed replay's controls |
| `rview runticks <n>` | research: a run's ticks per rendered frame at most (default 16; 0: no cap) |
| `benchmark`, `benchmark dump <frame>\|off`, `benchmark file <path>` | the built-in round under rollback stress 8, as Settings starts it; with `dump`, runs save that frame's state |
| `update [check [url]] \| install \| pretend <version> \| restart` | the updater (updater.h) |
| `reports [send] [url <address>]` | desync report uploads (report_upload.h) |
| `packlogs` | what Settings, Logs, "Pack logs to share" does |
| `crash game\|thread\|overflow\|hang\|fastfail\|raise\|stub\|exit`, `crash dismiss` | ends or freezes the game for real, to test crash reports (`dismiss` presses OK on the engine's error dialog, as a player would): `raise` is an exception that is not a fault, left to the engine; `stub` is what Wine raises for a function it lacks; `exit` ends the game with an error code, as the engine does after an error |

## Research: the game's objects, memory and tables

| Command | What it does |
| --- | --- |
| `peek <hex address> [qwords]` | logs memory, with image addresses as RVAs |
| `poke <hex address> <hex value> [bytes 1\|4\|8]` | writes memory (testing only) |
| `findbytes <hex bytes>` | where the bytes are in writable memory |
| `findptr <hex value>` | up to 32 addresses holding the 8-byte value (who points at an object) |
| `mapat <hex address>...` | commits 64 KB of zeroed memory at each address, for a test that depends on an address being mapped |
| `owner <hex address>` | the UObject whose memory holds the address, and the offset |
| `members <hex address> [bytes]` | names the UObjects the fields there point at |
| `find <text>` | objects whose name contains the text, up to 60 |
| `named <name>`, `uobj <name> [dump]` | every object with that name, with its outer (and class) |
| `ofclass <class> [max]` | live objects of that class or a subclass |
| `classes <text>` | classes whose name contains the text, with their live object counts |
| `classinfo <class>` | its fields, with the raw words that hold each property's offset (+0x4C) and size. A struct's fields start at its +0x38 (UField chain), its size is at +0x40, and an array or struct property's inner property or struct is at +0x70 |
| `enuminfo <enum>` | its names and values |
| `ufunc <name>`, `functrace <name> <owner\|-> <hex offset>` | a UFunction's owner and native code; logs its native calls |
| `relocate` | finds the object array again |
| `packages mark`, `packages new` | remembers the loaded packages; logs those loaded and gone since |
| `scene <StateClass> [object]` | switches the game's scene state |
| `worldctx` | logs the world context |
| `gfx [vtable words]` | the live Scaleform players: movie path, step mode, movie object and native player |
| `datatable <name> [filter\|-] [limit]` | a data table's rows |
| `charas`, `versions`, `charcache` | the character table; every character's versions; the character cache's slots |
| `costumes <code>`, `presets <code>`, `variants` | a character's costumes and material presets; every costume's variants |
| `stages` | the stage table |
| `preload <code> [costume]` | loads a character into the game's cache |
| `textures <filter> [limit]` | loaded textures by name or package |
| `showtex <name\|path>`, `showtex off` | a texture drawn by the overlay |
| `savetex <name\|path>...` | each texture's top mip to `Polarity\textures\<name>.dds` |
| `paktex on\|off \| status \| show <path> [max side] \| save <path> [max side] \| format <path> \| dump <path>` | textures read from the paks (pak_textures.h) |
| `keyconfigs` | every user profile's button settings |
| `layout [16 hex values]` | logs pad 0's live Button Settings table, or replaces it for this run ([local-input.md](local-input.md)) |
| `keys <key names> [hold ms]` | presses keys through the system, the game's window brought to the front first (an Alt tap lets a process that is not in front take it): letters, digits, `up down left right enter`. It logs how many the desktop took: none while locked or in a disconnected remote session |
| `input2 <key> <value>`, `input2 restore` | a key of `[AssignKeyboard]` in the game's Input2.ini, from the next start; the first change keeps a backup |

## Research: battles, rollback and replayed ticks

| Command | What it does |
| --- | --- |
| `rsnap`, `rdiff side\|all [prefix]` | snapshots the regions; logs the words changed since |
| `rpeek <region> <hex offset> [qwords]`, `rcmp <a> <b> <hex from> <hex to>` | a region's words; two regions compared (offsets may be negative) |
| `rdump <region> <tag>` | a region's bytes to `Polarity\research\<tag>.bin` |
| `rfind f <float> \| i <int> \| bac <name>` | a value in the snapshot regions |
| `ptrscan <from region> <hex before> <hex after> <to region>` | pointers from one region into another |
| `regions <size>` | the snapshot regions of that size with their addresses and owners; 0: every owner with the sizes it holds |
| `ftrace <ticks>` | changed snapshot words per tick to `Polarity\research\ftrace.bin` |
| `watchname <region> <hex offset> <len>` | a write watch on a region by name |
| `watchregion <region> <hex offset> [bytes]` | the same, armed for the next battle |
| `watchphase` | a write watch on the phase machine's requested phase |
| `watchcamera [new]` | write watches on the battle camera from the next freeze, or from the next camera's construction |
| `phases`, `roundinfo`, `tasklist [calls]` | the phase machine; the round's state; a census of the tick's tasks |
| `actorlog <ticks>`, `cinelog 2\|1\|0`, `camtrace <from tick> <to tick> [class]` | script runners, cinematics and camera objects per tick |
| `camrollback 0\|1`, `camlog 0\|1` | camera views follow rollbacks (on by default); logs every view request |
| `hudstep 0\|1` | replay runs keep the HUD's movies in step (on by default) |
| `soundrollback 0\|1` | the game's own rollback of sounds (on by default; 0: replayed ticks play theirs again) |
| `soundlog <count>`, `bonelog <count> [caller rva]`, `drawlog <count> \| ticks <from> <to>` | the next sound cues, bone updates and draws |
| `frameids 0\|1` | the game's restore callbacks get the game's own frame id (on by default) |
| `kowaitmode 0\|1` | loads put back the mode the round's end waits on (on by default) |
| `leanresim 0\|1` | replayed ticks skip phase 8 and some phase-4 tasks (on by default) |
| `resimtasks <hex task rva> \| v<hex vtable rva>... \| off \| default` | the tasks lean replayed ticks skip |
| `resimskip 0\|1`, `phasecost 0\|1` | skips presentation in replayed ticks; times each phase |
| `serialjobs 0\|1`, `joborder [calls]` | driven ticks run the battle's jobs in order on the game thread (1, the default) or on its threads; logs their order |
| `skipcallbacks <thunk rva>/<target rva>... \| off` | game callbacks that loads leave out |
| `forcerollback <frame>... [xor <hex>]` | replays reload before those frames; with `xor`, the frames first ran on a wrong guess of the remote input |
| `ptrtest old\|new` | the pointer test loads use |
| `transitions 0\|1` | lockstep round transitions for the next sessions |
| `warmupsleep <ms>`, `warmupextra <n>` | each neutral warm-up tick waits first; more warm-up ticks once the objects exist |
| `emptystage` | the next jump's stage request is emptied |
| `startprobe`, `outsidetest [ticks] [all]` | the start probe; the probe of state outside the snapshot |
| `freeze 1\|0` | holds the battle on its frame |
| `framemeter 0\|1`, `fmlog <ticks>` | the frame meter in any battle; each tick's classification |
| `hitboxes 0\|1`, `boxlog <ticks>` | the box display in any battle; logs the boxes |
| `battledata [dump]` | the loaded move data against the game's own; `dump` writes their hashes |
| `dumpbac <name>` | a loaded move file to `Polarity\research` |
| `inputlog <ticks>`, `mappedlog <updates>` | the game's inputs per tick; the input manager's mapped value on change |
| `uniqueparam <player> <index> <value>` | a unique param for the next battles (-1 clears it) |
| `recdiff` | the int32 fields that differ between the P1 and P2 battle records |
| `winsize <width> <height>` | resizes the game window's client area |
