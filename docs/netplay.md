# Netplay sessions

How a session runs, from the lobby to the end, and why it does what it does.
The code is `src/mod/netplay_*.cpp` (the class is in `netplay_internal.h`).
[input-delay.md](input-delay.md) covers the input delay and
[desync-replays.md](desync-replays.md) the reports a desync leaves.

## Threads

A session runs on two threads. The network thread (the control channel)
reads and answers messages, sends heartbeats and moves the session through
its states. The game thread drives the battle frame by frame. The fields
both touch are in the driver's `shared_` struct, read and written under its
mutex; so are the pick fields of its options. Flags one thread sets for the
other are atomics. What the game thread must act on (a new match, back to
the lobby, the end) the network thread hands over as the next step, and the
game thread takes it once it has let go of the match.

## The server connection

The Weyvelength service (`weyve_service.cpp`) holds the connection to the
rooms server and the peer to peer links. Its thread polls the client about
once a millisecond with a high-resolution timer, since a plain `Sleep(1)`
waits a whole scheduler tick (up to 15.6 ms) unless something raised the
timer resolution. The game thread's sends and receives wait while this
thread holds the client's lock, so a long turn shows as a frame drop; a
turn of 8 ms or more is logged as a hitch.

A connect that gets no answer blocks for about 21 s, the system's TCP
timeout, which is longer than a session waits. With a second port
configured, the service tries it too when the first has not connected
within 1.5 s or failed sooner. Trying both at once doubled the connections,
and the public server's firewall refuses an address after 6 in 30 s (two
PCs behind one router, a few quick hosts and joins). The first client to
connect is kept and the other closes when its connect returns. Each connect
runs on its own thread, so a stop (the hub closing its connection) gives up
on it at once. When the service waited instead, a custom server that gave
no answer held whoever closed the service for 21 s: the render thread, from
the settings' Server row, and the game with it (the 0.4.2 freeze c311abed,
2026-11-13).

The service keeps polling after the server connection is gone, so the peer
to peer links stay up (`third_party/weyvelength-keep-links.patch`). Nothing
else goes to the server while a match runs, and some networks cut a TCP
connection idle for about 90 s: players relayed through TURN were dropped a
minute or two into every match. A heartbeat goes to the server every 20 s.

Before any room, the server never puts the country on this player. `nocc`
sent outside a room gets "not in a room" back from a rooms server without
the country patch, which is not an error.

## The lobby

Each lobby starts at stage select. A table set's first game goes straight to
the characters: it has no select screen. Character select within a set
opens stage select again, so the host may take another stage.

A player's ready holds their pick (and the host's stage). They can take it
back until both are ready. From then on the match is locked in: both games
load the characters and the host starts it, so backing out would leave both
sides waiting. A new pick needs ready again, except in a Direct Match a
command started (tests ready at once).

Each player's place on the select screen (grid, loadout, the row, the
character under the cursor) goes out at once when it changes, at most every
33 ms, so the other screens and the spectators follow it live.

Both games preload the two picks, but only once both players are ready:
loading every pick or costume a player tried out stalled both games. The
host starts the match once both seats are ready and both games report the
characters loaded, or after 90 s. A player's own pick loaded does not count
as loaded before the other's is known: a match started that way had both
games load the other character while the battle was created, its objects
came after 3 neutral ticks instead of 6 or more, and spectators desynced
from frame 0.

A joiner still on the post-match screen with its choice made is not ready
for the next match, whatever its heartbeat says: its last heartbeats before
it reaches the lobby carry the previous match's ready flag.

## Joining and the room

The host's room data holds its version and game build. A joiner checks them
before it says hello and tells the player which side should update. The P2
seat goes to the ticket's seat token, or in a Direct Match to the first
player to join; anyone else watches. A table set streams to the keeper
alone, whose relay serves the viewers, so neither player's upload grows with
them; anyone else asking to watch is sent to the room.

No Polarity build chats in a match room, so a member that does runs a
modified client, and the host bans it.

## What the other side sends

Every message that carries a pick, a setup or the host's stage rule is
checked against this game's tables before it is used (core/pick_check.h).
From the other player, a bad value ends the session with the reason on both
sides; from a spectator, the message is dropped. The check never waits on
the game: a value it cannot check yet (a character's V-Comments still
loading, a costume's variants still being asked of the paks) leaves the
message for its next copy, since every such message is sent again
(heartbeats, Welcome until the joiner has loaded, Loaded until the host has
started, the match list every second).

A game that lacks a fighter's final balance version ends the session before
the match, with a message to verify the game files: both games play each
fighter's final version, not whatever each takes as its latest.

## Heartbeats and silence

Heartbeats go out every 250 ms: the joiner's PlayerState, the host's
SessionState, a spectator's ack. Each carries a timestamp and echoes the
other side's last one, which gives the round trip. Nothing heard for 10 s
ends the session (or drops a spectator); a table set already won ends with
its result instead of "connection lost", since the host's Bye can miss.

The server drops a member whose connection to it broke. While the peer to
peer link holds, a match in progress goes on over it, and ends when the link
does. A connect that never answers is nearly always a firewall, antivirus or
a network that allows only common ports on this side, and the message says
so.

## The jump and the frozen start

The match starts from the menus, a running battle or its results, straight
into the battle scene with the setup and stage (flow.h). The battle's
objects, and so its state regions, exist only after some ticks of the new
battle, so both sides run neutral ticks until they do, then hold still. From
then on the game sees neutral pads and the session feeds it.

Each side builds frame 0 itself, and a match starts only from equal frozen
states: no other machine's bytes are written into this game. States that
differ end the session 2 s later, while each side's Loaded still goes out so
the other side and the spectators find it too. Each side saves a desync
report of its start, and the server pairs them by match id.

The two sides' loaded setups are compared as the real picks: with both
players on one look, each side shows the other recolored (core/look.h).
The stage id is not compared: the battle data's stage index is not always
written by the time the battle runs.

### Leaving during the jump

A session that ends or goes back to the lobby during its jump leaves the
battle the jump asked for to come up with the session's setup. Cleared, the
battle was built from a bare jump's defaults (P2 a CPU, not Versus) and its
HUD read an object such a battle lacks (rva 0x2FF130 reading 0x13958B0),
which crashed the game. Once that battle runs, the game goes back to the
main menu (up to 30 s, unless another jump or driver took over). Going back
to the lobby calls the jump off first; an ended session also goes back to
the menu when no battle came.

## The match

Time sync: the side that runs ahead stretches its frame period a little
(pacing.h) rather than drop frames, which showed as a repeated frame about
ten times a minute. Only a large lead (after a hitch on the other side)
drops a whole frame, one per 10 at most, since the estimate is a 0.4 s
average and harder corrections made the peers swing. Only the side ahead
corrects: letting the side behind run two frames in one made both sides
swing and the session ran at 54 frames a second. In lockstep the side ahead
stalls for the other's inputs by itself.

### Outside the fight

The intro, the round call and the round's end are not rolled back with the
battle, and inputs can skip parts of them, so the session predicts only
while the fight runs. From a KO to the next "FIGHT!" (and through round 1's
call) it runs in lockstep: the round's end and the call's banners then run
on confirmed inputs only, and no load crosses them. The battle HUD is not
rolled back, and a load across a round over counted the round's win again.

Lockstep advances at most the input delay's frames per one-way trip, so
outside the fight the local delay goes up to cover the trip and its jitter.
Lowering it makes GekkoNet skip local inputs for the difference, so it comes
down before "FIGHT!": the round call (flow event 5) lasts 196 frames, and
the delay drops as many frames before its end as it was raised, plus 2. The
last frames of the call then run in lockstep at the player's delay.

### The result

The game counts each side's match wins; whose count went up won, and the
host keeps the score. Once the win screens have played, the battle and the
session hold still until both players agree on what follows: run on, the
results screen opens the game's own Results Menu.

Character select after a match keeps the battle on its results screen while
the picks change, and the next match jumps from there as a rematch does,
without loading the menus in between. Nothing the players press reaches the
game meanwhile. Either player's character select goes to the select screen
at once; the other's rematch cannot outvote it. A rematch rolls Eleven
again: the pick holds the fighter rolled when it was picked, for the first
match only.

## Sets and the next set

A table's set (the keeper's rules) has a vote after each game; no choice
within 15 s counts as a rematch, so a player who walks away does not hold
the table. Once a player has won the set, the side panels say so for 6 s,
then both go back to the room. The joiner ends from the host's score too, as
the host's Bye can miss.

When nobody else waits for the table, the set's end is a vote for the next
set: both choosing to go on asks the keeper, and its yes starts the next set
in this session. Neither choosing by the vote's end sends both back to the
room, so two players who walked away do not play set after set. The keeper
not answering within 5 s also goes back to the room. The next set starts the
score, the games played and the streaks over: the player bars, the results
screen's Wins, Win Streak and Win Ratio are the set's.

Leaving mid-set gives it up: the Bye says so and the other player wins the
set.

## Frame pacing

Time sync slows a machine that runs ahead by stretching its frames. The
stretch is waited out in the input poll before the pads are read
(`pacing.cpp`). The engine's own limiter waits before that hook toward its
own deadline, a hair over 16.667 ms apart. An absolute 60.000 Hz deadline
in the hook was therefore always already past, and a wait shorter than the
engine's period would only shorten its next wait. So the deadline is
relative to the last frame: with a stretch the frame runs past the engine's
period by that much, and without one there is no wait and the engine paces
as before.

The wait uses a high-resolution timer for all but the last millisecond, then
spins. A frame of 100 ms or more is a hitch, and the next frame is not
paced. Pacing stays on for 250 ms after the driver's last frame and starts
afresh with the next match. Some machines poll input twice per battle frame
(on TestPC a stretch counted twice per frame, the second poll waiting out a
whole period), so only the first poll after a netplay frame paces.

## Saves and checksums

Both peers add player 1 then player 2 to the GekkoNet session, so the
handles mean the same on both sides. The starting state is saved too, so a
setup that did not sync can be diffed word by word.

A frame's checksum folds its regions' hashes. A frame that ran on predicted
inputs is hashed once its inputs are confirmed, from its save: the same
bytes it had after its last simulation. Its checksum then goes into
GekkoNet's save, where the health check reads it later, along with the
recording and the state history. GekkoNet reuses a save slot the
prediction window plus 2 frames on, and a frame still waiting in it is
confirmed by then, so it is hashed before the slot is overwritten. A load
settles the frame loaded and those before it, since their inputs are
confirmed; the frames after it are about to run again.

GekkoNet loads only the frame before the first mispredicted one, and
confirmed inputs never change. A save whose next frame's inputs are
confirmed can therefore never be loaded, and is skipped. A stress session
confirms nothing, so it saves every frame. Stress mode runs frames again
with the same inputs and names the regions and the words that came out
different.

Once the outcome is decided every checksum is sent as 0, since the win
screens follow each machine's own menu input. GekkoNet never predicts
further than the prediction window past the last confirmed remote input, so
anything older is final on both peers; that is the frame its health check
compares, and the frame periodic dumps are taken from. Once the states
diverge every later health check fails too. Health checks can arrive out of
order, so the first event is not always the earliest frame: the session
keeps the dump of the earliest frame among the first few events, which both
peers then most likely share.

Costs are logged per advance unless the line says otherwise. "Per frame" is
the whole work over the frames that were not replays, about one per
rendered frame.

## Spectators

The host keeps the last 40 matches' streams, setups and starting checksums,
whether anyone watches or not: a spectator can join mid-match and starts
from frame 0, and a spectator behind watches a match to its end after the
players have moved on. The match list goes out once a second, in slices.
The stream goes out in batches, at most 4 ahead of a spectator's ack, and
again from the last ack after 300 ms without progress. A table set's viewers
watch through the keeper's relay, one hop more, so they start (and start
again after running dry) 120 frames behind; Direct Match spectators watch
the host itself.

A spectator's first start waits for the whole delay, and a start after
running dry for a quarter of it. Below a quarter of the delay in hand, the
spectator holds one rendered frame in every 6 (every 3 below an eighth), a
slight slow motion that lets the stream catch up, and goes back to full
speed at half the delay. The relay's frames come in bursts up to about
0.8 s apart (2026-11-07), so the slow motion starts only that low.

## Ending

A session ends once: the room's leave notice and the other side's Bye
usually both arrive. A Bye goes out at once with the reason for the other
side, since leaving the room would otherwise reach them first as "the other
player left". Closing the game's window sends it too. A new host or join
ends the session first, and a battle it ran goes back to the main menu: left
running, the game played it on as a local match with one pad on both
fighters.

## Recordings

Each match is recorded to `Polarity\matches` as
`<date>_<time>_<P1 name>-<P1 character>_vs_<P2 name>-<P2 character>_<match id>_<p1|p2|spec>[_desync].sfvr`,
with `_` only between the parts, and its log next to it with the same name
and `.log`: a summary, then every line logged since the session began or the
last match ended. A player's matches also go to `Polarity\replays` for the
Battle Log, compact, without the side part, kept until the player removes
them. A desync report mid-match is written on a worker thread: compressing
and writing it on the game thread cost frames of 100 ms and more. Paths are
UTF-8, so a game folder outside ASCII works.

The Battle Log's set is keyed by its first match's id, the same on both
sides: a table's set starts with its first game, a Direct Match set with
each match from the lobby. Each match log says whether the other side sends
desync reports, so a report with one half missing is explained.
