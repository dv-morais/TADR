#pragma once

// BuildWeaponSlotGuard -- fixes the stockpile ("Nanolathing") divide-by-zero crash and
// a separate, adjacent out-of-bounds bug in the same order type. Full derivation, byte
// evidence and test plan: ai-reference/build-weapon-slot-guard/CLAUDE.md and the project
// plan referenced from it.
//
// ---------------------------------------------------------------------------------
// FIX 1 -- root cause. `[bin]` VERIFIED.
// ---------------------------------------------------------------------------------
// WeaponDef_LoadTdfProperties @0x0042E440 stores a weapon's `reloadtime` TDF key as
// `WeaponStruct+0xE4 = (WORD)(int)(reloadtime * 30.0)` (0x0042E54B-0x0042E561) and its
// `stockpile` key into bit 28 of `WeaponStruct+0x111` (0x0042EA39-0x0042EA5F). Nothing
// checks that a stockpile weapon actually got a usable reloadtime: an absent key
// (TA's TDF getters default to 0), an explicit 0/negative, or a reloadtime whose *30
// lands on an exact multiple of 65536 (the `mov word` truncation, smallest case
// ~2184.53s) all produce `WeaponStruct+0xE4 == 0`.
//
// Every consumer of that field divides by it unchecked:
//   - Unit_GetLinkedBuildWeaponPercent @0x00439D20 does `idiv esi` with esi==0 ->
//     EXCEPTION_INT_DIVIDE_BY_ZERO at 0x00439D65. This is the crash that started this
//     investigation.
//   - MissionTick_BuildWeapon @0x00402B70 divides by the same zero four times in x87
//     (0x402C0D/1E/32/4A). It currently survives only because #Z/#I are masked by
//     default and two INF/NaN results happen to cancel -- a coincidence, not a guard,
//     and contingent on the FPU control word.
//   - Gameplay: the shot completes on its first tick, so the weapon stockpiles to its
//     200-shot cap almost immediately and the order then persists with a ~10s
//     recheck window -- so selecting a unit carrying such a weapon is what kills you,
//     repeatedly, not a one-tick fluke.
//
// FIX: clamp WeaponStruct+0xE4 from 0 to 1 (the smallest non-degenerate value) at the
// moment weapon-TDF loading finishes, but ONLY for weapons that are already degenerate
// (stockpile set AND the stored divisor is 0). A correctly authored weapon is untouched,
// bit for bit. This single write fixes every consumer above at once because it removes
// the zero at its source instead of defending each division separately. Logged, with
// the weapon named, so the real fix (correcting that weapon's TDF) is visible to
// whoever owns the data -- this clamp is a safety net, not the intended repair.
//
// Hook site: 0x0042F313, the function's own epilogue (`push ebp; call 0x49E010`),
// which runs after BOTH reloadtime and stockpile have already been stored. VERIFIED
// this session, not merely assumed: EBP (the WeaponStruct* this whole function
// operates on) is written exactly once, at 0x0042E489, and never reassigned anywhere
// between there and 0x0042F313 -- confirmed by disassembling the complete function and
// finding every instruction that writes EBP. The loop back-edge at 0x0042F30D ->
// 0x0042EFC3 does not touch it either. So this hook always sees the fully-parsed
// pointer for whichever weapon just finished loading.
//
// D1 (ai-reference/tools/verification/scan_stockpile_weapons.py) found ZERO weapons
// matching this shape in the operator's live Escalation data (240 weapons scanned,
// all ten stockpile weapons have healthy explicit reloadtime values) -- so this fix is
// confirmed correct and inert for that dataset, not confirmed as the cause of the
// original crash. It stays in as free, zero-behaviour-change hardening for any other
// mod's data (or a future edit) that hits the same authoring mistake.
//
// ---------------------------------------------------------------------------------
// FIX 2 -- a separate, adjacent defect found in the same code paths. `[bin]` VERIFIED.
// ---------------------------------------------------------------------------------
// UnitOrdersStruct::BuildUnitID (+0x36) is read as a weapon-slot index by both
// consumers above with NO bounds check against the only three real slots, {0,1,2}
// (UnitStruct::Weapon1/2/3 are the only weapon pointers a unit has -- tamem.h,
// static_assert-confirmed strides). An index of 3 or more walks off the end of the
// per-slot array into adjacent UnitStruct fields:
//   - Sim (MissionTick_BuildWeapon, unconditional load at 0x00402B89, BEFORE the
//     order's State dispatch): idx=3 lands on YPos__'s low byte -- silent desync;
//     idx=4 lands on FirstUnit's low byte -- a pointer smash.
//   - HUD (Unit_GetLinkedBuildWeaponPercent, 0x00439D51): the same stride reads a
//     non-pointer as a WeaponStruct*, then dereferences it at +0xE4.
//
// +0x36 is reused with DIFFERENT meaning by other, unrelated mission-tick handlers
// elsewhere in the binary (confirmed by a whole-.text scan for the same struct
// offset: other functions compare it against 2 or 3 for entirely different order
// types). That is exactly why this fix hooks INSIDE the two functions that are only
// ever reached for a BuildWeapon order, rather than attempting any kind of global
// clamp on the field -- a global clamp would silently reinterpret data other order
// types depend on. Scoping the check to these two call sites is what makes it safe.
//
// No evidence this has ever fired live -- D1 cannot test it (it is a runtime state
// question, not a TDF data question) and the operator does not remember which unit
// was selected at the time of the crash. Included anyway per the operator's
// judgement: an out-of-bounds write into position and pointer fields is
// game-breaking regardless of observed frequency, and the fix is free (Class B is
// acceptable here -- the DLL ships with a new game version, so there is no mixed
// client-version fleet to keep in lockstep).
//
// FIX: bounds-check the index at the top of each function and, if it is out of range
// (or the order pointer itself is not safely readable), redirect to that SAME
// function's own existing bail-out path rather than fabricating a new one:
//   - Sim: redirect to 0x00402BA4, vanilla's own "unrecognised order state" return
//     (already produced today for any unknown State value, so the caller already
//     handles it -- this adds no new behaviour surface, only reaches an existing one
//     from one more place).
//   - HUD: redirect to 0x00439D6B, the function's own `xor eax,eax` return-0, already
///    an exercised path today whenever BackgroundOrder is null or the order list is
//     exhausted.
// Both trigger paths log (throttled) and drop a CrashTrace_RecordEvent breadcrumb --
// if this ever fires live, that is the discovery of a second real bug and it must be
// visible, not silently absorbed.
//
// ---------------------------------------------------------------------------------
// Gating: BUILD_WEAPON_SLOT_GUARD_ENABLE (config.h / config_*.h). 1 on Escalation,
// 0 elsewhere -- every hardcoded address here is specific to Escalation GOLD
// 10.1/10.2's TotalA.exe. Disabled arm still identifies itself in the log so a
// module that failed its own byte check cannot be mistaken for one compiled out.
// ---------------------------------------------------------------------------------

namespace BuildWeaponSlotGuard
{
    // Byte-validates all three sites before installing any of them. Idempotent.
    // Runs an in-process self-test against synthetic data first (see the .cpp) and
    // refuses to install if that fails -- logic that misbehaves on synthetic input is
    // not trusted against live game data either.
    void Install();

    // Restores original bytes. Safe to call if Install() failed, was disabled, or did
    // nothing.
    void Shutdown();
}
