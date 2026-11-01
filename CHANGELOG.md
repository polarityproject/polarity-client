# Changelog

Releases of SFVR, the Street Fighter V rollback netplay mod. Each version's
first paragraph is the summary players see in the game (Settings, Updates);
`scripts/release.ps1` reads it, and refuses to publish a version that has no
entry here. Newest first; the format follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added

- Each side tells the other at the handshake whether it sends desync
  reports: the event log says so once, and every match log has a
  "desync reports: ... from this side, ... from the other side" line. A
  report whose other half never reaches the server now shows whether that
  player turned reports off or their game could not send them. Protocol 11:
  both players need this version.
- `setrounds <n>`: the round count match setups write, to test a game that
  kept another one.

### Fixed

- Desyncs from the first hit on players whose game had been left on another
  round count (the "Ken V-Trigger 2" desync, and a Ryu vs Urien match on
  0.2.2): each hit's damage gauge gain is divided by the battle's round
  count, which sessions took from each game's own settings (1 on those
  machines, 3 elsewhere). Match setups now write 3, and handicap 0.
- Reports that could not be sent (the server not reached or busy) are tried
  again every five minutes outside matches, not only after the next match or
  at the next launch.
- A desync report from a match the game closed or crashed in goes at the
  next launch, not half an hour into a later run.

## [0.2.2] - 2026-11-01

Stage settings on the stage select (Y): The Grid Alternative, Shadaloo Base
at Night and the rest. Characters speak the voice language from your Sound
options. Fixes a crash at startup with Special K, false desync warnings in
some matches, and crash reports for games that did not crash. Both players
need 0.2.2.

### Added

- Stage settings on the stage select: Y (or up and down) switches a stage to
  its other setting, as the game's own stage select offers them (The Grid
  Alternative, Shadaloo Base at Night, Suzaku Castle at Night...). The
  picture, name and strip icon follow it, "Setting 1 / 2" shows which, and
  the other player sees it live.
- Match logs and the event log name the game's data files: how many of the
  game's own paks there are and any other .pak under Content\Paks (mods).
  Two tester desyncs happened on one player's machine only.
- Match logs record every add to the fighters' damage gauge (the tick,
  vitality, damage, the amount and the move's divisor): one tester's game
  added 59 where the other added 20 for the same hit on Ken (V-Trigger 2),
  a divisor of 1 instead of 3, and the next such report shows where.
- A game frozen for 20 s logs where its game thread stands, so a freeze
  ended by force has its place in the crash report (a tester's game froze
  on the main menu after a session).
- `replay <file> mispredict <delay> [remote 1|2]`: replays a recording with
  one player's inputs arriving late, so frames run on guessed inputs and are
  rolled back as in a session (stress replays only resimulate with the right
  inputs). `regions 0` lists every snapshot region's owner.

### Fixed

- The game crashed at its first pad poll with Special K running: Special K
  hooks every XInput and took SFVR's loader for the original of System32's,
  so the two called each other until the stack overflowed (0.2.1). A call
  that comes back into the loader that way now goes to xinput1_4.
- Crash reports for runs that did not crash: a game closed from its window
  that then ended in a way SFVR does not see, and a game started outside
  Steam (Steam starts it again and ends the first one seconds in). A fault
  in either is still reported.
- Every character spoke Japanese in sessions whatever the player's Sound
  options said. Each game now gives both characters the voice its own player
  chose (Japanese, English, or per character with Custom); the other player
  hears their own choice.
- False desync warnings (adapter-8, so both players need this version):
  pooled effect objects' flag blocks (Menat vs Lucia), and an address and
  two groups of camera-like floats in one battle object that one machine set
  differently in Vega matches, are out of the checksum like the other effect
  state. The fight and its outcome never differed in those matches.

## [0.2.1] - 2026-10-31

A new character select for sessions: the host picks the stage, both players
see each other choose live, and characters load once both are ready. Desync
and crash reports go to the SFVR server to help fix them (Settings, Logs turns
that off). In a match, hold Start for the menu. Pads work on the Steam Deck.
Both players need 0.2.1.

### Fixed

- No pad input on the Steam Deck (Proton) with SFVR installed: the
  xinput1_3=n,b override that loads SFVR also made the game's XInput load as
  a copy that sees no pads. Under Wine SFVR now passes the game's XInput calls
  to Wine's own xinput1_4. The game's pad reading is hooked where the game's
  import leads, ahead of the Steam overlay's own patch there.

## [0.2.0] - 2026-10-31

A new character select for sessions: the host picks the stage, both players
see each other choose live, and characters load once both are ready. Desync
and crash reports go to the SFVR server to help fix them (Settings, Logs turns
that off). In a match, hold Start for the menu. Both players need 0.2.0.

### Added

- Desync reports are sent to the SFVR server, so a desync can be traced from
  both players' sides: after a match with a desync, its recording and match
  log (player names, picks, inputs). Settings, Logs turns it off; a notice
  says so once. Reports not sent (no network) go after the next match or at
  the next launch; `[report] url` sends them elsewhere. Needs sfvr-server with
  `sfvr-reports` (TCP 5580).
- Crash reports: when the game crashes, a small minidump and the fault with
  its stack are saved in SFVR\crashes, and at the next start a report with
  the end of the log goes to the SFVR server (the same setting turns it off).
  A run that ended without a clean exit and with no fault seen (a freeze
  ended by force, a crash past the handlers) is reported from its log.
  "Pack logs to share" takes the latest crash reports too. The `crash`
  command crashes the game on purpose, for testing.

- Eleven: pick it like any character and you play a random fighter in the
  silver model, with its V-Skill and V-Trigger at random (as the game's own
  Eleven). The other player sees ELEVEN. Each time it is picked it rolls again.
  When both players are Eleven, Player 2 wears the silver model's second
  palette (bronze), so the two can be told apart.
- Two players on the same character and costume cannot take the same color:
  stepping through colors skips the other player's (at the list's end, the
  step does not happen), the panel's list shows it greyed out, and a change
  of character or costume that lands on it moves on to the next. Should both
  still match when the match starts, Player 2 plays the next color.
- V-Comment: a row in the loadout with Auto and each character's own
  comments, by number with the chosen one's text under it. Both players see
  each other's.
- The panel's Debug tab has a Character select preview: the select screen as
  the game draws its own, with the game's art, real characters, costumes,
  colors, V-Skills, V-Triggers and V-Comments, and a mock opponent. Stage
  select first, then the character grid in the game's arrangement, each side's
  loadout and ready; pad, keyboard or mouse. "... as a spectator" shows the
  same screens watching: two mock players pick and ready up on their own, a
  WATCHING banner gives the spectator's link to the host, and Back leaves.
- Sessions use that select screen: it fades in when a session starts (players
  and spectators), and over the results screen in place of the vote's windows
  when both choose Character select. Into a match it stays up while the stage
  loads, then fades to black as the game's loading screen comes. The host picks the stage first (B on
  the grid goes back to it), then both pick a character and their loadout and
  say ready; B says not ready again until both are. Start brings up the
  panel, and Start closes it. Each side shows where that player is on their
  own screen: the fighter under their cursor while they browse the grid,
  their loadout (and the row they are on) once they picked. A picks; the
  characters load only once both are ready ("Both ready: loading the
  characters"), so trying out characters and costumes no longer stalls
  either game. Pads the game reads past XInput (DirectInput, Steam Input)
  work on the screen and in the panel too, and the game's menus behind the
  screen no longer react to them. The character lists are read once the game
  reaches its menus, and the screen's pictures load in the background on the
  way to a session (with the panel open at the menus, while hosting or
  joining; twice as fast as before), so the screen is complete and answers at
  once when it opens. Lobby protocol 10
  (the stage confirmation, each player's place on the screen): both sides
  need this version.

### Changed

- In a match the panel opens by holding Start for 2 seconds (a short bar
  fills at the bottom center under "Hold for menu..." meanwhile; a tap still
  skips intros and win poses), shows only its Match tab, and Start closes it
  again. Back + Start
  no longer opens it there; F1 still does.
- The game connects to the public server on 5555 alone, and on 443 too only
  when 5555 does not answer within 1.5 s. Both at once doubled the
  connections, and the server's firewall refused two PCs behind one router
  for a minute after a few quick hosts and joins.
- Debug, Relay only now holds when only one side ticks it: that side offers
  no direct addresses either, so the link goes through the TURN relay (before,
  the other side reached it directly and the link came up peer to peer).
- The input delay in the panel and the status line is the match's, as the
  fights play it: between rounds, in intros and win poses the games run in
  lockstep with a higher delay of their own, which is no longer shown there.
  The panel shows the delay alone (the prediction window is always 8).
- The rollback number on the other player's bar (RBF) is the longest
  rollback in the last second, not the longest of the whole session: one
  long rollback early on kept it high for the rest of the match.
- The panel no longer warns when your input delay is below the suggested
  one: rollbacks cost little enough now that a lower delay is a fair choice.
  The log still notes it at the start of each match.

### Fixed

- The stage select's frame could show a white box behind "Random" on one PC:
  its shadow texture was taken while still the engine's white stand-in. A
  view that small now counts as not loaded yet and is asked for again.
- A character's grid icon on the select screen could stay blank for the
  whole run (Gill's, once): a texture the render thread had not set up yet
  counted as missing. It is now asked for again a few times.
- The game's own Results Menu opened over the post-match vote and the lobby
  (since 0.1.1, about 30 seconds after the KO): the battle ran on past the
  results screen, which opens the menu by itself. The battle now holds still
  once its results screen is up, until both players have chosen; the win
  pose before it still plays out. The vote also keeps the pads from the game
  while the panel is open over it (the press that closed the panel reached
  the results screen).

## [0.1.5] - 2026-10-30

Matches stay in step far more often: random move outcomes, Rose, R. Mika
against Akira and screen shakes now play the same on both PCs after
rollbacks, and visual effects no longer cause false desync warnings.
Rollbacks take less time, keyboard players can skip intros, and several
crashes are fixed. Both players need this version to play each other.

### Added

- Spectators pick which match of the session they watch: Previous, Next and
  Live in the panel, and the list of the session's matches (the last 12)
  with who won. A picked match plays from its start; after it the next one
  follows (a pick made while switching matches waits for the switch). Lobby
  protocol 8.
- The panel warns when your input delay is below the one suggested for the
  connection (the game then rolls back most frames), and the log says so at
  the start of each match.

### Changed

- Rollbacks take less time: after loading an earlier frame the game no
  longer runs its three costliest follow-up steps (the fighters' animation
  re-sync and two more), about 90% of the time a rollback's load took. The
  frames played again after it set the same state. `[netplay]
  skip_load_callbacks=0` in sfv_rollback.ini turns it off.

### Fixed

- The game could crash a few seconds after starting (about one start in
  five on some PCs), when the mod's overlay set itself up at the same moment
  as the game's graphics. The overlay now waits for the game's window.
- Matches with random move outcomes desynced, or played differently after a
  rollback: R. Mika against Akira picked a different variant of a move on
  each PC for a few frames, and once the whole fight went a different way.
  Characters' random numbers went to whichever object drew first in a frame,
  and that order could change; a second, global random state the game uses
  for move variants was never set. Both now come from the match and the
  frame, the same on every PC and after any rollback. Players need the same
  version to play together.
- Keyboard players could not skip an intro or the win poses online: the
  mod kept the keyboard's Start (Enter) from the game so it could not pause
  one side, and the skip never saw it. Enter now counts as Start for the
  skip (both players still need to press it), and so does Start on a pad in
  any controller slot.
- A screen shake after a big hit could shake the other way on one PC after
  a rollback (a short desync warning, seen in a Rose mirror match): the
  counter that flips the shake's direction was not restored by rollbacks.
  It is now. The engine's own random numbers and other objects' random
  draws in a frame are also set from the match and the frame now.
- Rose's matches desynced every time after a rollback (a short warning
  around her soul level change): a countdown that applies her new soul
  level was not restored by rollbacks, so the level changed a frame early
  on the PC that rolled back. It is restored now. Players need the same
  version to play together.
- A match could start before both games had loaded both characters: a
  game reported its characters loaded while it only knew its own pick, so
  the other character loaded as the battle began, a few frames off on each
  PC (spectators then desynced from the first frame). The match now waits
  until both games have both characters (up to 90 seconds).
- False desync warnings with visual effects such as R. Mika's motion
  trail: after a long rollback the game could put the same effect into
  another of its effect slots, and the leftovers in the slots it did not
  use were reported as a desync, often for the rest of the match. The
  effect looked the same on both screens. Pooled effect objects and the
  script's effect records are still restored on rollbacks but no longer
  compared. Players need the same version to play together.
- The game could crash when you left a session at the moment the other
  player left it too (both games closing the match at once).
- The game could crash, rarely, while a match was being set up: the mod
  read the game's object list as the game was growing it.
- A desync report is saved in the background: writing it during the match
  froze the game for a tenth of a second or more.
- Non-Latin systems and games: the game's own text (character, costume,
  stage, V-Skill and V-Trigger names) came out as question marks when the game
  runs in Chinese, Japanese or Korean; the panel now shows it, in the fonts of
  Windows' display language first (Chinese systems no longer get Japanese
  glyph shapes). Recordings and match logs are saved when the game folder's
  path has characters outside ASCII (they were not), and the window title
  shows names in any script.
- The panel opened during the title screen read the game's character lists
  before they were complete, and kept them: characters showed as codes
  ("Z34"), costumes as "costume 0" and V-Skills without names (seen by a
  spectator, also in the post-match windows; a player's own lists could
  miss characters too). The lists are now read once the game is past its
  start.
- A spectator could open the game's own Results Menu over a watched
  match's results screen (any press, even the one opening the panel): the
  game now gets no input there, as the players' vote already did for them.

## [0.1.4] - 2026-10-29

Keyboard and custom button layouts now work online, names can use any
script, each character remembers its loadout, and offline modes no longer
take the last online session's stage.

### Changed

- The character list is in alphabetical order.
- Each character keeps its own loadout (costume, color, variant, V-Skill,
  V-Trigger): picking a character brings back what you last played with it,
  also after restarting the game. A character not played before starts from
  its first costume and color, V-Skill I and V-Trigger I.

### Fixed

- Offline modes after a session: until the game was restarted, every stage
  the game asked for (Arcade, Versus, Training, even the menus') was swapped
  for the session's last stage, and after the Debug tab's Battle now offline
  battles also got its Ryu and Ken. The mod's stage and characters now apply
  only to the battle it jumps into.
- The debugging hotkeys F5, F6, F9 and F10 are off: they loaded saved
  states in any battle, offline ones too. `[probe] debug_hotkeys=1` turns
  them on; the old `savestate_hotkeys` setting is no longer read.
- Player names in any script: names are read from the ini files as UTF-8,
  sent whole up to 96 bytes (cut between characters, never inside one), and
  the panel draws them with Windows' fonts for other scripts (accented Latin,
  Greek, Cyrillic, Japanese, Korean, Chinese). Lobby protocol 7.
- Button Settings now apply online: netplay sent the keys before the game
  applies them, so custom layouts and shortcut buttons (3P, 3K, two-button
  shortcuts) came out wrong. Keyboard players, who map keys to pad buttons
  and set both, were hit hardest. Each side now sends its input the way the
  battle reads it offline.
- A spectator's lobby no longer shows "measuring..." under the players: it
  has no link of its own to measure.

## [0.1.3] - 2026-10-29

No more false desync warnings during intros and round calls, and players
whose network blocks the server's usual port can now connect through port 443.

### Fixed

- False desyncs in intros, round calls and win poses: the camera's and the
  pawns' animation clocks follow how ticks fall into rendered frames, which
  differs per machine (more so since 0.1.2 stretches frames). They are still
  restored on rollbacks but no longer part of the checksum. In two matches
  against a tester they caused 300-400 desync events each while the rest of
  the state matched byte for byte.
- A test script's `server` command no longer sticks in the panel's saved
  settings.

### Changed

- When the rooms server does not answer at all, the message says what
  usually causes it (a firewall, antivirus or a network blocking its port).
- The public server is tried on port 443 at the same time as 5555, and the
  first to answer is used: some networks let only common ports through. The
  server needs sfvr-server's `deploy/port-443.sh`.
- The panel's Debug tab has a "Public server" button next to "This PC", to
  get back to the public server after trying a local one.

## [0.1.2] - 2026-10-29

Smoother matches: the game no longer repeats a frame to keep both sides in
step, and rollbacks cost less CPU.

### Changed

- Time sync stretches frames instead of repeating them: the side that runs
  ahead makes its frames a few percent longer (a wait right before the game
  reads the pads, so no input lag) until the other catches up. Only a lead of
  8 frames or more still drops a whole frame. Before, time sync repeated
  about ten frames a minute; between two PCs it now repeats none.
- Cheaper rollback bookkeeping. The state checksum reads the masked words as
  zero in place and fetches the next memory ahead: about 0.35 ms down to
  0.1 ms a frame, with the same checksums. A save whose next frame's inputs
  are already confirmed (the delay covers the latency) keeps its checksum but
  skips the state copy (`[netplay] skip_saves`).

### Added

- Every load is checked against the checksum taken when that frame was saved
  (`[netplay] verify_loads`), and a `costs over 600 advances` log line says
  where the netplay time goes (ticks, replays, hashing, saves, loads). The
  pace log says how much was stretched; the frame drop lines show the wait.

## [0.1.1] - 2026-10-29

Fixes from the first test sessions: players with the same version always
connect now, the lobby no longer hangs after a split rematch vote, and the
win pose plays out while players choose what's next.

### Fixed

- The noise mask is built into the mod, so every copy of a version checks the
  game state the same way. Under 0.1.0 a copy without `sfv_noise.txt` could not
  join a copy with it ("the host runs another build of SFVR 0.1.0").
- When builds differ, the message says which part: the game's version, or
  SFVR's state checks.
- The lobby over a match's results screen no longer waits on character
  preloads that can only run at the main menu: changing a pick there showed
  both players "READY (loading)" and held the match back.
- The win pose keeps playing behind the post-match menu and the lobby, instead
  of freezing when the game's own results menu would open.

### Changed

- Ready locks a player's pick (and the host's stage) in until the match starts;
  Leave is the way out. Backing out while the host started the match left both
  sides waiting.
- A desync shows as one small line along the bottom of the screen instead of a
  banner across the top.
- `sfv_noise.txt` is no longer part of a release (a 0.1.0 copy left behind is
  ignored); `[mod] noise_file` uses a file instead of the built-in mask, for
  learning new words.

## [0.1.0] - 2026-10-29

The first test build: rollback netplay for Versus matches through match codes,
with spectators, rematches, match recordings and updates from the project site.

### Added

- Rollback netplay (GekkoNet) for Versus matches, built on the game's own
  rollback state: host a match and send the 6-character code, or join with
  one. The rooms server is the public sfvr-server (`159.69.32.141:5555`).
- Peer-to-peer links, through Cloudflare's TURN relay when two players can't
  link directly; the other player's bar shows a cloud when relayed.
- A lobby with both players' picks (character, costume, color, variant,
  V-Skill, V-Trigger), the host's stage, ready checks, and both characters
  preloaded before the jump into battle.
- After each match: rematch, character select or leave, with the session's
  score. Up to 8 spectators per session.
- An in-game panel styled after the game's menus (F1, or Back + Start): host,
  join with an on-screen keypad, settings, and player bars over the health
  bars with ping, the other player's frame rate and the longest rollback.
- Input delay suggested from the ping (1 to 3 frames) and set per player.
- An offline boot straight to the main menu, without the game's server login.
- Desync detection on every frame, with a banner when it happens.
- Match recordings (`.sfvr`, compact: about 270 KB a match) and a log for
  every match, in the `SFVR` folder next to the game; **Pack logs to share**
  zips them to send with a report.
- Updates from the project site (https://sfvr.pages.dev), offered in the
  game and installed only when the player agrees; both players need the same
  version to play each other.

### Known issues

- Vsync can't be turned off from the game yet.
- Round intros, round calls and KOs run in lockstep (no rollback), so they
  can stretch a little on long-distance connections.

