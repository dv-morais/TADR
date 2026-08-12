#pragma once

// Ports the live-tested Cheat Engine script `ai-reference/reclaim_assist_v5.CEA` into a
// real hook. See CLAUDE.md, ENGINE_NOTES.md and ai-reference/TA-previous-files/
// TA_PROJECT4_RECLAIM_FINDINGS.md for the full derivation, verification log, and the
// exploit/performance analysis this module's design answers.
//
// Feature reclaim (0x404AD0 ground / 0x414770 VTOL) is vanilla's WORST-scaling mechanic:
// the counter decrements by a flat -2 per callback regardless of who is reclaiming, it is
// completely free, and N units reclaiming the same wreck do not stack -- only the fastest
// one's time matters and only it gets the metal. This module replaces that with:
//
//   1. Build-Power-scaled speed, via an exact-rational accumulator (no dead zones: a
//      WT-31 unit is not silently floored to the same speed as a WT-1 unit).
//   2. An energy cost, charged per unit of work actually consumed, through the vanilla
//      resource gate at 0x401180. Total energy per feature is invariant -- it does not
//      depend on who reclaims it or how fast, only on the feature itself.
//   3. Multi-unit assist: N workers on the same wreck each subtract their own decrement
//      from ONE shared counter in a side table, so two equal units give exactly 2x and
//      mixed Build Power contributes exactly proportionally -- precisely how vanilla BUILD
//      assist already works, ported to a subsystem that structurally lacks a target-side
//      progress field (a feature's map cell is 13 bytes with no per-instance state).
//
// Plus a nano-beam visibility fix: the beam is suppressed when the energy gate fails, and
// (unlike the CE reference) does not strobe on any unit whose Build Power puts it below the
// speed accumulator's carry threshold -- see "THE BEAM BUG" below.
//
// ---------------------------------------------------------------------------------------
// WHERE TO TUNE
// ---------------------------------------------------------------------------------------
//   Feature on/off:  src/DDraw/config_<yourmod>.h   #define RECLAIM_ASSIST_ENABLE 1
//                    (0 in every shipped config -- this is an opt-in balance change, not
//                    a bug fix; see "WHY OFF BY DEFAULT" below)
//   The numbers:     the constexpr block below. Tuning guide with worked examples and
//                    tables for every constant follows it.
//   Full reference:  src/DDraw/RECLAIM_ASSIST.md -- the player-facing guide to what this
//                    changes and why, plus a mod-owner section covering every setting below,
//                    the three non-obvious couplings between them, and the multiplayer
//                    determinism rules. Read that before changing anything you are not
//                    already sure about.
//   After editing either, rebuild:
//     msbuild src/DDraw/ddraw.vcxproj /p:Configuration=ReleasePublic /p:Platform=x86 \
//             /p:TDRAW_CONFIG=TDRAW_CONFIG_ESCALATION
//   There is no ini and no in-game menu. TA is lockstep-deterministic multiplayer; a
//   live-editable config is a desync generator (every client must run bit-identical code
//   paths), so retuning requires a rebuild. See CLAUDE.md rule 5 and ENGINE_NOTES section 10.
//
// WHY OFF BY DEFAULT
//   This is a balance change, not a bug fix (contrast REPAIR_RATE_FIX_ENABLE, which removes
//   an unintended rounding bonus). Vanilla feature reclaim being free, flat-rate, and
//   non-stacking is *known, shipped, 25-year-old behaviour*, not a defect. A mod owner opts
//   in deliberately by flipping the macro, the same convention as SHARE_ABUSE_GUARD and
//   PATROLING_CONS_RECLAIM_OR_ASSIST_ENABLE.
//
// THE BEAM BUG (fixed here, present in the CE reference this was ported from)
//   The CE script hides the nano-beam whenever this callback's decrement D == 0, treating
//   that as "the reclaimer went broke". But D == 0 is also the NORMAL way the exact-rational
//   accumulator represents a fractional decrement for any unit with
//   WorkerTime < kSpeedDen / kSpeedNum (45 at defaults) -- it has nothing to do with the
//   resource gate. Emulated on corsumo_dead (C=490): a WorkerTime-30 unit (all four of
//   kVanillaSpeedUnits below) sees D == 0 on 245 of 735 callbacks under the CE's read of
//   the bug -- a 33% strobe. This module tracks "did the resource gate pass this callback"
//   as its own boolean, independent of whether D happened to be 0, so the beam only goes
//   dark when the reclaimer is actually browned out.
//
// See RECLAIM ASSIST TUNING GUIDE below the constants for per-value tables (D, seconds to
// clear a real feature, energy drain, stacking speedup) so a mod owner can predict the
// effect of a change before rebuilding.

#include <cstdint>

namespace ReclaimAssist {

    // Installs the four hooks (ground tick, VTOL tick, ground beam gate, VTOL beam gate)
    // and prepares the per-player assist tables. Idempotent. No-op unless
    // RECLAIM_ASSIST_ENABLE is 1 for this config -- callers still gate on the macro
    // themselves (see ddraw.cpp) so a disabled build carries zero runtime cost, not just an
    // inert Install().
    void Install();

    // Restores the four original instruction sequences and frees the assist tables. Safe
    // to call multiple times.
    void Shutdown();

} // namespace ReclaimAssist

// =============================================================================================
// RECLAIM ASSIST TUNING GUIDE
// =============================================================================================
//
// Every table below was computed by emulating the exact integer algorithm this module runs
// (ai-reference/tools, Python), not estimated. Baseline feature throughout: corsumo_dead
// (metal 950, energy 0), whose channel length is C = 15 + ((950+0)>>1) = 490. Vanilla time
// to reclaim it is fixed: ceil(490/2) callbacks x 2 ticks/callback / 30 ticks/sec = 16.27 s,
// for free, and does not stack. That 16.27 s is "vanilla" in every table below.
//
// ---------------------------------------------------------------------------------------
// kSpeedDen -- the master dial. Seconds to clear corsumo_dead, LINEAR mode (kMode=0).
// The BOLD diagonal is the calibration anchor: whatever WorkerTime equals kSpeedDen
// reclaims at EXACTLY vanilla speed, no matter what else is tuned.
// ---------------------------------------------------------------------------------------
//
//   WorkerTime   den=30   den=60   den=90*   den=120   den=180   den=360
//   30 armflea    16.27    32.60    48.93     65.27     97.93    195.93
//   60 corca       8.13    16.27    24.47     32.60     48.93     97.93
//   90 corck       5.40    10.87    16.27     21.73     32.60     65.27
//  120             4.07     8.13    12.20     16.27     24.47     48.93
//  150 coraca      3.20     6.47     9.73     13.00     19.53     39.13
//  180             2.67     5.40     8.13     10.87     16.27     32.60
//  360 corcom      1.33     2.67     4.07      5.40      8.13     16.27
//  450 coreca      1.07     2.13     3.20      4.33      6.47     13.00
//  6480 coruck     0.07     0.13     0.20      0.27      0.40      0.87
//  19200 corulab   0.00     0.00     0.07      0.07      0.13      0.27
//    (* = default kSpeedDen = 90; read any column's WorkerTime==den row as the anchor --
//     e.g. WorkerTime=150 reclaims at exactly vanilla speed when kSpeedDen=150)
//
// Bigger kSpeedDen = everyone reclaims slower. Ratios between units never change in LINEAR
// mode (a WT-360 unit is always exactly 4x a WT-90 unit), so this dial moves the whole curve
// without distorting it. Side effect: raising kSpeedDen widens the WorkerTime range where
// D can be 0 on some callbacks (harmless for the maths -- see THE BEAM BUG above, which this
// module already handles correctly regardless of where that band sits).
//
// ---------------------------------------------------------------------------------------
// kMode -- LINEAR (0) vs SQRT (1), at kSpeedDen=90. SQRT compresses the 216x spread between
// the weakest and strongest mobile constructor down to about 26x. Effective Build Power
// (EBP) is what actually feeds the decrement; D = EBP * kSpeedNum / kSpeedDen.
// ---------------------------------------------------------------------------------------
//
//   WorkerTime   EBP linear   D linear   EBP sqrt   D sqrt   sqrt vs linear
//        30           30        0.67        51       1.13     1.70x faster
//        60           60        1.33        73       1.62     1.22x faster
//        90*          90        2.00        90       2.00     1.00x -- ANCHOR, never moves
//       180          180        4.00       127       2.82     0.71x
//       360          360        8.00       180       4.00     0.50x
//       900          900       20.00       284       6.31     0.32x
//      1440         1440       32.00       360       8.00     0.25x
//      6480         6480      144.00       763      16.96     0.12x
//     19200        19200      426.67      1314      29.20     0.07x
//
// SQRT_K is deliberately equal to kSpeedDen, which is why the two modes agree exactly at the
// anchor whichever mode you flip to. Pick LINEAR to preserve the mod's real Build Power
// ratios; pick SQRT if factory-tier constructors trivialising every wreck is the problem you
// are solving, or if you want early-game constructors to stay relevant into late game.
//
// ---------------------------------------------------------------------------------------
// kEnergyNum / kEnergyDen -- total energy cost per feature, floor(C * num / den).
// INDEPENDENT of who reclaims it and how fast -- only of the feature itself.
// ---------------------------------------------------------------------------------------
//
//   feature            C      OFF    1/4    1/2   3/4*   1/1    3/2
//   dryrock03         27       0      6     13     20     27     40
//   armrock_dead      46       0     11     23     34     46     69
//   architree01      140       0     35     70    105    140    210
//   corsumo_heap     252       0     63    126    189    252    378
//   corsumo_dead     490       0    122    245    367    490    735
//   armwalk_heap    8685       0   2171   4342   6513   8685  13027
//    (* = default 3/4 = 0.75 energy per work unit)
//
//   Steady drain while working, energy/sec, at kSpeedDen=90:
//   unit        WT     OFF    1/4    1/2   3/4*    1/1    3/2
//   armflea     30      0     2.5    5.0    7.5   10.0   15.0
//   corca       60      0     5.0   10.0   15.0   20.0   30.0
//   corck       90      0     7.5   15.0   22.5   30.0   45.0
//   corcom     360      0    30.0   60.0   90.0  120.0  180.0
//   coreca     450      0    37.5   75.0  112.5  150.0  225.0
//   coruck    6480      0     540   1080   1620   2160   3240
//
// At the default 3/4 the drain is EXACTLY 25% of that unit's Build Power per second, for as
// long as it keeps working. kEnergyOn=false reverts to vanilla free reclaim while KEEPING the
// speed scaling and assist stacking -- a middle ground between "vanilla" and "full economy".
//
// ---------------------------------------------------------------------------------------
// kSpeedNum -- global multiplier. Rescales everything uniformly WITHOUT moving what the
// anchor MEANS (it moves the anchor's speed). Time on corsumo_dead:
// ---------------------------------------------------------------------------------------
//
//                    num=1    num=2*   num=3    num=4
//   corck (WT 90)   32.60s   16.27s   10.87s    8.13s
//   corcom (WT 360)  8.13s    4.07s    2.67s    2.00s
//
// Prefer tuning kSpeedDen alone for most balance work; reach for kSpeedNum only for a
// blanket "reclaim is too slow/fast in this mod, across every unit" adjustment.
//
// ---------------------------------------------------------------------------------------
// kAssistOn -- what multi-unit stacking is worth. Time on corsumo_dead, ON vs OFF:
// ---------------------------------------------------------------------------------------
//
//   crew                            assist ON   assist OFF   speedup
//   1x corck                          16.27s      16.27s      1.00x
//   2x corck                           8.13s      16.27s      2.00x
//   4x corck                           4.07s      16.27s      4.00x
//   8x corca (WT 60)                   3.00s      24.47s      8.16x
//   1x corcom                          4.07s       4.07s      1.00x
//   1x corcom + 1x corck               3.20s       4.07s      1.27x
//   1x corcom + 4x corck               2.00s       4.07s      2.03x
//   20x corca (late-game air swarm)    1.20s      24.47s     20.39x
//
// Contribution is exactly proportional to Build Power (the mixed-crew rows add real weight,
// not a flat +1 per helper). "assist OFF" is vanilla's race: only the fastest unit's time
// matters and only it gets the metal. kVanillaSpeedUnits (below) NEVER stack, regardless of
// this flag -- see "SCAVENGE UNITS" below.
//
// ---------------------------------------------------------------------------------------
// kBeamMin -- PURELY VISUAL. Never touches the counter, the speed, or the energy charged.
// 0 (default) = beam visible for the entire reclaim. 15 ground / 30 VTOL restores vanilla's
// cutoff, where the beam vanishes for the last second or two of every reclaim and never
// appears at all on fast ones.
//
// kAssistStale -- resume-after-cancel window and slot expiry, in ticks (30/sec):
//     2 ticks = 0.07s (1 callback)      16 ticks = 0.53s (8 callbacks)
//     4 ticks = 0.13s (2 callbacks)     30 ticks = 1.00s (15 callbacks)
//     8 ticks = 0.27s (4 callbacks) *   60 ticks = 2.00s (30 callbacks)
// Longer = more forgiving of a unit being briefly interrupted or re-tasked, at the cost of
// widening the window where a same-species feature respawn could inherit stale progress
// (bounded: different feature types are always caught by the featDefIdx check regardless of
// this value).
//
// ---------------------------------------------------------------------------------------
// SCAVENGE UNITS -- kVanillaSpeedUnits
// ---------------------------------------------------------------------------------------
// armflea, corsc, armnh, cornh are all VERIFIED WorkerTime 30 (parsed directly from the
// Escalation HPI archives). Matched by unit-def NAME, not by editing their FBI stats, so
// their Build Power is untouched for every other purpose (building, repairing, being
// repaired). For these four, and only these four:
//
//   - D = kSpeedNum (2 at defaults) ALWAYS, regardless of Build Power -- vanilla speed.
//   - They STILL PAY ENERGY at the normal per-work-unit rate, so a feature's total cost
//     (floor(C * kEnergyNum / kEnergyDen)) is unaffected by who reclaims it, scavengers
//     included.
//   - They NEVER stack through the assist table, even with each other, even with
//     kAssistOn=true. Two fleas on one wreck race exactly as vanilla: only the first to
//     finish gets the metal.
//
// Net effect versus leaving them under the normal formula: at defaults a WorkerTime-30 unit
// would get D=0.67 (three times SLOWER than vanilla, and see THE BEAM BUG above for what that
// does to the beam). Pinning restores vanilla feel for the unit CLASS this exception exists
// for, while every other constructor still gets the full scaling and assist behaviour.
namespace ReclaimAssistTuning {

    constexpr int  kMode        = 0;    // 0 = LINEAR (EBP = WorkerTime), 1 = SQRT
    constexpr int  kSpeedNum    = 2;    // global speed multiplier; 2 == vanilla's decrement
    constexpr int  kSpeedDen    = 90;   // the WorkerTime that reproduces vanilla EXACTLY
    constexpr int  kEnergyNum   = 3;    // energy per work unit, numerator
    constexpr int  kEnergyDen   = 4;    //   3/4 = 0.75 -> drain == 25% of Build Power/sec
    constexpr bool kEnergyOn    = true; // false = speed scaling only, reclaim free as vanilla
    constexpr int  kBeamMin     = 0;    // beam draws while counter > this; vanilla was 15/30
    constexpr bool kAssistOn    = true; // multi-unit stacking (scavenge units excluded always)
    constexpr int  kAssistStale = 8;    // ticks a slot survives without a fresh contribution

    constexpr int  kSlotsPerPlayer = 256;  // concurrent DISTINCT reclaim jobs, per player --
                                            // NOT per unit. Ten units on one wreck cost one
                                            // slot; twenty units on twenty different wrecks
                                            // cost twenty. See the performance note in the
                                            // module's .cpp for why this can be generous.
    constexpr int  kProbeWindow    = 8;    // hash-probe depth per lookup; exceeded -> that
                                            // unit falls back to scaled-solo (still gets
                                            // Build Power speed and pays energy, just does
                                            // not pool with anyone). Progress is NEVER lost
                                            // on exhaustion, only stacking.

    // Units pinned to the vanilla decrement regardless of Build Power. Matched by
    // UnitDefStruct::Name (case-insensitive), NOT by unit stats -- add or remove entries
    // freely without touching any .fbi file. Empty the array to disable the exception
    // entirely (every unit then uses the normal Build-Power-scaled formula).
    constexpr const char* kVanillaSpeedUnits[] = { "armflea", "corsc", "armnh", "cornh" };

} // namespace ReclaimAssistTuning
