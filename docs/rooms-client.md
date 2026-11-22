# The rooms client

The rooms client is the hub's connection to the rooms server (Weyvelength)
for Custom Rooms. It lists the rooms, joins the desk and rooms, sends the
player's requests to the keeper, and hands table sets and watches to the
overlay. [keeper.md](keeper.md) describes the other side;
[overlay.md](overlay.md) describes the hub's screens.

The code is `src/mod/rooms_*.cpp`, with the client in `rooms_internal.h`:

| File | What it covers |
|---|---|
| `rooms_client.cpp` | the client's thread, proofs and signatures, the calls the overlay makes |
| `rooms_listing.cpp` | the desk and room listings, peeks at a room's detail |
| `rooms_events.cpp` | the server's events, the keeper's answers, the room as it shows |
| `rooms_tables.cpp` | pings, table requests, watching, a table set's reports |

## The thread

The client runs on its own thread, a step every 20 ms. Each step copies the
server's view of the room into the overlay's (`RoomsSnapshot`) under one
lock. The overlay's calls (`RoomsCreate`, `RoomsJoin`, `RoomsChat` and the
rest) leave a request there for the next step. Opening and closing hold one
more lock from start to end. Two opens at once (the hub on the render
thread, a `rooms open` command on the game thread) each made a client, and
the second replaced the first with its thread still running.

## The keeper's key

The keeper proves itself with signatures: its member data in a room, and
every listing it writes. The client checks them against the rooms VPS
keeper's public key, built in. `[rooms] keeper_key` (128 hex digits) adds
another key, for tests against a local keeper. Checked proofs and signatures
are remembered.

The client's mutex guards its `shared` struct: the view the screens read and
the requests they leave for the thread (create, join, leave, chat, table
requests, tickets, the watched table and the set being played). The rest of
the client is the thread's own. The relay room last handed out gets one
watch ticket a set. A set being played counts as begun once its match
itself starts (the jump), not only its session, and keeps the score seen
last while the session was live.

## Listings

The server takes one listing a second from a connection, so the client asks
for one at a time, at least 1.1 s apart. A listing not answered within 3 s
is asked again. The desks are listed every 2 s while the rooms screen is
open, every 5 s from the hub alone, and every 10 s from inside a room. The
desk's revision says when the public rooms changed, and only then are they
listed again. The server filters the rooms by kind, so only this version's
rooms come back. Unsigned desks and rooms are left out.

With no desk for this build, the client asks once for every desk, to tell
the player which build the server has.

## The desk

The desk is joined while the rooms screen is open and left once it closes.
The client picks the fullest desk with room. A desk it cannot join (full,
or gone since it was listed) is passed over for 120 s. A desk with no keeper
for 30 s is left and passed over too. That is a channel an earlier keeper
left: the players' presence kept it alive while the keeper hosted another,
and the hubs chose the busiest.

Back in the desk after a room, the client dials the keeper (a ping without
a clock). The keeper's side of the old link stays up as far as it knows,
since the rooms server keeps a member's link when it leaves. So its answers
went into a dead link until that timed out, 13 s and more, and a room made
just after leaving one never heard back. The dial makes the keeper take the
new link at once.

## Rooms

A joined room must show it is a custom room within 4 s, else the player is
told there is no room on that code. A room of another version, joined by its
code, is left at once with a notice. While a room's host is not the keeper
it is paused, and its data stays as it was. After a keeper restart a player
may hold host; the client hands it back to the keeper, at most every 2 s.

A create asked while the desk is still being joined waits up to 5 s. The
keeper's answer to a request must come within 20 s.

The server's listing still has a room just left for a few seconds,
counting this player. A room left alone with one member is kept out of the
list until the listing drops it, 30 s at most.

Leaving the room during this player's table set is leaving the set: a
forfeit, told to the keeper before the room hears of the leave. Otherwise
the keeper would only see a member go, as when a game crashes, and break
the set off.

## Names and chat

A member's name shows as the keeper's mask ("Player 1234") unless the player
turned the word filter off. Names show without emoji: the fonts have none.
`core/display_name.cpp` takes out Miscellaneous Technical, Miscellaneous
Symbols, Dingbats, Miscellaneous Symbols and Arrows, the zero-width joiner
and keycap, variation selectors, all of plane 1 (emoji, pictographs,
historic scripts) and the tag characters of flag sequences. Mathematical
alphanumerics (U+1D400 to U+1D7FF) show as the plain letter or digit: the
letters run in styles of 52 (A to Z, then a to z) up to U+1D6A3, and the
digits in styles of 10 from U+1D7CE.
A public room carries the keeper's word list in its data, by its hash; the
client reads it again when the hash changes and keeps it across rooms. Chat
is censored once, as it arrives, and the setting picks which copy shows.

## Peeks

The focused room's detail is asked for every 3 s while it is focused. The
keeper drops a peek that comes within 500 ms of the last one, so the client
never asks sooner than 650 ms. Without that gap, focus moving on quickly
asked for a room that was never answered. A room newly focused is asked for
at once. The answer comes in parts, put together before it shows.

## Pings

Each member is pinged every 2 s. The ping shown is the lowest of the last 4.
Each side answers from a loop that wakes every 20 ms, and the lowest of a
few mostly leaves that wait out. A send to a member with no link up yet
builds one and waits seconds in the link's queue, which read as a far ping
after joining. Those go without a clock, and their answers are not counted.

## Table sets

A table's ticket hands the set to the overlay, once per ticket. Through the
set the client reports to the keeper:

- **The code:** player 1 sends the match room's code and build, which the
  keeper's relay room shows to viewers.
- **The score** as it changes, from player 1.
- **The next set:** when nobody else waits, the set's end is a vote for the
  next one, and player 1 asks.
- **A forfeit** when a player left the set. Both players tell the keeper,
  which takes the first.
- **The result** when the set is over. An unfinished set reports 0 - 0, and
  the keeper calls the table again. After this side's own leave the session's
  score is gone, so the score seen last while it was live goes.

A session that never starts within 30 s reports 0 - 0. A table the keeper
moved on before the match began (the other player left, or the match room
never opened) ends the session.

## Watching

A watched table's viewers watch in the keeper's relay room, never in the
match room. Each set's relay room is a ticket, once. A set still playing out
for this viewer goes first, and the next waits its turn. A call to play at
any table stops the watching.
