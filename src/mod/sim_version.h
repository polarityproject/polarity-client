#pragma once

// The simulation version: recorded in every match recording's build id
// ("/sim-<n>", core/build_id.h). The Battle Log plays a recording only on
// the same game exe and the same sim version, so this is the one number to
// bump when, and only when, the mod changes what a battle computes from the
// same start state and inputs.
//
// Bump it for:
//   - the random seeding (draw seeds, rand(), the move variant picks);
//   - when and how inputs reach the game (the pad latch, the injected values);
//   - what the battle setup writes into the battle (round count, timer,
//     stage name, character versions, voice language, handicap, ...);
//   - state the mod sets or puts back during a straight run (the round
//     director per tick, the round end wait, anything RunBattleTick changes
//     about whole updates).
//
// Not for:
//   - checksum masks, noise maps, which regions are hashed;
//   - presentation (cameras, the HUD, sounds, the frame meter, hitboxes);
//   - rollback-only work (lean replayed ticks, regions added for loads, load
//     callbacks, the camera journal): a straight replay never loads.
//
// When unsure, run scripts/replay-regression.ps1: a reference recording that
// stops passing on an unbumped version is a regression or a missed bump.
//
// History: 12 = the adapter version of 0.3.0 and 0.3.1 (ids before the sim
// part count their adapter version as it). 13 = driven ticks run the battle's
// jobs in order on the game thread (battle_driver.cpp, serial jobs): a frame
// whose result hung on which worker thread ran first can come out the other
// way (779bc488: four single frames of a sim-12 recording).

namespace sfv::mod {

constexpr int kSimVersion = 13;

} // namespace sfv::mod
