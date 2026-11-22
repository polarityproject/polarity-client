# The overlay

The mod's screens, drawn with Dear ImGui into the game's own frames: the
hub, the rooms, a session's select screen, the HUD during a match, the vote
after it, Polarity settings, the Battle Log and the benchmark. The code is
`src/mod/overlay*.cpp` (shared state in `overlay_internal.h`); the screens
themselves are in their own files (`hub_screen.cpp`, `select_screen.cpp`,
`room_screen.cpp` and the rest), which the overlay feeds and draws.
[local-input.md](local-input.md) covers the input it shares with the game.

## Drawing into the game's frames

The overlay draws on top of the finished frame in the swap chain's Present,
leaving the game's render targets as they were. It starts on the first
Present after the game's window has been up for two seconds: finding
Present takes a device and swap chain of its own, and made while the game
was creating its device that crashed the game in ntdll now and then.

From the first Present on, the game's swap chain's table entry for Present
draws the overlay, and the code hook only passes frames on. Capture tools
(OBS's game capture, Discord's Go Live) hook dxgi's Present in its code, and
the hook made last runs first: hooked after the mod, they took the frame
before the overlay was drawn into it. The game calls Present through the
table, so drawing there comes before every such hook. When the back buffer
goes away (ResizeBuffers) the overlay's view of it goes first.

The overlay's frame runs every Present, also with nothing to show: it
consumes the input events queued for it. Present and ResizeBuffers run on
the render thread; window messages reach it from the window thread through a
queue. Work that opens sockets or waits for threads (hosting, joining,
leaving) runs off the render thread.

The frame rate counts the frames presented over each half second: averaging
1 / frame time reads high whenever frame times are uneven.

## Input

The overlay takes the pads as the XInput hook composed them, the game's own
pad keys (so a pad the game reads past XInput, through DirectInput or Steam
Input, works too) and the keyboard keys the game assigns in Input2.ini
`[AssignKeyboard]`, read again every 2 s since the game's key settings write
it. A key the game assigns is its pad button, unless a text field took the
keyboard last frame. The triggers count past XInput's threshold.

Back + Start (or F1) opens the hub at the game's menus and the leave modal
in a match; once per press, not again until both are let go nor within
300 ms. The screen it opened does not see its Start until both are let go.
Watching, a tap of Start alone (within 450 ms) opens the watch panel, a tap
of Back alone shows the room chat, and Back + Y starts typing into it.

While the screens take the input, the game gets none of it. After they let
go, the game still gets nothing until every button and key is up: the press
that closed them was still held, and the game took it as a new press (a held
A picked the main menu's Arcade). A key held longer than 3 s (stuck, or a pad
unplugged while held) lets the game have the input anyway.

Mouse positions come from WM_MOUSEMOVE only. ImGui's backend asks for
WM_MOUSELEAVE with TrackMouseEvent, which from the render thread reports a
leave at once, so the real cursor decides whether the overlay's cursor
shows: only where the game hides the system's, and not with the real cursor
off the game's window. The game captures the mouse at start and lets it go
when it sees the mouse move; with the overlay up it sees none, so the
overlay releases the capture, or a click on the title bar reached the game.
The backend does not set the system cursor: the render thread does not own
the game's window.

## The look

The screens follow the game's menus: near-black bars, light warm gray tiles
with dark gray text and a cream outline, the focused one dark olive with
gold text over a gold rule, square corners and thin outlines. The fonts are
embedded so they look the same on Windows and Proton: Barlow for text, its
bold italic for titles and tiles, Archivo Black for the select screen's
names. Each takes the characters it lacks from the bundled Noto Sans CJK,
whose face follows Windows' display language, so the Han characters Chinese,
Japanese and Korean share take the player's own shapes. Fonts are built for
the screen's size (1.0 at 720 lines) and again when it changes; ImGui makes
glyphs as text needs them, so names in any script draw without a rebuild.

The screens share one toolkit (`ui_kit.cpp`). The game's textures are read
from the paks on the mod's own threads; one the paks cannot give loads on
the game thread, a few milliseconds' worth a frame: a screen's first frames
ask for about 80, and all of them in one frame held the game about 5 s after
a join. A texture drawn on screen goes ahead of those asked for in advance.
Each fades in as it arrives, and a missing one leaves its place dark. A
texture whose view is not made yet is asked for again a few times; a view
8 pixels or smaller is the engine's white stand-in and is not kept. Images
are sized in steps of 32 px, so a window resize asks for few new sizes, and
a new size swaps in once its fade-in is over.

Directions repeat while held; A and B count once per press, since a long
frame (a character loading) would turn a held A into a repeat. Every
direction plays the menus' up and down cursor sound. A click on a prompt's
button glyph acts as that button on the next frame. The mouse wheel carries
a touchpad's fractions of a notch over to the next frame.

Behind the hub's screens is the Capcom Pro Tour arena's stage picture
(S21's `StageSelectBG`, 2048 x 512), darkened, with a light grain over it.
Panes show a blurred copy of the stage behind them under their tint, and a
soft shadow; a pane over a fight has no solid core under it, since the fight
shows through.

Latency is colored by the delay the link suggests; a relayed link shows a
cloud and its ping in yellow. The other player's frame rate is averaged over
3 s, since it moves around 60 as rollback catches up.

## The game's tables

The character and stage lists (each costume's name and color ids, V-Skill
and V-Trigger names, ownership) are read once on a worker thread: the names
walk every object, which on the render thread froze the game. They are read
only once the game is past boot and the title, since a read then came out
short and stayed, and after every stage's name has loaded. The V-Comment
names follow on their own (each loads the character's subtitle asset, about
0.4 s a character), so nothing waits for them; until then the V-Comment row
says "Comment 2". Characters are listed alphabetically.

Ownership follows the profile's entitlements. The lists stay whole, since
the other player's pick shows from them; only this player's choices skip
what they do not own, and a saved pick they no longer own falls back to the
first owned character, costume and color.

## The player's pick

A session always plays a pick, since there is no character select to fall
back on: the saved one, else the first character. Picking a character again
brings back its loadout as last saved or left. In a session the pick, the
host's stage and the input delay go to the session as they change, once the
lists are read, and are saved for the next session. A variant goes only with
a costume known to have it. The other player's color on the same character
and costume is not offered here: after a change, this player's color moves
to the next free one, and stepping through colors skips it.

Eleven plays a fighter picked at random (another than last time) in its
silver costume, with its V-Skill and V-Trigger at random too. This player's
game picks the fighter, so the pick others see is an ordinary one; screens
show it as Eleven, and the rolled fighter stays hidden until the match
loads. Ready on Eleven rolls a new fighter each time.

## Sessions

A session's lobby is the select screen. It fades in (over the last match's
results screen after character select), and stays into the match until the
jump switches to the battle scene, then fades to black with the game's own
loading screen. Still drawn while the jump loads, it keeps the input from
the game: the main menu behind took presses then. A table's set starts
without a select screen (both picks are each player's My character); its
room screen stays up until the jump.

On the way to a session the select screen's textures start loading (about
170, 40 ms each on the game thread), and stay loaded for the run, so nothing
pops in when it opens.

Reaching a session's lobby sets the input delay to the player's setting, or
with Auto to the suggestion for the ping between them, once a session. A
session a command started keeps the delay its commands set, and a table's
set the one chosen on its your-turn card.

During a match, a bar over each health bar shows the player, their set
(matches won of those played), their connection and, on the other player's
bar, the ping, their frame rate and the longest rollback in the last
second. A long name is shortened so the rest stays in the bar. The bars fade
out over 800 ms once the match is over.

After a match, a pane at each side's bottom shows the score, each player's
choice of what follows and this player's input delay for the next match;
presses in its first 750 ms do nothing, so buttons held from the match
cannot choose. The game's own results menu is kept from opening meanwhile,
and the panes take the pads, also while another screen covers them.

Toasts come down from the top right out of a fight, with the game's notice
sound; in a fight they are small notes at the bottom centre. A desync note
stays until the match is over. Update toasts show once per version and only
at the menus.

The leave modal (`fight_ui.cpp`) opens on Stay. The post-match pane grows up
from the bottom: the other player's flag and name, the score (the set's, or
this session's wins), Rematch, Character select and Leave, then the delay
row, which left and right change while it has the focus. Its footer shows
the vote's timer or the other player's delay. Notices in a fight stack up
from 92 design units above the bottom, the newest lowest; out of a fight the
newest slides down into the top slot and pushes the others. Over the game's
own menus, which are bright and busy, a notice gets a darker backing under
its glass so the text behind doesn't show through.

## The hub and the rooms

The hub shows over the game's menus after the title and when Start is held
there; it goes when a session starts or a battle loads. Its screens (the
room browser, a room, settings, My character) fade into each other over the
stage behind, which stays.

The hub's connection to the rooms server uses the player's name as it is
now: a new name in settings connects again at once, or once the player is
out of a room. A lost server is tried again every 10 s while the hub is up.
The browser with its server gone goes back to the hub, which says why. Only
the latest client plays in Custom Rooms: an update out takes the browser to
the hub's update modal, and opening the rooms first checks for one unless a
check ended in the last 5 minutes.

At launch the hub stays black until the backdrop's art is in (1.5 s at
most), then fades in. The fade counts drawn frames, each at most 1/30 s,
since the game's first seconds at the menu hitch (frames of 60 to 180 ms)
and ate a fade timed by the clock. A Custom Rooms tile that can't be used
opens a modal saying why instead of a dead screen. While the hub is still
connecting the modal waits, and Custom Rooms opens by itself once the server
answers. The update modal shows the release's first paragraph of notes, the
download's progress, then Restart now or why it failed.

On the rooms screen, a room code uses 32 characters: A to Z and 2 to 9,
without I, O, 0 and 1. In streamer mode a typed code shows as dots, and a
friend's code stays hidden too. Typing on a keyboard goes straight into the
code field, every key as itself, since a code has the letters of the game's
keys. A public room's row shows the owner's flag from the rooms server's
lookup, or the two letters in a plate when the game has no flag for the
country. The flag textures are 128 x 64 with the flag in the middle 96 x 64.
A character's face is a crop of its select-grid icon in a frame leaning at
the icon's own angle, so it is neither sheared nor stretched.

In Polarity settings the cursor skips rows that can't be used. A value too
long for its row (a folder's path) shows its end, and typed text sits where
the value does.

The mod's menu sounds are the game's own: `PlayGameSE` (rva 0x80DD30 maps a
`GameSoundSECue` to the system cue sheet's id, rva 0x777F40 plays it on the
system SE player), through the CriWare plugin's play request, which takes
requests from any thread. They follow the game's SE volume: the options
apply it to the CriWare category `M_SE` at the setting x 0.9 (rva
0xA77BCF), and the game's own apply sets every category from
`KWOptionSettingsSave` (game instance, rva 0x521690, then the system save,
rva 0x68E0D0, then +0x60). A muted run's sounds are stopped before the
volumes come back, with a moment for the stops to reach the sound thread
first, or a tail slips out. The mod's own menu sounds are never stopped as
part of a muted run. A press that moved and chose in one frame is heard as
the choice.

A table's ticket starts its set with the player's My character, ready at
once, and the delay chosen on the your-turn card (the setting, or with Auto
the suggestion for the ping to the opponent at the table). Back at the menus
after the set, the room screen comes back, also when the room is gone. A
table watch plays the set from its first game, with the room chat under the
watch panel. Training while waiting runs the game's own Training until the
table calls; then the main menu, where the room screen comes back with the
your-turn card. A watched match's jump, load and play keep the input from
the room screen until the battle takes it.

The watch panel (`watch_panel.cpp`) is a modal over the dimmed match: the
table and the game in its header, the two players (names shrink down to 18
design units when two long ones would run past the pane, then are cut),
and Previous game, Current game and Next game, the focused one gold. It
opens on the current game. LB, X and RB go straight to a button. Previous
and Next show only where there is a game that way; the panel shows no
results and no list of games, so nothing about the set is spoiled. The
room chat while watching (`watch_chat.cpp`) sits between the V-gauges at
the bottom, clear of the round timer and the health bars: a tap of Back or
Tab shows and hides it, and Back + Y or T starts typing (T is a letter the
game doesn't take as a button), with every key itself while typing. It
shows the last lines that fit, newest at the bottom, without muted members'
lines: each name in bold, its message indented by the name on the first
line and wrapping under it. With nothing said yet there is no box.

My character is the select screen for one player, with the current loadout
card where the other player would be. Confirm saves the pick (the menus'
preload loads it, the room sees it); B on the grid keeps the pick that was
in effect. Nothing saved yet, it starts from the Fighter Profile's favorite
with its loadout and the profile's stage. Its Random stages list is kept
apart from the pick and saved once the player has one. Its loading bar runs
on the time a character takes, since the asset queue gives no progress.

## The room screen

A room's screen (`room_*.cpp`) shows its tables on the left, four in
view, and the players and chat on the right. The table that has the focus
has a gold strip. Each table shows its rules and how many wait in its queue,
not counting watchers. The seats show each player's character facing the
score, opaque over the face and fading out only in the art's last quarter.
On the focused open table, the seat A would take reads "Sit here as P1" (or
P2).

The player list has eight rows: this player first, then the others, oldest
first. Each row's character art is moved so the face lands clear of the
name, by a face point marked by hand for each character. A row shows the
player's activity, their ping colored by the delay it suggests (a cloud when
it goes through a relay), and their connection. Muting a player hides their
chat for this player only.

The chat's newest line is at the bottom. Scrolled up, new lines do not move
the view, and a chip says how many wait below. Someone else's line plays the
chat sound, unless they are muted, and the joined and left lines play the
room's sounds. Lines already there when the room opened play none. With the
word filter on, chat shows the masked names and starred text. Enter starts
typing when the game's keys leave it free (by default it is Start). On a
pad, Back + Y starts typing; on a Steam Deck, Steam + X brings up the
keyboard. Back alone, released without Y, copies the room's code, as Ctrl+C
does.

The your-turn card comes up with each call of this player's table, and a
call of the same two again is a new card. Ready stands as the keeper has
it. The card has the V-Skill, V-Trigger and input delay for the match; the
opponent's choices show gold for a moment when they change. Its timer is
the keeper's call less 1.5 s for the trip to the keeper, counted from when
the call first showed here, even while this game was still coming back from
a battle. The last five seconds tick until this player is ready. A player
who runs out of time goes to the back of the queue.

From both players' Ready to the match loading, the connecting modal lists
the steps: both ready, the match set up, connected (with the ping),
characters loaded, starting the match. A step in progress for 3 s or more
shows its seconds. Cancel works until the match loads, and only before the
set's first game.

The owner sets the table rules (Start), which apply from each table's next
set; changed tables are sent one a frame. The creation form's rules go once
the keeper is in the room to take them. The owner can also remove a player
(X on the player). The leave dialog says what leaving does: this player's
place at a table is lost, and the longest present after them becomes the
owner, or the room closes if they are the last. While the room has no keeper
it is paused: the tables are on hold until the rooms server reconnects, and
chat still works. Training (LB) fills the wait while this player is seated
or queued with no set on.

## The select screen

The select screen draws the game's character select its own way, with the
game's textures (`select_*.cpp`). It has two pages. On the stage
select only the host moves the cursor. Left and right step through the
stages, then Random. Up, down and Y change a stage's setting (The Grid: The
Grid Alternative), shown in its place. The cursor steps over stages and
settings the profile does not own.

Once the stage is locked, the character grid follows the game's final
roster screen: from the middle outwards, 11 a side on the top row and 12 a
side on the bottom. The random tile in the middle spans both rows, so left
and right reach it from either. The tiles lean as the portraits do:
`CharaSelectIconLeft` and `CharaSelectIconRight` (116x144) each hold a
parallelogram 62 px wide over rows 2 to 142, leaning 12 px every 62 rows.
The picture fills the tile corner to corner inside a 2 px frame. A character
the profile does not own shows dimmed and cannot be picked, as in the
game's own select. Each side shows what that player sees: on the grid the
character under their cursor, past it their pick and loadout. A side whose
screen is not known yet is drawn from its pick.

Behind both pages is the stage's picture (`StageSelectBG`, 4:1, cropped at
the sides), blurred and darkened, with grain over it. A stage just moved to
shows once its blurred copy is made, a frame or two later; the last one
stays until then. The stage select's frame (`StagePreviewWindow`, 1920x534)
spans the screen. Its mask (`StagePreviewBG`, 1920x512, 11 px into the
frame) curves 2 px from the edge at the sides and 43 px at the middle, top
and bottom alike. The picture is drawn in slices that follow the curve.
While the stage is picked, the character select's textures load behind it,
in the order asked.

B goes back a step: from the loadout to the grid, from the grid to the stage
select (the host), and from ready to not ready until both are ready. With
nothing to go back to, B opens the leave dialog, on Stay. While the dialog is
up it takes the input, and the screen behind stays as it is.

My character's Random stages list opens with X on the random tile. It is a
grid of the stages Random can land on, never the never-random ones, each
with its settings as Random draws them. A stage with settings opens their
list, as the game's own random stage menu does. At least one stage stays on.
All off leaves the stage under the cursor on.

The Debug tab opens a preview of the screen with mock players. Watching, the
mock players act on a clock that starts over with each phase.

## The Battle Log

The Battle Log (`battle_log_screen.cpp`) lists the recordings in
`Polarity\replays`, read on a worker thread. A file whose size and time have
not changed keeps the header read before. The list shows a timeline, grouped
by day (TODAY, YESTERDAY, then the weekday), or the matches by set (LB and
RB switch). LT and RT open the filters: the characters on each side, and
the opponent, with a name search. A set's row says WON or LOST by this
player's side in its last game, with the set's score. The mouse wheel
scrolls the list a match a notch; a focused row scrolled out of view becomes
the nearest one in view. The details show round winners only for a decided
match: a match left early keeps its rounds' frames for playback, not who
won them. The replay viewer's stripe is `battle_log_stripe.cpp`
([replays.md](replays.md)).

## Launches

Watching a replay from the Battle Log and running the benchmark both keep
their screen up, taking no input, while the recording is read and its
characters and stage load and the jump switches to the battle scene. Then
the screen goes to black, held while the game's own screen goes black, and
closes. Gone at once, the main menu showed for the seconds of the preload.

## Screenshots

`shot` reads the back buffer as it is about to be presented and writes a
PNG aside, renamed into place so a reader never sees half a file.
`shotburst` keeps every presented frame in memory for a while and writes
them after, since a shot a frame stalls the frame and the HUD's movies run
on real time.
