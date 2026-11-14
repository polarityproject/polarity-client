# Hitbox display

The hitbox display draws the boxes the game uses on each frame:
- hurtboxes: green outline;
- throwable areas: yellow;
- hitboxes: red;
- throwboxes: blue;
- proximity block areas: gray.

Code: `src/mod/hitbox_display.{h,cpp}`. The boxes are collected after each
battle frame (`battle_driver.cpp`) and drawn on the overlay's background
list (`overlay.cpp`).

## Where it shows

| Where | Default | Toggle |
| --- | --- | --- |
| Training | on | Polarity settings → TRAINING → Hitboxes |
| Battle Log replays | off | View ("Show/Hide hitboxes" on the stripe); remembered |
| Online matches and spectating | never | |

The choices are kept in `polarity.ini` as `[hitboxes] training=1 replays=0`.

## Where the boxes come from

Every actor (the two fighters and 16 pooled effect slots) has a
15,808-byte snapshot region R. The owner's vtable sits 0x18 before R.

**Box lists.** These are the game's own, and they hold exactly the boxes
active on the frame:
- hitboxes from R+0x120, hurtboxes from R+0x1610, entries of 0x78 bytes;
- +0: the move file's box (x, y, z, w, h);
- +0x30: the type. For hitboxes it's a byte: 0 strike, 1 projectile,
  2 throw, 4 proximity, 5 projectile cancel. For hurtboxes it's bits:
  1 strike, 2 projectile, 4 throw;
- +0x50: the id (box << 8, script << 16; -1 is an empty slot).

The world coordinates at +0x60 are filled a tick late, so they aren't used.

**Position.** R-0x40 points at the actor's position object. Fighters keep x
and y at +0x790, effects at +0x1F0 (+0x1D0 is the effect's spawn point).
A box's world position is x = px + bx facing right, px - bx - w facing left;
y = py + by.

**Side:** R+0x3DB8 is 0 on the left, 1 on the right, -1 in an unused effect
slot.

**Effect alive:** R+0x11C0 or R+0x1358 is 1 while the effect is out.

The lists are part of the rollback snapshot, so replay seeks, steps, round
jumps and takeover rewinds show the right boxes for the frame loaded.

## Drawing

- **Camera:** the `KWBattleCameraManager` of the controller with a local
  player, reached from the battle camera through `KBP_BattlePlayerController`
  (+0x390). Its POV (location, rotation, horizontal FOV) is at +0x370.
- **Mapping:** battle units × 100 = Unreal units, on the Y = 0 plane.
- **Projection:** perspective from the POV, into the 16:9 rectangle the game
  draws centred in the back buffer, in back-buffer pixels. The render
  resolution scale and the window size don't matter.
- **Draw order,** back to front: throwable, hurt, proximity, throw, hit.

## Research commands

| Command | What it does |
| --- | --- |
| `hitboxes 0\|1` | show the boxes in any battle |
| `boxlog <frames>` | each frame's listed box ids per actor |
| `freeze 1\|0` | hold the battle on its frame |
| `winsize <w> <h>` | resize the game window |
| `actorlog <ticks>` | scripts, frames, sides, positions, camera |

## Not checked yet

- Effects other than projectiles: whether they keep their position at
  +0x1F0 and set the alive flags (for example Rashid, Menat, Juri).
- Pushboxes aren't drawn. They're at R+0x3048.
