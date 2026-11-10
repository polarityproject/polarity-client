#pragma once
// The battle's actors as the game keeps them (docs/hitbox-display.md and
// docs/frame-meter.md have the research). Each actor, the two fighters and
// 16 pooled effect slots, owns a 15808-byte snapshot region R; the owner
// object's vtable sits 0x18 before it. Offsets below are from R unless said.

#include <cstddef>
#include <cstdint>

namespace sfv::mod::actor {

constexpr size_t kRegionSize = 15808, kVtableAt = 0x18;
constexpr uint32_t kFighterVtable = 0x2C4F220, kEffectVtable = 0x2C6E6D8;

// R+0: the script runner. In it: +0x40 the script (its header in the move
// file), +0x48 the script's index, +0x4C the frame (16.16), +0x64 the
// playback speed (16.16: 0 in hitstop and a super freeze, 0.75 in some stun).
constexpr size_t kRunner = 0x0;
constexpr size_t kRunnerScript = 0x40, kRunnerIndex = 0x48, kRunnerFrame = 0x4C, kRunnerSpeed = 0x64;

// R-0x40: the position object. A fighter's x, y at +0x790 (its position
// region s300 at +0x764), and +0xB90 is 1 while a super freeze holds it; an
// effect's x, y at +0x1F0 (+0x1D0 is where it was spawned).
constexpr size_t kPosition = 0x40;
constexpr size_t kFighterXY = 0x790, kEffectXY = 0x1F0, kSuperFreeze = 0xB90;

// R+0x3DB8: the side (0 on the left, facing right; 1 on the right; -1 in an
// effect slot never used). R+0x3DD8: 1 while a cinematic plays (a Critical
// Art's), in the fighter it involves.
constexpr size_t kSide = 0x3DB8, kCinematic = 0x3DD8;

// A fighter's state object S (vtable rva 0x2C515E0 at +0; its snapshot regions
// of 13092 bytes at +0x10 and of 192 at +0x3338, apart from the actor): +0x738
// is 0 while the fighter reacts to a hit or a block (stun, a knockdown, getting
// up) and 1 otherwise; +0x33E0 is its side, as the actor's.
constexpr uint32_t kStateVtable = 0x2C515E0;
constexpr size_t kStateRegionSize = 192, kStateRegionAt = 0x3338;
constexpr size_t kStateFree = 0x738, kStateSide = 0x33E0;

// An effect slot's two flags: 1 while it is out, 0 once it hit or ended (its
// box lists stay as they were).
constexpr size_t kEffectLiveA = 0x11C0, kEffectLiveB = 0x1358;

// The box lists, the boxes active on the actor's frame: hitboxes from R+0x120,
// hurtboxes from R+0x1610; entries of 0x78 bytes: +0 the move file's box (x,
// y, z, w, h), +0x30 the hit type (a byte: 0 strike, 1 projectile, 2 throw,
// 4 proximity guard, 5 projectile cancel) or hurt type (bits: 1 strike, 2
// projectile, 4 throw), +0x50 the id (box << 8 | script << 16; -1 an empty
// slot), +0x58 the entry's vtable (other slots hold another, or old bytes).
constexpr size_t kHitList = 0x120, kHitListEnd = 0x1608, kHurtList = 0x1610, kHurtListEnd = 0x3040;
constexpr size_t kEntrySize = 0x78, kEntryType = 0x30, kEntryId = 0x50, kEntryVtableAt = 0x58;
constexpr uint32_t kEntryVtable = 0x2C4D6F8;

// A move file's script header (the runner's +0x40): +0 the first hitbox's
// frame, +8 the interrupt frame (from it, any action cancels the script: its
// IASA frames; the move's total), +12 the total ticks, +52 the number of
// sections, +60 where their headers start. A section header (16 bytes): type
// (i16), count (u16), then the offsets of its ticks, data and conditions,
// each from the section header. Section types: 1 status (8 bytes: player
// flags, opponent flags; 0x20 = counter-hit state), 5 hitboxes, 6 hurtboxes
// (60 bytes: +37 armor strength, +44 armor effect, 2 = a parry).
constexpr size_t kScriptFirstHit = 0, kScriptInterrupt = 8, kScriptTotal = 12, kScriptSections = 52, kScriptSectionsAt = 60;
constexpr size_t kSectionSize = 16;
constexpr int kSectionStatus = 1, kSectionHurtboxes = 6;
constexpr size_t kHurtboxRecord = 60, kHurtboxArmorEffect = 44;
constexpr uint32_t kStatusCounter = 0x20;
// Script indices every move file shares: 4..12 the jumps (TAKEOFF_V,
// JUMP_V, LANDING_V, then the same forward and back).
constexpr uint32_t kScriptFirstJump = 4, kScriptLastJump = 12;
constexpr int kArmorParry = 2;

} // namespace sfv::mod::actor
