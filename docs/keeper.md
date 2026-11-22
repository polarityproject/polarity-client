# The keeper

`polarity-keeper` runs beside the rooms server and keeps Custom Rooms: it
hosts every room, answers players' requests, runs each room's tables, lists
public rooms and relays table sets to spectators. Players' clients talk to
it through the rooms server (Weyvelength), as room members do. The server
builds it on Linux; the mod's build makes a Windows copy for local tests.
The code is `src/keeper/keeper*.cpp`, with the class in `keeper.h` and the
shared constants in `keeper_internal.h`:

| File | What it covers |
|---|---|
| `keeper.cpp` | connections, the main step, commands, the saved state |
| `keeper_rooms.cpp` | the lobby channel, joining and hosting rooms, members, requests, chat |
| `keeper_listing.cpp` | the public room list, player counts, the status and metrics, names |
| `keeper_tables.cpp` | tables: seats, calls, results, the rotation |
| `keeper_relay.cpp` | closing rooms, spectator relays |

The messages themselves are in `src/core/keeper_protocol.h`;
[netplay.md](netplay.md) covers a table set from the players' side.

## Running the keeper

```
polarity-keeper --key <file> [--server host:port] [--state <file>] [--words <file>] [--ports A-B] [--per-link-ports] [--protocol N]
                [--metrics <file.prom>] [--status <file.json>] [--server-stats <file.json>]
polarity-keeper --new-key <file>    writes a key and prints its public half
```

The keeper logs to stdout (the journal on the server), and keeps count of
the time spent writing log lines, since a slow journal or Docker log driver
holds up the whole loop. When asked, it writes every 5 s its metrics in the
Prometheus text format (`--metrics`, for node_exporter's textfile collector)
and the public room list as JSON (`--status`, for the rooms API), each to a
temporary file renamed over the old one, so readers never see half a file.
`--server-stats` reads the rooms server's own counts (its `--stats`) for the
players online in `--status`. The word list (one entry a line) is read again
on SIGHUP (`docker compose kill -s HUP keeper`), without a restart.

## Connections

Each room the keeper holds has a connection of its own to the rooms server,
plus one for the lobby. Connects run on worker threads, so a connect that
waits (a firewall dropping new connections, TCP retrying, a slow name
lookup) holds up no room. A connect not through after 30 s counts as
failed; its client, if it ever arrives, is destroyed. A lost lobby
connection is tried again after 5 s. Datagrams sent to the keeper are not
used and are drained.

## The lobby channel

Players meet in a lobby channel (the desk) before they pick a room. With no
lobby channel of its own (a first start, a lost state file, or the saved
channel gone), the keeper first looks for one of this build's made by an
earlier keeper and joins it: the players may already sit there, and a
second channel would split them, since hubs pick the busiest. The players
then hand its host back to the keeper.

## Rooms

The keeper hosts every room. It writes a proof into its own member data
that any member can check, host or not. Anything a player wrote while they
were host is written over. A room's title is kept as written; an older or
broken one has none. A new room closes if its owner has not come within
30 s, and an empty one closes after 3 s. Rejoining after a restart, the
keeper takes the members already there as joined.

Requests are numbered per member; a request at or below the last number
seen is a repeat and is ignored. An answer is sent again every 250 ms until
the member acknowledges it. Only known request kinds count in the metrics,
since what members send is theirs to choose.

Each member's country comes from the rooms server (its `@cc` key, set from
the member's address; clients cannot set `@` keys): two letters (ISO 3166
alpha-2) or nothing. It goes out on the rooms API.

## Names

In a public room, a name the word filter rejects is shown to everyone as
"Player 1234": four random digits that no other masked member of the room
has. The keeper writes the room's list of masked members whenever it
changes, and again when members join or leave. Other names a room shows as
"Player N" use the member's place in the room by join order, as every
client draws them.

The word filter (`core/keeper_words.cpp`) folds what it compares: lower
case, with the digits and symbols that stand for letters read as those
letters (0 o, 1 i, 3 e, 4 a, 5 s, 7 t, @ a, $ s), and everything else
dropped. A word file entry is a word, a prefix (ending in `*`) or a phrase
(words with spaces between them). An entry of five letters or more matches
anywhere in a folded name; a shorter one matches only a whole word of the
name, or its start for a prefix. In chat a word ends at whitespace or ASCII
punctuation, but `@`, `$` and `'` belong to words ("a$$", "don't"). A
phrase matches only as its words in a row, never letters typed apart, up to
six words, and the longest run is starred first. Each character of a match
becomes one `*`. A public room carries the list in its data (`wl0` and on,
each value at most 512 bytes) with its hash (`wlv`, FNV-1a).

## The public list

Public rooms are listed in the rooms server's own room listing: each public
room's slots, signed, written by the keeper as the room's host, and the
desk's player count and summary. Players list rooms without joining one.
A room is listed once it is public with its owner in; private rooms, rooms
whose owner has not come, empty rooms and rooms taken off the list are not.
The list and the desk's count are written at most every 2 s. The summary
lists the busiest client versions first and is cut at 128 bytes. Each
table's rules, phase and seats go out on the rooms API's per-table view.

## Player counts

Players online (the rooms API's `online`) come from the rooms server's
connection count while its stats file is fresh, since browsing players are
in no room. Without it, the count is the members of rooms: a player is in
one room at a time (the desk or a custom room), so the members add up
without counting anyone twice. Connections that are not one player each
are left out of the server's count: the keeper's own (a room, relay rooms)
and the second connections players open for a match (both players of a
table set) or to watch one through a relay.

## Tables

A room's tables keep their state across a keeper restart; new tables open
empty. After a restart only members still present keep their places, and a
set that was running goes on without the keeper (its players report the
result). Whatever else was in progress starts over.

While the room has no host, nothing moves. A player is at one table at a
time: joining one takes them off any other. A table nobody waits at takes a
player straight into a seat.

A call gives the two seated players 21 s to ready; the your-turn card
shows 1.5 s less for the trip. After a set that broke off a call lasts
26 s, since its players are still leaving the battle. A player who did
not ready goes to the back of the queue; a second lapse in a row takes them
off it. A player who did ready keeps the seat and their Ready for the next
call. If a call or a set loses one of its players, the other keeps the
seat and the next in line takes the empty one. Cancel on the connecting
modal (clients send it only before their match began) counts as leaving
the room: the other keeps the seat and the table is called again.

Player 1 hosts the match room and has 45 s to open it and report its code;
player 2's ticket goes once it is open. Tickets and notices are sent again
until acknowledged, as answers are, numbered after the player's last
request so their acknowledgement covers them.

Player 1 reports the result; player 2 only once player 1 is gone. A player
who leaves a set forfeits it: the other wins, the rotation runs, and the
one who left is off the table and its queue. After a set the result panels
show for 5 s, then the rotation runs. When the set is won, both want more
and nobody else waits, the same two play on in the same match room and
seats; the stays limit does not count it. Someone waiting, or a set that
did not finish, ends the set as a result does.

A request that matches nothing (a phase the table has left, a seat it does
not hold, a table that is gone) is refused; races between players show up
here. A member the room bans is barred until they reconnect, since the
rooms server's ban holds the connection.

A table's state travels as one line:
`<phase>|<p1>|<p2>|<ready bits>|<w1>|<w2>|<code>|<stayer>|<streak>|<last winner>|<last loser>|<lw>|<ll>|<call>|<call seconds>|<watch>|<again>`.
Older keepers send 15 fields (before the relay's room, `watch`) or 16
(before playing on, `again`), and readers take all three.

After a set, who may stay depends on the rotation: the winner, the loser, or
nobody (round robin), while they have played fewer than the stays limit's
sets in a row at the table. The others go to the back of the queue. When
both go back, whoever has now played two sets in a row waits longest;
otherwise the loser goes first. So three players at a round robin table take
turns whoever wins.

The public list sorts rooms with open seats first, then rooms with room to
queue, then full rooms, and by members within each. A room's listing gives
its Polarity version as `client_version` (players list and join only their
own version's rooms; null from a keeper before versions) and again as
`sfvr_version`, its name before 0.4.0, for readers written then. It lists
every table with its rules, state, seats (player 1, player 2; null when
open) and the running set's score; the room's `rules` stays the first
table's. Names and codes in the list have no tabs, newlines or commas (the
list's separators) and nothing below space, and JSON strings show bytes
that are not valid UTF-8 as U+FFFD, since names are players' own.

## Spectator relays

A table set can be watched through a relay (`core/spectate_relay.h`): the
keeper joins the match room for the viewers and relays the stream, one hop
more than watching directly. The relay's room is made once its connect
comes through. It joins the match room with its first viewer; the host
keeps the set's games, so a viewer who comes late still starts at the
first. Without player 1's build (from an older client), the relay joins at
once to learn it.

The relay room carries what the match room says of the build, so viewers
check it as they would the host's, and the host checks the relay's proof
before it lets it watch. Nobody chats in the relay room, as in the match
room. A relay ends when the set is done, when the host never lets it in, or
when the set is over and nobody watched. When the same two play on, the
relay carries on to the next set's end; a set's relay keeps serving its
viewers, and nobody new joins.

On the host's side the relay says Hello again every second until the host
knows the member is the keeper, since its proof arrives through the server.
It takes the oldest game the host has that it doesn't hold yet: the host's
Loaded (its checksum, for the viewers), then the stream, at most 4 batches
ahead, as the host sends. With every game in hand it sends a heartbeat for
match 0. Games keep the host's order. Games it no longer lists (older ones
it dropped) stay in front, and a game known only from its Welcome so far
(the next game, before the host's list has it) stays after: viewers take
the next game by its place in the list, and when it once came first, live
viewers loaded game 1 again for game 2. When the same two play on into a
next set, the score starts over and the set's end is to come again.

Viewers get the set's games at once. A table viewer starts at the first;
with the live game's Welcome first, it loaded that one and then the first,
and lost half a minute. A new viewer gets the list before any Welcome, 1.5 s
after its Accept, and a viewer still on an earlier game takes the players'
match up once that game ends. When a player chooses character select, only
a viewer at the live game's result goes to the select screen with them, and
stays there while they are on it. One still playing a game, the live one or
an earlier one, is told the match goes on: the select phase would take it
out of its game, and it lost the rest of that game.

Once the host is gone nothing more comes. A viewer is done when it is at
the result of the last game played, or has played through every frame
there is (a set that broke off mid-game); having the first game alone isn't
enough, since a viewer takes frames far ahead of what it shows. Viewers far
behind are still served for 10 minutes after the host left. A viewer that
is done gets a Bye 3 times, 500 ms apart.

A room closing takes its listing and its relays with it; a relay asked for
and never made just ends.
