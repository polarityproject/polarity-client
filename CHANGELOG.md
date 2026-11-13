# Changelog

Releases of Polarity (SFVR until 0.3.1), the Street Fighter V rollback netplay mod. Each version's
first paragraph is the summary players see in the game (Settings, Updates);
`scripts/release.ps1` reads it, and refuses to publish a version that has no
entry here. Newest first; the format follows [Keep a Changelog](https://keepachangelog.com).

## [0.4.2] - 2026-11-12

The benchmark now checks your game's battle data and names mods that
change characters' moves (those fall out of sync against players without
them). Logs and desync reports list your mod paks. Frame meter fixes for
R. Mika, held moves and idle poses, and the Battle Log keeps its HUD after
LB from a KO or the results. 0.4.2 plays only 0.4.2.

### Added

- The benchmark's result says whether this game's battle data is the
  game's own. Mods that change a character's moves or stage transitions
  are named with the characters they change, since matches with those
  characters fall out of sync against players without the same mods. Mods
  that only change looks and sounds are fine. When the round itself played
  with changed data, the result says which characters'.
- Polarity's log and desync reports list each mod pak and whether it changes
  battle data, and each match checks its characters' loaded data against
  the game's own.
- A failed benchmark keeps what's needed to find out why without the log:
  whether the round itself or only a rollback's replay of it came out
  different, how many frames differed, what this game session had played
  before (matches, replays, Training, desyncs), and a snapshot of the first
  frame that differed, saved in Polarity\logs (Pack logs includes it).

### Fixed

- The frame meter never showed in a match with R. Mika: her partner
  Nadeshiko counts as a third fighter, and the meter only started with two.
- Startup on moves that can be held (Urien's standing HP) was counted from
  where the held version hands over to the normal one, not from the
  button press: a tapped Urien st.HP read 5 frames instead of 12.
- Akuma's and Birdie's idle animations counted as moves on the frame meter
  (startup, then active frames on Akuma's long idle pose), and against them
  the meter often didn't start at all.
- In a Battle Log replay, LB after a round's KO (before the fade) or from
  the results at the end went back without the timer and health bars, and
  from the results left them behind. It now goes on to the next fight
  first, or starts the battle again from the end.

## [0.4.1] - 2026-11-12

Fixes a crash when a set's next game started after a stage transition KO,
and a desync from the first frame of some matches. The mouse wheel now
scrolls Polarity's lists, the frame meter shows proximity guard and armor,
and the benchmark shows its loading and runs on a Steam Deck. 0.4.1 plays
only 0.4.1.

### Added

- The mouse wheel scrolls Polarity's lists: settings, the Battle Log and
  its filters, the rooms list, a room's tables and its players. A focused
  row scrolled out of view moves to the nearest one in view.
- In a room, RB "Players" is in the prompts, and clicking a player picks
  them in the list (Mute, and Remove for the room's owner).
- Running the benchmark shows its loading: the round read, the characters
  and stage loading, the round starting, with the time each step takes.
  Settings stays behind it until the battle loads, then it fades through
  black into the round once the round runs.

### Changed

- The frame meter shows proximity guard in brown, apart from the yellow of
  hit and block stun, and armor in orchid (Zangief's Iron Muscle, EX
  moves with armor), in a move or while free to act.

### Fixed

- The game could crash as a table's next game started. It took a stage
  transition KO (the KO that carries the fight into another part of the
  stage) in a round before the last, followed by a rollback from the next
  round back into that KO; the crash came when the battle was put away for
  the next game.
- A match could part between the two players from its very first frame
  (a desync at frame 0) when a rollback went back to the start of the
  match. It showed most in a set's later games.
- The benchmark never started on a Steam Deck (Proton): its built-in round
  couldn't be read there. Replays recorded on Windows now play on a Steam
  Deck too.
- After a benchmark that could not start, its "Please wait" stripe stayed
  under the settings screen.

## [0.4.0] - 2026-11-11

SFVR is now Polarity, and this update moves your settings, replays and
logs over by itself. New: the Battle Log of your online matches with
hitboxes, a frame meter and Replay Takeover, a Benchmark, Training while
you wait at a table, room chat while watching, table rules when creating a
room and My Character on the hub. Fixes for the desyncs and crashes
reported from 0.3.x. 0.4.0 plays only 0.4.0.

### Added

- Polarity's screens make the game's own menu sounds: moving the cursor,
  changing a value, choosing, going back, windows opening, notices, chat
  lines, players coming and going in a room, Ready, and your turn at a
  table with its last seconds counting down. They follow the game's SE
  volume (Options, Sound) as its own menus do.
- **Battle Log**, on the hub under Play Offline: every online match you
  play is kept in Polarity\replays, newest first, with both players' names,
  characters and round wins. LB/RB shows them grouped by set (a table's
  set, or a Direct Match session until character select), LT filters by
  your character and your opponent's, RT by opponent. A watches a match,
  starting at round 1's call (the intro is skipped):
  pause and play (A), frame by frame both ways (left and right), the next
  or previous round (RB, LB), fast forward (hold RT), leave (B). Y opens
  the folder: copy replays out, or drop others' in. A replay plays on later
  Polarity versions too, until one changes how battles play out; a replay that
  goes out of sync with its recording says so.
- **Benchmark**, in Polarity settings: one recorded round played with an
  8-frame rollback on every frame, the most a match can have, measured from
  its FIGHT on. 59 fps or more on average passes; it also checks the round
  played exactly as recorded, as a PC that plays it differently would
  desync online. The last result shows under it.
- **Replay Takeover** in the Battle Log: during a round's fight, X takes
  control of Player 1 and Y of Player 2. After a count of 3 you play that
  side from player 1's pad against the recorded other side. When the round
  ends, its KO screen plays and the replay fades back to where you took
  over, paused. Start stops a takeover early.
- **Hitboxes**: hurtboxes (green outline), throwable areas (yellow),
  hitboxes (red), throwboxes (blue) and proximity block areas (gray), from
  the game's own box lists. On by default in Training (Polarity settings,
  Training, Hitboxes); View toggles them in Battle Log replays. Never
  online.
- **Frame meter**: 90 frames per player above the V-Trigger gauges, showing
  startup, active, recovery, stun, invincibility, parry and counter-hit
  frames, with startup, total and frame advantage. On by default in
  Training (Polarity settings, Training, Frame meter); Start toggles it in
  Battle Log replays, where it follows frame steps both ways. Never online.
- **My Character** on the hub, beside Custom Rooms: your room loadout's
  character over its stage, with its V-Skill and V-Trigger ("Seth VS1 /
  VT2"). A opens the same My Character screen as in a room.
- **Table rules when creating a room**: the Table rules row in Create a
  room opens the same editor the owner has in the room (A, Customize
  Tables), so a room can open with its own first to, rounds, timer and
  rotation on each table.
- **Training while you wait**: seated at a table or in its queue, LB on the
  room screen goes to Training (the game's own character and stage
  select). When your table calls you, you're taken back to the room screen
  with the your-turn card up. The game's "Go to Main Menu" in Training also
  leads back to the room.
- **Room chat while watching**: watching a table's set, the room chat shows
  in a see-through box at the bottom of the screen, between the gauges. T
  (or Back + Y on a pad) types a line, Enter sends it; Tab or a tap of
  Back hides the box completely, and shows it again.

### Changed

- **The name**: SFVR is now Polarity, in the game and on the site
  (polaritymod.link). On its first start, Polarity moves an SFVR install's
  files to its names: `sfv_rollback.ini` and the panel's ini become
  `polarity.ini` and `polarity.panel.ini`, the `SFVR` folder (logs,
  matches, replays, crashes, fonts) becomes `Polarity`, and the old DLL is
  removed. A file that can't move (open, or Polarity already has one of
  the same name) stays in the `SFVR` folder, with a note saying what
  stayed. A notice says so once. Replays keep their `.sfvr` files and play
  as before. An SFVR build started afterwards starts from fresh settings.
- The intro, round call, KO and win poses no longer run in lockstep online:
  they roll back like the fight, so they play at full speed at any ping
  instead of slowing to the connection's round trip. (`lockstep_round_stages=1`
  under `[netplay]` in polarity.ini brings the old behaviour back; the
  old `lockstep_presentation` line is no longer read.)

### Fixed

- Throw and Critical Art cameras (Zangief's SPDs, Abigail's Critical Art)
  no longer cut to the fight camera when a rollback lands during them.
- Desyncs reported from 0.3.x matches:
  - A fighter's list of active effects (seen most with M. Bison) could end
    up different on each side after a rollback, or from the first frame.
    These were the most common reports. The list now rolls back with the
    fight.
  - A hit's stun could count 10 % apart on the two PCs: whether the
    defender's stun reduction applied depended on which of the game's
    threads got there first. Online matches now run the game's battle
    tasks one after another in a fixed order, the same on both PCs, at the
    same speed. Because of this, replays recorded before this version can't
    be played in the Battle Log; it says so.
- The game could crash a few seconds after starting, when Steam's online
  service was not ready yet.
- The game could crash when a session ended during the jump into a match,
  for example when a spectator pressed X (light punch) on the room screen
  as the match loaded: it now goes back to the menu. X on the room screen
  no longer stops watching while the watched match is on.

## [0.3.1] - 2026-11-08

Choose the stages Random can land on, in My character. Sounds no longer
play twice after rollbacks, rollbacks cost about a quarter less, and stage
transitions play in rematches and a table's later games. Ring of Arcade is
back in the stage select. 0.3.1 plays only 0.3.1.

### Added

- Random stages: in My character's Stage Select, X on Random opens the list
  of stages Random can land on. Turn stages on or off; a stage with
  settings (such as The Grid and The Grid Alternative) opens a list of its
  settings, as the game's own menu does. Your list decides Random when you
  host, and when you're P1 at a table. It starts from your Fighter
  Profile's random stage setting.

### Changed

- Pings are colored by the input delay they call for: blue for 0 frames,
  green for 1, yellow for 2, orange for 3, and red where even 3 frames leave
  more than 2 frames to rollback. The your-turn card now shows the ping to
  your opponent under the VS, and the select screen's ping is larger.
- Names with emoji (often from Steam) show without them instead of with "?"
  marks; names written in script or bold letters show in plain ones.
- The room's player list gives names more room before cutting them short
  (shorter activity labels, such as "T2 · playing").
- Rollbacks cost about a quarter less time: frames a rollback simulates
  again leave out effects and camera work the newest frame makes up for, a
  frame is hashed once its inputs are confirmed instead of at every
  simulation, and each rollback's restore is quicker. Long rollbacks at high
  ping and low input delay drop fewer frames.
- Random stage no longer lands on Flamenco Tavern, Skies of Honor or
  Kanzuki Beach. They can still be picked by name.

### Fixed

- Ring of Arcade was missing from the stage select and from Random. It's
  there now.
- Joining a room no longer shows "SFVR left" in its chat (the room's own
  keeper, seen for a moment as a player).
- With no character saved in My character, a table's first game loaded the
  first character of the list (Abigail) instead of the Fighter Profile's
  favorite the room shows.
- A rematch into a new set started its counts over only partly: the player
  bars' "(wins/played)", the results screen's Win Ratio and the win streak
  now start over with each set, for spectators too.
- The delay box between a table's games could show a delay from an earlier
  opponent (2 while 0 was in use), and Auto could put that earlier
  suggestion back between games of a set; it now shows and keeps the delay
  chosen on the your-turn card.
- "muted" in the room's player list no longer runs into the player's
  activity.
- Sounds played twice after rollbacks (hits, whiffs, voices heard doubled a
  frame or two apart), most at low input delay. The game's own rollback
  handling for sounds, made for Capcom's netcode, now works: a sound already
  played is not played again, and one from a prediction that turned out
  wrong is stopped.
- After a rollback, the game's own clean-up of what began in the undone
  frames got a wrong frame number (as the sounds did), so it never ran; it
  now gets the game's own.
- A rollback across a KO could end the round a few frames later on one side
  than on the other: the round's end waits on a mode the game keeps outside
  the rollback's state, which a load left as the later frames had set it.
  Loads now put it back with the rest.
- Typing in a settings field (your name) showed the text off to the right
  with a dark copy of it where it should be; the text now stays in place
  as you type, and a long name shows its end instead of running over the
  field's label.
- Stage transitions (a KO that knocks the fighter into another part of the
  stage) never played in rematches or in a table's later games: a battle
  started from the last one's results lost the stage's name the fighters'
  stage data is looked up by. A spectator who joined after the first game
  still played them, so its round ended on another frame and the rest of
  the match went wrong on its screen; recordings of later games replayed
  wrong too.

## [0.3.0.1] - 2026-11-07

Players who updated from 0.2.x reach the new server, and a new name takes
effect without restarting the game.

### Fixed

- A game updated from 0.2.x kept looking for SFVR's old server, so Custom
  Rooms couldn't connect, unless a server had been picked in SFVR settings
  since. The old address now means the new server wherever it was saved.
- A name changed in SFVR settings only showed in rooms after restarting the
  game. It now takes effect at once (in a room, as soon as you leave it).

## [0.3.0] - 2026-11-07

Custom rooms: up to 16 players on up to eight tables with their own rules,
in public rooms anyone can find or private ones by code, with chat, a queue
at each table and watching any table. SFVR has a new look, starting from a
hub at launch, and moved to a new server, which 0.3.0 finds by itself; it
plays only 0.3.0.

### Added

- **The SFVR hub** at launch, in place of the F1 panel: Play offline (the
  game as it ships; Back + Start or F1 in its menus comes back), Custom
  rooms, and SFVR settings (Start). When the rooms server can't be reached,
  Custom rooms says why instead of opening; it is tried again every 10
  seconds.
- **Updates in a few presses:** when a new SFVR is out, the hub offers it
  with its notes; Update now downloads it, then Restart now starts the game
  again on the new version. Players in a room get a toast instead and the
  offer when they leave it.
- **A new look** for every SFVR screen: blurred stage art behind glass
  panels, the game's own button glyphs (each one clickable), and larger text
  sized for the Steam Deck's screen.
- **Custom rooms.** Create one with a title from the game's own Battle
  Lounge comments (two of them name characters you pick), public or private,
  with 1 to 8 tables. Public rooms are listed with their title, owner,
  players, faces and open seats, and the focused room shows its tables and
  players before you join. Any room joins by its code (Copy code beside the
  room's title; Y pastes a copied one).
- **Tables:** sit or queue, ready when called (with your V-Skill,
  V-Trigger and input delay for the match, and your opponent's choices shown
  as they change), and play a set; the winner
  stays (as the table's rules say) and the queue rotates. Each table has its
  own rules: first to 1-20, best of 1, 3 or 5 rounds, a 99, 60 or 30 second
  timer; the owner changes them between sets. After a set: play on, or
  character select.
- **Watching a table** from the room: any set, from its first game even if
  you come late, through the server's relay (the players stream once).
- **Room chat** (200 characters, about one message a second), each player's
  ping and connection (wired or Wi-Fi), muting a player for yourself, and the
  owner removing a player; the room passes on when its owner leaves.
- **My character** from the room: character, costume and color, V-Skill and
  V-Trigger, input delay and preferred stage, kept for the next set.
- **Country flags** beside names in rooms and the room list, from the
  server's look-up of each player's connection (IP Geolocation by DB-IP).
  SFVR settings, Country flag: Hidden shows yours to no one.
- **Word filter** in public rooms: names on the server's word list show as
  "Player 1234" and its words in chat are starred. SFVR settings, Word
  filter: Off shows them to you as they are. Private rooms are never
  filtered.
- **Streamer mode** (SFVR settings): room codes show as dots on the rooms
  screens; Copy code still copies them.
- **SFVR settings** in one list: updates, name, frame rate, country flag,
  word filter, streamer mode, input delay, server, and reports, with
  third-party acknowledgements at the end.

### Changed

- **Custom Rooms need the latest SFVR:** with an update out, they open
  only once it is installed, and the game checks for one every 15 minutes
  and before opening them. Rooms are per version: the list shows only
  rooms on your version, and a room on another version joined by its code
  says so and lets you go.
- **The game never logs in to Capcom's servers** with SFVR installed: the
  title goes straight to the main menu, and the setting that allowed the
  login is gone.
- **The keyboard follows the game's own key settings** in SFVR's screens
  (Button Settings in the game's options), so the keys that confirm and go
  back match the game's menus.
- **SFVR's server moved** (rooms, Direct Match and desync and crash
  reports). A server chosen in settings before 0.3.0 that was the old public
  one becomes the new one by itself. 0.3.0 plays only 0.3.0.

- The suggested input delay now starts at 0 frames: nearby players play
  with no delay, as the game's own online did, and rollback covers the rest.
  It aims at rollbacks of 2 frames or fewer and goes up to 3 frames for far
  players. It goes by the ping and its jitter over the lobby's last 10
  seconds, not one moment's ping.
- Versions like 0.3.0-rooms.10 order after 0.3.0-rooms.9 (the parts after
  the dash compare as numbers where they are numbers): the rooms testers'
  builds move to 0.3.0 by themselves.
- Updates can install the bundled font into SFVR\fonts\ (.ttc files only),
  and skip files the game folder already has.
- The panel and the select screen use bundled fonts (Barlow, Archivo Black,
  and Noto Sans CJK for Japanese, Chinese and Korean names), so they look the
  same on Windows and the Steam Deck. Names in scripts those fonts lack
  (Arabic, Hebrew, Thai) show boxes for now.
- The select screen's pictures are read from the game's files on the mod's
  own threads, and the common ones load at the main menu: nothing waits on
  the game, and each picture fades in.
- Two players on the same character, costume and color: each sees
  themselves as picked and the other in the costume's next color
  (spectators and replays see what P1 sees). Before, P2 was recolored for
  both.
- Match rules travel with each match (rounds per game and the round timer;
  Direct Match keeps best of 3 and 99 seconds), ready for custom rooms'
  tables.

### Fixed

- The window's close button did nothing while an SFVR screen was up, and a
  second mouse cursor trailed the real one (and stayed behind when it left
  the window).
- A room created or joined right after leaving another could wait 20
  seconds and fail.
- Leaving a public room as its only player could leave it listed with you
  in it for a few seconds.
- A room showed the fighter Eleven had rolled before the match; it shows
  Eleven now, with V-Skill and V-Trigger left random.
- A malformed message from a rooms server could make the game set aside
  gigabytes of memory; messages are now held to a size cap (the same fix
  keeps the rooms server itself from being crashed this way).
- The marker over each fighter at the start of a round said "Player 1" over
  both fighters in sessions and replays. It says Player 1 and Player 2 again,
  as in local Versus.
- Confirming Leave with A or Enter (on the select screen's leave dialog, the
  vote after a match, or the panel) also pressed the button on the game's
  main menu once the session had ended: it opened Arcade ("You are not
  logged in to the server") or whatever the cursor was on. The game now gets
  no input until the button is released.
- After a match, the first A on the side panel (Rematch, Character select,
  Ready) did nothing when the panel had just appeared or the mouse had
  moved: a direction had to be pressed first. A works at once now.
- Desync warnings during the round call on a stage whose scenery shakes
  the screen: the shake follows the scenery's animation, so when a frame
  hitched it began a few frames apart on each PC. The fight was never
  affected; the screen shake is left out of the desync check now
  (adapter-12, so both players need this version).

## [0.2.6.1] - 2026-11-03

Fixes the game closing on the Steam Deck and other Linux installs at the
end of a session's first match, as it saved the match's recording. Such
crashes now send a report. Both players need 0.2.6.1.

### Fixed

- The game closed on the Steam Deck and other Linux (Proton) installs after
  the first match of a session, when leaving the results screen: saving the
  match's recording called Windows' compression, which Proton does not have,
  and Proton ends a game that calls a missing function. Recordings (and
  desync reports) are saved uncompressed there now.

### Changed

- Crash reports cover more crashes: a game that ends itself after an error
  it caught (with who ended it and the error just before), a call to a
  function Proton does not have (naming it), and a crash dump the game
  wrote itself goes with the report. Such crashes counted as a normal exit,
  so they sent nothing.

## [0.2.6] - 2026-11-03

Fixes crashes on the Steam Deck and other Linux installs a few seconds
after hosting or joining a session, while the select screen loaded. Both
players need 0.2.6.

### Fixed

- Crashes on the Steam Deck and other Linux (Proton) installs a few seconds
  after hosting or joining, while the select screen's pictures loaded: the
  mod looked for each picture's view among objects that, under Proton, can
  be something else, and called into one of those. It now takes only real
  views of the game's graphics device there (Windows is unchanged).

## [0.2.5] - 2026-11-03

Fighters always play their final balance version in sessions, and a game
that does not have it says so before the match. Fixes desync warnings after
trades and after hits on a dizzy fighter. The player bars show each
player's set, and the results screen's Win Ratio counts the session's
matches. Both players need 0.2.5.

### Added

- The player bars over the health bars show each player's set so far, as
  "(wins/played)": the matches they won of those played this session.

### Changed

- A desync in a match's first frames logs, on both sides, the battle camera
  and both fighters' state as they were at the start, to find the cause of a
  desync from the first frame (seen with Guile against Lucia).

### Fixed

- The results screen's Win Ratio counts the session's matches: it read
  100% on both sides.
- A long name wraps in the rematch panel and the session's player columns
  instead of running past them; in the player bars it is shortened.
- Desync warnings after trades (both fighters hit in the same frame): the
  game counted one side's hit twice at once on different threads and could
  lose one, so each PC ended up with its own count (adapter-11, so both
  players need this version).
- Desync warnings for the rest of a round after hits on a dizzy fighter: the
  game's stun totals for the fight stats can come out differently on each
  PC, though the fight itself stays the same. They are left out of the
  desync check now, as their other halves already were.
- Fighters always play their final balance version. Each game took the
  newest version it knew, and some games know only older ones (two testers
  played Season 4 versions of Guile, M. Bison, Urien and Balrog). The host
  now sets each fighter's version for both players; a game that lacks it ends
  the session and says which fighter, and the other player is told why.
  The match log lists the game's versions that are not the final ones.
  Both players need this version.
- Long notices (such as why a session ended) wrap instead of running past
  the edges of a small game window.

## [0.2.4] - 2026-11-03

Fixes desync warnings in Vega matches once his claw or mask is knocked off,
and with E. Honda and against Rose. Sessions no longer end a minute or two
into a match on networks that close quiet connections. Eleven rolls a new
fighter every rematch, and B leaves the select screen. Both players need
0.2.4.

### Changed

- Leaving from the select screen: B on the stage select (on the character
  grid for the joining player and spectators, and once both players are
  ready) asks "Leave the session?" with Leave and Stay, Stay chosen first.
  Start no longer opens the menu over the select screen, and the prompts at
  the top right show "Leave" where B leaves.
- Skipping an intro, round call or win pose with Start takes effect one
  frame sooner, on the same frame on both sides however each game's frames
  ran (the game read the press once per drawn frame, not per battle frame).
- A game that freezes logs where each of its threads stands, not only the
  game thread, so a crash report of a freeze shows what the game waited for.

### Fixed

- Sessions ended a minute or two into the first match for players whose
  network cuts quiet connections (seen with a player whose link went through
  the relay): the game sent nothing to the server during a match, the
  network closed the connection, and the server told the other player that
  they had left. The game now checks in with the server every 20 seconds,
  and a match goes on over its own link when the server reports the other
  player gone while that link still works.
- Eleven played the same fighter for the whole session. It now rolls a new
  fighter (and V-Skill and V-Trigger) for every rematch, and again each time
  you ready up on Eleven at the select screen.
- A button pressed while the match loaded could reach the main menu behind
  the select screen: its cursor starts on Arcade, so a confirm press showed
  "You are not logged in to the server" over the fight. The select screen now
  keeps the pads and keyboard until the battle replaces the menu.
- Captures started after the game, such as Discord's screen share of the game
  or an OBS Game Capture added later, showed the game without SFVR's
  overlay. The overlay is now drawn before any capture tool takes the frame.
- Reports made while report uploads were off (a crash, or a match's desync
  files) were still sent once uploads were on again. They now stay on the PC.
- A desync warning in E. Honda matches when a rollback left his effect drawn
  a frame differently on one side (seen against Urien and Chun-Li): the value
  it is drawn from is now rolled back with the rest of the fight.
- Desync warnings in Vega matches once his claw or mask was knocked off,
  until the match ended: it flies from where his hand or head is drawn, and
  after a rollback that could be a later frame's pose, so it could land a few
  frames apart on each side. It is only drawn (the fight never reads it), so
  it is now left out of the desync check, as effects are.
- A desync warning in matches against Rose after a rollback: a flag one
  fighter keeps from one frame to the next was not rolled back with the rest
  of the fight, so Rose's soul could mark the other fighter a frame
  differently on each side (seen with Urien against Rose).

## [0.2.3] - 2026-11-01

Fixes desyncs when a game was left on another round count (the "Ken
V-Trigger 2" desync) and false desync warnings with Oro's V-Trigger II.
Clipping with Medal (F8) no longer leaves the match, and a freeze at the start
of some matches is gone. Both players need 0.2.3.

### Added

- Each side tells the other at the handshake whether it sends desync
  reports: the event log says so once, and every match log has a
  "desync reports: ... from this side, ... from the other side" line. A
  report whose other half never reaches the server now shows whether that
  player turned reports off or their game could not send them. Protocol 11:
  both players need this version.
- `setrounds <n>`: the round count match setups write, to test a game that
  kept another one.

### Changed

- F7 (host) and F8 (join the match code on the clipboard) are off unless
  `[netplay] hotkeys=1`, and never act while a session runs. Medal clips on
  F8: with a match code still on the clipboard, each clip left the match.

### Fixed

- Clipping with Medal (F8) mid-match left the session: the other player saw
  them leave, and their game played the match on locally with their pad on
  both fighters. Hosting or joining while a match runs now also goes back to
  the main menu, as leaving does.
- A game could freeze for good a moment into a session (seen on a host as
  the match started): the menu's character preloading and the session's
  thread each waited for a lock the other held.
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
- False desync warnings with Oro's V-Trigger II: the game keeps its floating
  stones' motion outside its snapshot, so after a rollback the stones went on
  from where they were, a little apart on the two machines. Only where their
  effects are drawn depends on it; those words are out of the checksum (nine
  Balrog vs Oro matches on 0.2.2).

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

