# Changelog

Releases of Polarity, unofficial online play for Street Fighter V. Newest
first, in the [Keep a Changelog](https://keepachangelog.com) format.

Each version's first paragraph is the summary players see in the game
(Polarity settings › Updates). A release tag doesn't build without its
version's entry here, and the entry becomes the GitHub release's description.

## [0.4.4] - 2026-11-25

Fixes an Urien desync from his alternate costume, crashes when a session
went back to the lobby as a battle loaded and on an empty save file, and
Battle Log round jumps that drifted from the recording. Each game now
builds a match's start on its own and checks the other side's picks.
0.4.4 plays only 0.4.4.

### Added

- `load=none` under `[loader]` in polarity.ini starts plain Street Fighter
  V, with nothing of Polarity loaded.
- Polarity is under the MIT License. Each release carries the licenses of
  the libraries built into it in `polarity_third_party_notices.txt`.

### Changed

- Updates come from Polarity's releases on GitHub, and so do the
  installers on polaritymod.link.
- Each game builds a match's first frame on its own, and nothing from the
  other side is copied into it. If the two starts differ, the match ends
  after 2 seconds and both sides save a desync report.
- Picks and match setups from the other player, from the host a spectator
  watches and from recordings are checked against this game's own
  characters, costumes, colors and stages first. A bad one ends the session
  with the reason on both sides, and a recording with one doesn't play.
- When SFV Ember's launcher starts the game, Polarity steps aside and loads
  nothing.

### Fixed

- Urien could start a match in a different costume on each side when his
  player held LP+LK+HK for his alternate costume.
- A crash when the other player went back to character select, or a
  watched match ended, just as the next battle loaded.
- A crash at the start of some battles, where the game closed itself.
- A crash on a profile whose save file was empty.
- A Battle Log round jump could drift from the recording partway through a
  fight, mostly on the Steam Deck.
- Joining a room by code right after a failed join waited on "joining" for
  good.
- A damaged recording could end the game. It is now refused.

## [0.4.3] - 2026-11-13

Fixes desyncs from the first frame of some matches and three seconds after
a KO, a spectator crash as a match loaded and a freeze when closing the hub.
Round win markers and the FIGHT banner now show right online, and the frame
meter reads air fireballs and supers correctly. 0.4.3 plays only 0.4.3.

### Added

- Rollback netplay built on the game's local Versus mode and its own
  rollback manager, with GekkoNet sessions and Weyvelength rooms. Players
  connect peer to peer, or through a TURN relay when they can't reach each
  other directly.
- Custom rooms, public or private, with tables, table rules (rounds, timer,
  stage), spectators, chat and moderation. Rooms list only players on the
  same Polarity version. Public rooms filter names and chat, and show each
  player's country flag; both can be turned off. Players waiting at a table
  can play Training (LB) until their set starts. Direct Match by code too.
- The game's own select screen, intros, round calls, KOs and win poses
  online. Start skips them once both players press it.
- A fixed input delay for each match, chosen by each player. Polarity
  suggests one from the ping and jitter, or uses a delay set in Polarity
  settings.
- Checksums on every frame. A desync shows a banner and saves a recording
  on each side, and desync and crash reports upload to the project's server
  unless turned off. Polarity settings › Logs packs the logs to share.
- The Battle Log: every match is recorded and can be watched back, stepped
  through frame by frame, jumped round to round, or taken over at any point.
- A frame meter and a hitbox display in Training and Battle Log replays.
- An online benchmark: a recorded round played with an 8-frame rollback on
  every frame, which passes at an average of 59 fps or more. It also checks
  whether mods have changed the game's battle data.
- The Polarity hub, which replaces the game's main menu at launch: Play
  Offline, the Battle Log, Custom Rooms, and Polarity settings on Start.
- The game starts offline, skipping the Capcom login, and the profile gets
  the DLC content the Steam account owns.
- Stage settings, voice language and a custom Random stage list.
- Updates from polaritymod.link, signed and offered at start. Installing
  one restarts the game on the new version.
- Runs on Linux and the Steam Deck through Proton.

### Changed

- Round transitions run in lockstep online again: from a KO to the next
  round's FIGHT, and round 1's call (the match's opening intro still rolls
  back). A rollback across them re-ran the round's end under the HUD, which
  isn't rolled back, so the round win markers doubled or landed on the
  wrong side, and the FIGHT banner snapped back to "ROUND n" and often never
  showed. `lockstep_round_transitions=0` under `[netplay]` in polarity.ini
  lets them roll back.

### Fixed

- A desync from the first frame of some matches: a side could take the
  match's starting state before the battle had finished setting up, so the
  two players started from different states. Each side now waits for the
  battle's state to settle first, and the joiner's log notes when the
  host's state layout differs from its own.
- A desync about three seconds (182 frames) after a KO, when a rollback
  landed between the KO and the end of the round: a timer of the game's
  round phases could stay as the other timeline had it, so the next phase
  started a frame late on one side.
- A crash for a spectator who stopped watching as the match loaded.
- Closing the hub froze the game for about 21 seconds when a custom server
  set in Polarity settings didn't answer.
- Mod paks made with older tools weren't checked for battle data (the log
  said "index out of the file").
- Frame meter: air fireballs, Ryu's Critical Arts and Cammy's EX Cannon
  Strike and V-Trigger 2 kick read as neutral once their counter-hit
  frames ended, and now show their recovery. Akuma's air fireball landing
  no longer counts as stun.
