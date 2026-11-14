# Input delay suggestion

This doc covers how Polarity picks the input delay a session starts at, and
where the numbers come from.

Code:
- `src/core/input_delay.h`: the formula and the measurement;
- `NetView::SuggestedDelay` in `src/mod/netplay_driver.h`;
- the lobby step in `src/mod/overlay.cpp`.

## In short

Nearby players get **0 frames** of input delay, as the game's own online
did. Players farther away get 1, 2 and at most **3** frames. The delay is
chosen so that, at a steady ping, a rollback never has to go back more than
**2 frames**. Jitter (how much the ping wobbles) moves each step to a lower
ping, up to a cap.

The delay a match starts with is the delay it ends with: Polarity never
changes it during a match.

## The rule

```
late  = ceil((ping / 2 + min(jitter, 17)) × 60 / 1000)   frames
delay = clamp(late − 2, 0, 3)
```

- `ping` is the median round trip to the other player over the lobby's last
  10 seconds, in ms.
- `jitter` is the average change between consecutive round trips over the
  same time, in ms, counted up to 17 ms (one frame).
- `late` is how many frames after its own the other player's input arrives:
  the one-way trip (half the ping) plus the jitter, in 60 fps frames,
  rounded up.
- Input delay hides that many frames of lateness, and rollback covers the
  rest. `late − 2` leaves 2 frames to rollback.

In plain words: add twice the jitter (17 ms at most) to the ping, then read
the band.

| ping + 2 × jitter | Suggested delay |
| --- | --- |
| up to 66 ms | 0 frames |
| 67–100 ms | 1 frame |
| 101–133 ms | 2 frames |
| 134 ms and up | 3 frames |

The same bands at a few jitter values (ping as the lobby shows it):

| Delay | 0 ms jitter | 4 ms | 8 ms (typical) | 12 ms | 17 ms or more |
| --- | --- | --- | --- | --- | --- |
| 0 | up to 66 ms | up to 58 ms | up to 50 ms | up to 42 ms | up to 32 ms |
| 1 | 67–100 | 59–92 | 51–84 | 43–76 | 33–66 |
| 2 | 101–133 | 93–125 | 85–117 | 77–109 | 67–99 |
| 3 | 134 and up | 126 and up | 118 and up | 110 and up | 100 and up |

## The measurements

**Ping.** Both sides send a heartbeat every 250 ms. Each echoes the other's
timestamp and how long it held it, so the round trip leaves out the other
side's hold time. The last 40 round trips (10 seconds) are kept, and the
suggestion takes their median, so one slow packet doesn't move it. A
suggestion needs at least 8 of them, so it appears about 2 seconds into the
lobby.

**Jitter.** The mean absolute change between consecutive round trips, the
same measure GekkoNet logs during a match. Two guards apply:
- each step counts at most 50 ms, so a single spike adds about 2.5 ms
  rather than the spike itself;
- the result counts at most 17 ms. Above that the link has spikes rather
  than a steady wobble, and a few frames of delay for the whole match would
  cost more than the rollbacks they save.

**Noise floor.** The session's network thread wakes every 5 ms, so two games
on one PC (no network at all) read a 12–13 ms ping and 1–3 ms jitter. That
is part of every reading, and the bands were fitted with it included.

**Lobby ping and match ping differ.** GekkoNet's in-match ping, which the
pace log lines show, runs about 8 ms higher than the lobby's, because it
also counts each side's wait for its next frame. The rule uses the lobby
ping, the one the select screen shows.

## When it's used

- **A session's lobby.** Once per session, as soon as the suggestion exists,
  the player's delay is set to it. The player can change it there. It isn't
  touched again in that session.
- **Polarity settings → Input delay.** *Auto* uses the suggestion; *0–8*
  starts every session at that many frames. `polarity.ini` keeps it as
  `fixed_delay=auto` or `fixed_delay=<frames>`.
- **A room's table card.** Only the peer-to-peer ping to the opponent is
  known there, so the card assumes typical jitter (8 ms,
  `kTypicalJitterMs`) and offers 1 frame until the ping is measured. A fixed
  setting wins here too.
- Not for spectators, who have no delay of their own, or for sessions a test
  command started.

The select screen and the lobby show "Suggested delay N (ping X ms)", or
"Your setting: N" when a fixed delay is set.

Log lines:

```
overlay: input delay 0, suggested for a 35 ms ping, 3 ms jitter (8 heartbeats)
overlay: input delay 0, the player's setting
netplay: link 59 ms, jitter 3 ms (40 heartbeats): delay 0 suggested, 0 chosen
```

The last is written as each match starts. It adds ": rollbacks deeper than
2 frames are likely" when the chosen delay is below the suggestion.

## Why these numbers

**Zero at short range.** The game's own online had no input delay
(rollback only, with a window of ceil(0.045 × ping) + 1 frames, up to 15),
and players compare Polarity with it.

**Two frames of rollback.** A 1–2 frame rollback is hard to see, and the
game's own netcode rolled back up to 4 frames at a 50 ms ping. Leaving 2
frames to rollback saves a frame or two of delay at every ping below about
130 ms. The cost is frame time: on a desktop PC a resimulated frame takes
about 1.1 ms and a state load about 0.3 ms, so a 2-frame rollback adds about
2.5 ms to that frame.

**At most 3.** Past about 150 ms (typical jitter) rollbacks grow past 2
frames: 3 frames from about 151 ms, 4 from about 185 ms. More delay for far
players is the player's choice.

**Where inputs start arriving late.** In tester match logs (399 match
halves), rollbacks per 100 frames rise from about 0 (inputs on time) to
about 7–9 (every change of the other player's input late) as the ping
passes what the delay covers:

| Delay | On time up to (match ping) | Late from |
| --- | --- | --- |
| 1 | about 40 ms | about 60 ms |
| 2 | about 70 ms | about 100 ms |
| 3 | about 95 ms | about 120 ms |

That is the one-way trip in frames plus about 0.2 frame of slack, in match
ping. In lobby ping (8 ms lower) the slack and the offset cancel out, which
is why the rule uses the plain one-way trip.

**How much jitter.** From 5,506 ten-second samples in the same logs:

| Match ping | Jitter median / p90 / p99 |
| --- | --- |
| 20–40 ms | 4 / 10 / 13 ms |
| 40–60 ms | 7 / 11 / 17 ms |
| 60–80 ms | 8 / 13 / 23 ms |
| 80–100 ms | 8 / 13 / 35 ms |
| 100–130 ms | 10 / 21 / 47 ms |
| 130–170 ms | 7 / 22 / 95 ms |
| 170 ms and up | 10 / 66 / 133 ms |

Half the samples have 8 ms or less, 90 % 14 ms or less. If the round trip
varies like a normal distribution, adding one jitter figure to the one-way
trip covers about 95 % of inputs. The long tail is spikes, which the 17 ms
cap leaves to rollback.

## What to expect

- Up to about 50 ms (typical jitter): 0 frames. Each change of the other
  player's input arrives 1–2 frames late and is rolled back.
- 50–117 ms: 1–2 frames, with rollbacks of up to 2 frames.
- From 118 ms: 3 frames. Rollbacks stay at 2 frames up to about 150 ms, then
  grow with the ping.
- A jitter spike makes one input a frame later than usual: one rollback a
  frame deeper, for that input only.

## Tests

The unit test `input delay suggestion` in `tests/tests.cpp` covers the bands,
the jitter cap, the median and the spike guard. Two games on one PC through
simulated links gave the suggestions the bands predict:

| One way (both sides) | Lobby ping, jitter | Suggested |
| --- | --- | --- |
| none | 12–13 ms, 1–3 ms | 0 |
| 10 ms | 35 ms, 3–4 ms | 0 |
| 25 ms | 69–71 ms, 2–4 ms | 1 |
| 40 ms | 96–97 ms, 6–7 ms | 2 |
| 60 ms | 137–138 ms, 3–5 ms | 3 |
| 25 ms + 0–12 ms jitter | 78–80 ms, 7–12 ms | 1 on one side, 2 on the other |

Each side measures for itself, so the last row's sides read 7 ms and 12 ms
of jitter.

## Open questions

- The match logs count rollbacks and the deepest one, not how late each
  input was. A histogram of per-input lateness would confirm the slack and
  the jitter margin directly.
- Frame time on the Steam Deck at delay 0 isn't measured.
- The table card assumes typical jitter, so a jittery player there gets a
  frame less than the lobby would suggest.
