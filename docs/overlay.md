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

A table's ticket starts its set with the player's My character, ready at
once, and the delay chosen on the your-turn card (the setting, or with Auto
the suggestion for the ping to the opponent at the table). Back at the menus
after the set, the room screen comes back, also when the room is gone. A
table watch plays the set from its first game, with the room chat under the
watch panel. Training while waiting runs the game's own Training until the
table calls; then the main menu, where the room screen comes back with the
your-turn card. A watched match's jump, load and play keep the input from
the room screen until the battle takes it.

My character is the select screen for one player, with the current loadout
card where the other player would be. Confirm saves the pick (the menus'
preload loads it, the room sees it); B on the grid keeps the pick that was
in effect. Nothing saved yet, it starts from the Fighter Profile's favorite
with its loadout and the profile's stage. Its Random stages list is kept
apart from the pick and saved once the player has one. Its loading bar runs
on the time a character takes, since the asset queue gives no progress.

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
