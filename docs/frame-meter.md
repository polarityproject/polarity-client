# Frame meter

The frame meter shows, frame by frame, what each fighter is doing: startup,
active frames, recovery, stun, invincibility and armor. It also shows the
startup, total and advantage numbers of the last exchange.

Code: `src/mod/frame_meter.{h,cpp}`. The actor layout it shares with the
hitbox display is in `src/mod/battle_actors.h`.

## Where it shows

| Where | Default | Toggle |
| --- | --- | --- |
| Training | on | Polarity settings → TRAINING → Frame meter |
| Battle Log replays | off | Start ("Show/Hide frame meter" on the stripe); remembered |
| Online matches and spectating | never | |

The choices are kept in `polarity.ini` as `[framemeter] training=1 replays=0`.

## What it draws

Two bars of 90 blocks (1.5 s), P1 over P2, centred above the V-Trigger
gauges. One block is one battle tick.

| Block | Color | When |
| --- | --- | --- |
| Counter-hit state | green #1fa37e | the move file's status flag 0x20 is on for that frame |
| First active | pink #ffa3ae | the first frame of a run of active frames |
| Active | red #e5435a | an attacking hitbox or throwbox is in the live hit list (proximity boxes don't count) |
| Recovery | cyan #5fdcf2 | in a move before its interrupt frame; the jump scripts (pre-jump, airborne, landing); every frame of a script that has the counter-hit state on some frame; any airborne frame |
| IASA | blue #2a63c4 | from the script's interrupt frame on |
| Parry | purple #8657e0 | a live hurtbox whose move-file record has armor effect 2 |
| Armor | orchid #d662d0 | a live hurtbox with armor effect 1. In a move before its interrupt frame it counts as the move; otherwise the fighter counts as free to act |
| Hit / block stun | yellow #f2d94a | the fighter is in stun, from a script change on, until the damage script's interrupt frame |
| Proximity guard | brown #9c683a | the guard pose before an attack is blocked (scripts 50–61, FGUARD_L/M/H/C with their _NEAR and _FAR forms) |
| Invincible | white | no strike or projectile hurtbox while acting |
| Strike invincible | white/red stripes | projectile hurtboxes only |
| Projectile invincible | white/orange stripes | strike hurtboxes only |
| Neutral | dark | everything else |

Other marks:
- **Counts** above P1's bar and below P2's give each run's length over its
  last block. A one-block run has no count.
- **Orange lane:** the player's projectile is out.
- **Cream tick:** a super freeze or a cinematic held the meter at that point.
- **White borders** mark milestones: the first airborne frame after the
  pre-jump, the first landing frame, and an exchange's last frame before both
  fighters are neutral.

The numbers on the left:
- **Startup:** the first active frame, counted from 1.
- **Total:** frames until the fighter can act again, which is where IASA
  begins.
- **Advantage:** frame advantage after an exchange in which someone was
  stunned.

## Timeline rules

- One block per battle tick, sampled as the next tick starts.
- **Not counted:** hitstop (both fighters' playback speed is 0), super
  freezes and cinematics. A freeze or cinematic leaves a cream tick.
- **Not recorded:** until both fighters have been free to act once in the
  battle (this skips Training's start-up scripts), and outside a replay's
  rounds (intro, KO, win poses). Each round's call starts the meter again;
  the last exchange stays up through the KO.
- **Neutral:** while both fighters are neutral the meter holds, and the last
  frame gets the end marker.
- **New exchange:** a player acting from neutral or IASA while the other is
  free to act (not stunned, not in a move) starts a new bar. Combos,
  blockstrings and whiff punishes carry on; a fresh approach, or a frame trap
  after the opponent recovered, starts over.
- **Past 90 blocks** the bar wraps, clearing only the block after the newest.
- **A move's start** (for Startup and Total) is an action from neutral or
  IASA, or a change into a script with its own hitboxes at that script's
  frame 0. A hand-over later in a script is the same move: Urien's st.HP
  starts as 5HP_HOLD and, released early, goes on as 5HP from its frame 7, so
  it reads Startup 12F.
- **Moves without hitboxes or an interrupt frame,** such as air fireballs,
  Ryu's Critical Arts and Cammy's EX Cannon Strike, count as moves for as
  long as their script runs, because the counter-hit state appears only in
  moves. Out of a jump, such a move starts over at its frame 0, so an air
  fireball reads its own startup.
- **Stun** counts only from a script change on. Some moves drop the stun flag
  with no hit (Akuma's air fireball landing).
- **STAND (script 0) is always neutral,** whatever its move file says. Akuma's
  and Birdie's long idle poses would otherwise read as attacks.
- **Proximity guard** ends a move like stun does, but starts no exchange.

### In replays

The meter keeps one record per recording frame. When the frame on screen
follows on, new records are applied; after a step back, a seek or a round
jump it is rebuilt from the round's start, so stepping either way shows the
bar as the frames led there. A paused replay keeps its meter. Hidden, the
meter keeps recording, so showing it again fills it at once.

Training's pause menu stops the ticks, so the meter and the hitboxes hide
while it's open.

## How it reads the game

- **Playback speed:** the script runner's +0x64 (16.16 fixed point). It is 0
  during hitstop and on the frozen side of a super freeze.
- **Super freeze:** the frozen fighter's position block +0xB90 is 1. The
  attacker's script keeps running.
- **Cinematic:** the actor's +0x3DD8 is 1 from the cinematic's start to the
  end of the victim's damage script.
- **Stun:** the fighter state object (a separate 13,092-byte region, paired
  with the actor by side) has +0x738 = 0 while damaged.
- **Helpers:** R. Mika's partner is a third fighter actor with a state object
  whose side is -1. States are paired by side, skipping side -1. On a side
  with two fighter actors, the meter takes the one whose script is running.
- **Counter-hit state:** the move file's status section carries flag 0x20.
- **Parry and armor:** the hurtbox record's armor effect (+44: 1 armor,
  2 parry, 3 V-Shift, which isn't shown), strength (+37) and hits (+38).
- **Interrupt frames:** a fighter is actionable from its script's interrupt
  frame; 0 means none. A damage script's speed is scaled so its interrupt
  lands as the stun ends. Checked against published data: Ryu's st.LP on Ken
  reads Startup 3F, Total 10F, Advantage +4F.
- **Projectile owner:** the fighter whose side the effect had when it
  appeared, kept for the effect's life (its spawn point marks it). With both
  fighters on one side at that moment, the one nearer the spawn point.

The research command `ftrace <ticks>` logs every snapshot word that changes
per tick, with each fighter's script and frame.

## Not checked yet

- A guarding training dummy: blockstun is assumed to use the hitstun flag.
- Effects that aren't projectiles, for the projectile lane.
