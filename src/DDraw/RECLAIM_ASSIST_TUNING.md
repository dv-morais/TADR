# Reclaim Assist — Tuning Reference

Complete reference for every tunable in `ReclaimAssist.h`: what each value does, what it
influences, which other values it interacts with, and what it cannot do.

Companion to the inline guide in `ReclaimAssist.h`. That header is the authoritative source
for the *current* values; this document explains the *reasoning* so you can change them
deliberately.

> **Every number in this document was produced by emulating the exact integer algorithm the
> module runs, not by estimation.** Where a claim comes from measurement or from the
> disassembly rather than emulation, it says so.

---

## 1. Turning the feature on

The feature ships **off in all five configs**. It is a deliberate balance change, not a bug
fix — vanilla feature reclaim being free, flat-rate and non-stacking is known, shipped,
25-year-old behaviour.

```c
// src/DDraw/config_<yourmod>.h
#define RECLAIM_ASSIST_ENABLE 1
```

| Config file | Mod |
|---|---|
| `config_escalation.h` | Escalation |
| `config_bta.h` | Balanced Total Annihilation |
| `config_tazero.h` | TA Zero |
| `config_full.h` | Full feature set |
| `config_minimal.h` | Minimal |

Rebuild after any change — to the macro *or* to any number in this document:

```
msbuild src/DDraw/ddraw.vcxproj /p:Configuration=ReleasePublic /p:Platform=x86 \
        /p:TDRAW_CONFIG=TDRAW_CONFIG_ESCALATION
```

**There is no ini file and no in-game menu, deliberately.** TA is lockstep-deterministic:
every client simulates every tick independently and they must agree bit-for-bit. A
live-editable config is a desync generator. Retuning requires a rebuild, and **every player
in a match must run the identical build.**

---

## 2. What the module changes

Vanilla feature reclaim (ground handler `0x404AD0`, VTOL `0x414770`) decrements a counter by
a flat `-2` per callback, regardless of who is reclaiming, for free, and N units on one wreck
do not stack — they race, and only the winner gets the metal.

This module replaces the decrement with three coupled mechanics:

1. **Build-Power-scaled speed** via an exact-rational accumulator.
2. **An energy cost**, charged through the vanilla resource gate at `0x401180`.
3. **Multi-unit assist** — N workers subtract from one shared counter.

Plus a nano-beam visibility fix (the beam no longer strobes on low-Build-Power units).

### The numbers the engine gives us

A feature's *channel length* `C` — how much counter must be consumed to reclaim it — is set
by the engine and **this module does not change it**:

```
C_ground = 15 + ((Energy + Metal) >> 1)
C_vtol   = 30 + ((Energy + Metal) >> 1)
```

Callbacks fire every 2 ticks, i.e. **15 per second** (TA runs at 30 ticks/sec).

```
callbacks to finish = ceil(C / D)
seconds to finish   = ceil(C / D) / 15
```

Reference features used throughout this document:

| Feature | Metal | Energy | `C` |
|---|---|---|---|
| `dryrock03` | 24 | 0 | 27 |
| `armrock_dead` | 62 | 0 | 46 |
| `architree01` | 250 | 0 | 140 |
| `corsumo_heap` | 474 | 0 | 252 |
| `corsumo_dead` | 950 | 0 | **490** |
| `armwalk_heap` | 17340 | 0 | 8685 |

To find `C` for any feature in your mod: hover it with a constructor selected, read
Metal + Energy off the tooltip, and apply the formula above.

**Vanilla baseline for `corsumo_dead`: 245 callbacks = 16.27 seconds, free, non-stacking.**
That 16.27 s is what "vanilla" means in every table below.

---

## 3. The per-callback pipeline

Tuning decisions only make sense against the actual order of operations. Every reclaim
callback, for one unit, runs this:

```
  1.  WorkerTime = UnitDef.nWorkerTime          (the unit's Build Power)
      WorkerTime == 0  ->  D = kSpeedNum, done  (never-stall guard)

  2.  Scavenge check: is this unit in kVanillaSpeedUnits?
        yes -> D = kSpeedNum, accS = 0, SKIP the assist table entirely
        no  -> continue

  3.  EBP = kMode==0 ? WorkerTime
                     : isqrt(WorkerTime * kSpeedDen)      <- "effective Build Power"

  4.  t    = EBP * kSpeedNum + accS            <- accS carried from the previous callback
      D    = t / kSpeedDen                     <- integer division: this callback's decrement
      accS = t % kSpeedDen                     <- remainder carried to the next callback

  5.  ASSIST PASS 1 (search only, no mutation):
        found a live slot for this tile -> cap = slot.counter
        no slot                          -> cap = this order's own counter
      cap clamped to >= 0

  6.  work = min(D, cap)                       <- never bill for more than remains

  7.  e      = work * kEnergyNum + accE
      charge = e / kEnergyDen
      accE   = e % kEnergyDen

  8.  RESOURCE GATE (0x401180) if charge > 0
        FAIL -> nothing commits at all: counter unchanged, accumulators discarded,
                slot untouched, beam goes dark. The unit contributes nothing and
                cannot free-ride an ally's paid-for progress.
        PASS -> continue

  9.  COMMIT: pack accS/accE back into order+0x3A
             ASSIST PASS 2: claim a slot if pass 1 missed
             slot.counter -= D  (or the order's own counter if solo)
```

Two design points that matter for tuning:

- **The accumulator (`accS`) is why there are no dead zones.** A unit with
  `WorkerTime = 31` genuinely reclaims faster than one with `WorkerTime = 30`, even though
  both produce `D = 0` on most individual callbacks. The remainder carries. Do not "fix"
  a `D == 0` by flooring it at 1 — that collapses every low-Build-Power unit onto one speed
  and distorts every ratio above it.
- **The gate runs before any commit.** This ordering is load-bearing. Mutating an
  accumulator before knowing whether the charge succeeded silently duplicates or drops a
  fractional work unit on boundary-crossing ticks.

---

## 4. Parameter reference

### Summary

| Parameter | Default | Type | Category | Affects sim? |
|---|---|---|---|---|
| `kMode` | `0` | int (0/1) | Speed | Yes |
| `kSpeedNum` | `2` | int ≥ 1 | Speed | Yes |
| `kSpeedDen` | `90` | int 1–65535 | Speed | Yes |
| `kEnergyOn` | `true` | bool | Economy | Yes |
| `kEnergyNum` | `3` | int ≥ 0 | Economy | Yes |
| `kEnergyDen` | `4` | int 1–65535 | Economy | Yes |
| `kAssistOn` | `true` | bool | Stacking | Yes |
| `kAssistStale` | `8` | int (ticks) | Stacking | Yes |
| `kSlotsPerPlayer` | `256` | power of 2 | Capacity | Yes |
| `kProbeWindow` | `8` | int 1–slots | Capacity | Yes |
| `kBeamMin` | `0` | int | **Visual only** | **No** |
| `kVanillaSpeedUnits` | 4 names | string list | Exception | Yes |

---

### `kSpeedDen` — the master dial

**Default `90`. Valid `1`–`65535`.**

The denominator of the speed formula: `D = EBP * kSpeedNum / kSpeedDen`.

**The single most important property: `kSpeedDen` is the calibration anchor.** Whatever
WorkerTime you set it to reclaims at *exactly* vanilla speed. At the default of `90`, a
`corck` (WorkerTime 90) takes exactly as long as it does in unmodded TA. Everything faster
than WT 90 beats vanilla; everything slower loses to it.

Seconds to clear `corsumo_dead`, LINEAR mode. **Bold** = the anchor for that column:

| WorkerTime | den=30 | den=60 | **den=90** | den=120 | den=180 | den=360 |
|---|---|---|---|---|---|---|
| 30 `armflea` | **16.27** | 32.60 | 48.93 | 65.27 | 97.93 | 195.93 |
| 60 `corca` | 8.13 | **16.27** | 24.47 | 32.60 | 48.93 | 97.93 |
| 90 `corck` | 5.40 | 10.87 | **16.27** | 21.73 | 32.60 | 65.27 |
| 120 | 4.07 | 8.13 | 12.20 | **16.27** | 24.47 | 48.93 |
| 150 `coraca` | 3.20 | 6.47 | 9.73 | 13.00 | 19.53 | 39.13 |
| 180 | 2.67 | 5.40 | 8.13 | 10.87 | **16.27** | 32.60 |
| 360 `corcom` | 1.33 | 2.67 | 4.07 | 5.40 | 8.13 | **16.27** |
| 450 `coreca` | 1.07 | 2.13 | 3.20 | 4.33 | 6.47 | 13.00 |
| 6480 `coruck` | 0.07 | 0.13 | 0.20 | 0.27 | 0.40 | 0.87 |
| 19200 `corulab` | 0.00 | 0.00 | 0.07 | 0.07 | 0.13 | 0.27 |

**Raising it slows everyone down uniformly. Ratios never change in LINEAR mode** — a WT-360
unit is always exactly 4× a WT-90 unit regardless of this value. This dial moves the whole
curve without distorting its shape.

**Interacts with:**
- `kSpeedNum` — only the *ratio* `kSpeedNum / kSpeedDen` sets speed. `2/90` and `4/180` are
  identical in speed terms (but see the `kSpeedNum` warning about scavengers).
- `kMode` — in SQRT mode `kSpeedDen` doubles as the sqrt constant, which is exactly why both
  modes agree at the anchor.
- **The `D == 0` band.** `D` can be 0 on some callbacks whenever
  `WorkerTime < kSpeedDen / kSpeedNum` (45 at defaults). Raising `kSpeedDen` to 180 moves
  that threshold to WT 90, which pulls `armflea`, `corca`, `armca`, `armck` and `corck` into
  the band. **This is harmless** — the accumulator handles it correctly and the beam-strobe
  bug that used to accompany it is fixed in this module — but it is worth knowing the
  band exists and moves.

**Gotcha:** must stay under 65536. The speed accumulator is packed into the low 16 bits of
`order+0x3A`; a larger denominator cannot round-trip. Enforced by `static_assert`.

---

### `kMode` — LINEAR vs SQRT

**Default `0` (LINEAR). Valid `0` or `1`.**

Selects how a unit's WorkerTime becomes *effective Build Power* (EBP), the quantity actually
fed to the decrement:

```
kMode = 0   LINEAR:  EBP = WorkerTime
kMode = 1   SQRT:    EBP = isqrt(WorkerTime * kSpeedDen)
```

At `kSpeedDen = 90`:

| WorkerTime | EBP linear | D linear | EBP sqrt | D sqrt | sqrt vs linear |
|---|---|---|---|---|---|
| 30 | 30 | 0.67 | 51 | 1.13 | **1.70× faster** |
| 60 | 60 | 1.33 | 73 | 1.62 | 1.22× faster |
| **90** | 90 | **2.00** | 90 | **2.00** | **1.00× — the anchor** |
| 180 | 180 | 4.00 | 127 | 2.82 | 0.71× |
| 360 | 360 | 8.00 | 180 | 4.00 | 0.50× |
| 900 | 900 | 20.00 | 284 | 6.31 | 0.32× |
| 1440 | 1440 | 32.00 | 360 | 8.00 | 0.25× |
| 6480 | 6480 | 144.00 | 763 | 16.96 | 0.12× |
| 19200 | 19200 | 426.67 | 1314 | 29.20 | **0.07×** |

**SQRT compresses the spread from 216× down to about 26×.** LINEAR's top end runs away: a
`corulab` is 640× an `armflea`. SQRT pulls the whole range into 1.13 … 29.20.

- **Pick LINEAR** to preserve your mod's real Build Power economy. A unit that costs 4× as
  much reclaims 4× as fast. Predictable, and consistent with how build assist already works.
- **Pick SQRT** if factory-tier constructors trivialising every wreck is the problem you are
  solving, or if you want early-game constructors to stay useful for reclaim into late game.

Neither is "correct" — they encode different balance philosophies.

**Interacts with:** `kSpeedDen` (used as the sqrt constant, which pins both modes to the same
anchor), and only weakly with everything else. Switching modes never changes total energy
cost per feature — only how long the drain lasts.

---

### `kSpeedNum` — global speed multiplier

**Default `2`. Valid ≥ 1.**

The numerator of `D = EBP * kSpeedNum / kSpeedDen`. Rescales every unit uniformly.

Time on `corsumo_dead`:

| | num=1 | **num=2** | num=3 | num=4 |
|---|---|---|---|---|
| `corck` (WT 90) | 32.60 s | **16.27 s** | 10.87 s | 8.13 s |
| `corcom` (WT 360) | 8.13 s | **4.07 s** | 2.67 s | 2.00 s |

**⚠ Non-obvious coupling — read this before changing it.** `kSpeedNum` is used in *two*
places:

1. As the speed numerator for normal units.
2. **As the flat decrement for `kVanillaSpeedUnits`** (`D = kSpeedNum` for scavengers).

The default `2` is deliberate: it is exactly vanilla's flat decrement, which is what makes
the scavenge exception land on true vanilla speed. **Setting `kSpeedNum = 4` does not just
double everyone's speed — it also makes every scavenger reclaim at 2× vanilla**, which is
probably not what you intended.

**Recommendation: tune `kSpeedDen` alone for normal balance work.** Reach for `kSpeedNum`
only for a blanket "reclaim is too slow/fast across this entire mod" adjustment, and when you
do, re-check what it did to your scavengers.

---

### `kEnergyOn` — energy cost master switch

**Default `true`.**

- `true` — reclaiming costs energy, charged per unit of work actually consumed.
- `false` — reclaim is **free**, exactly as vanilla, while **keeping** Build-Power speed
  scaling and assist stacking.

`kEnergyOn = false` is a genuinely useful middle ground: it gives you the "reclaim should
scale with Build Power and multiple units should stack" half of the change without touching
the economy at all. If you want to introduce this feature to a player base gradually, this is
the setting to start from.

When `false`, the resource gate is never called, so the brown-out behaviour (beam goes dark,
reclaim stalls) also never triggers.

---

### `kEnergyNum` / `kEnergyDen` — energy cost rate

**Defaults `3` / `4` (= 0.75 energy per unit of work). `kEnergyNum` ≥ 0,
`kEnergyDen` 1–65535.**

Total energy to reclaim a feature is `floor(C * kEnergyNum / kEnergyDen)` and — this is the
headline invariant — **it does not depend on who reclaims it or how fast.** A Commander
clearing a wreck in 4 seconds pays exactly what a construction kbot pays over 16 seconds.

**Total energy per feature:**

| Feature | `C` | OFF | 1/4 | 1/2 | **3/4** | 1/1 | 3/2 |
|---|---|---|---|---|---|---|---|
| `dryrock03` | 27 | 0 | 6 | 13 | **20** | 27 | 40 |
| `armrock_dead` | 46 | 0 | 11 | 23 | **34** | 46 | 69 |
| `architree01` | 140 | 0 | 35 | 70 | **105** | 140 | 210 |
| `corsumo_heap` | 252 | 0 | 63 | 126 | **189** | 252 | 378 |
| `corsumo_dead` | 490 | 0 | 122 | 245 | **367** | 490 | 735 |
| `armwalk_heap` | 8685 | 0 | 2171 | 4342 | **6513** | 8685 | 13027 |

**Steady drain while working (energy/sec, at `kSpeedDen = 90`):**

| Unit | WT | OFF | 1/4 | 1/2 | **3/4** | 1/1 | 3/2 |
|---|---|---|---|---|---|---|---|
| `armflea` | 30 | 0 | 2.5 | 5.0 | **7.5** | 10.0 | 15.0 |
| `corca` | 60 | 0 | 5.0 | 10.0 | **15.0** | 20.0 | 30.0 |
| `corck` | 90 | 0 | 7.5 | 15.0 | **22.5** | 30.0 | 45.0 |
| `corcom` | 360 | 0 | 30.0 | 60.0 | **90.0** | 120.0 | 180.0 |
| `coreca` | 450 | 0 | 37.5 | 75.0 | **112.5** | 150.0 | 225.0 |
| `coruck` | 6480 | 0 | 540 | 1080 | **1620** | 2160 | 3240 |

**At the default 3/4, drain is exactly 25% of that unit's Build Power per second.** That is a
useful mental model: a unit with 360 Build Power costs 90 e/s to run as a reclaimer, for as
long as it keeps working.

**Interacts with:**
- `kSpeedNum` / `kSpeedDen` / `kMode` — these change the *rate* of drain (faster reclaim
  compresses the same total into less time) but **never the total**. Speed and cost are
  orthogonal by construction.
- `kEnergyOn` — the master switch; these two are ignored when it is `false`.
- Nothing else.

**Gotcha:** `kEnergyDen` must stay under 65536 — it packs into the high 16 bits of
`order+0x3A`. Enforced by `static_assert`.

**Balance note:** with a high `kEnergyNum/kEnergyDen`, a large wreck can cost more energy than
its metal is worth to a player who is energy-starved but metal-rich. That is a legitimate
design choice (it makes reclaim an economic decision rather than free money), but it is worth
being deliberate about. `armwalk_heap` at 3/2 costs 13,027 energy.

---

### `kAssistOn` — multi-unit stacking

**Default `true`.**

When on, N units reclaiming the same feature subtract from **one shared counter**, so their
Build Power pools exactly the way build assist already does.

Time on `corsumo_dead`:

| Crew | assist ON | assist OFF | Speedup |
|---|---|---|---|
| 1× `corck` | 16.27 s | 16.27 s | 1.00× |
| 2× `corck` | 8.13 s | 16.27 s | **2.00×** |
| 4× `corck` | 4.07 s | 16.27 s | **4.00×** |
| 8× `corca` (WT 60) | 3.00 s | 24.47 s | **8.16×** |
| 1× `corcom` | 4.07 s | 4.07 s | 1.00× |
| 1× `corcom` + 1× `corck` | 3.20 s | 4.07 s | 1.27× |
| 1× `corcom` + 4× `corck` | 2.00 s | 4.07 s | 2.03× |
| 20× `corca` (late-game air swarm) | 1.20 s | 24.47 s | **20.39×** |

**Contribution is exactly proportional to Build Power** — the mixed-crew rows add each unit's
real weight, not a flat +1 per helper.

`kAssistOn = false` restores vanilla's race: N units each run their own private countdown,
the fastest wins, and everyone else's work is wasted.

**Stacking is strictly per-player.** The table is partitioned by owner (`Unit+0xFF`), so an
enemy or ally reclaiming the same wreck can never pool with you, evict your slot, or inherit
your progress. This is structural, not a heuristic — a different player's units never touch
your partition.

**`kVanillaSpeedUnits` never stack regardless of this flag** — see below.

---

### `kAssistStale` — slot expiry / resume window

**Default `8` ticks (0.27 s). Measured in ticks; 30 ticks = 1 second.**

How long an assist slot survives without a fresh contribution before it is considered dead
and reusable.

| Ticks | Seconds | Callback periods |
|---|---|---|
| 2 | 0.07 | 1 |
| 4 | 0.13 | 2 |
| **8** | **0.27** | **4** |
| 16 | 0.53 | 8 |
| 30 | 1.00 | 15 |
| 60 | 2.00 | 30 |

This value controls two visible behaviours at once:

1. **Resume-after-cancel.** Cancel a reclaim and re-issue it within the window and the unit
   picks up where it left off. Wait longer and it starts over.
2. **Slot lifetime.** Slots that stop receiving contributions expire and free up for other
   reclaim jobs.

**Longer = more forgiving** of a unit being briefly interrupted, re-tasked, or bumped out of
range. **Shorter = tighter**, at the cost of restarting progress more often.

**Trade-off to be aware of:** a longer window slightly widens the case where a feature of the
*same species* respawning on a just-reclaimed tile (Escalation's `reproduce` /
`reproducearea`) could inherit stale progress. Features of a *different* type are always
caught by the feature-def identity check regardless of this value, so the exposure is narrow
and bounded.

---

### `kBeamMin` — nano-beam visibility (**cosmetic only**)

**Default `0`. Purely visual.**

The beam draws while the counter is greater than this value. **It never touches the counter,
the speed, the energy charged, or anything else in the simulation.** It is safe to change
without any balance consideration.

| Value | Effect |
|---|---|
| **`0`** | Beam visible for the entire reclaim, including tiny features. |
| `15` / `30` | Vanilla's cutoff (15 ground, 30 VTOL) — beam vanishes for the last second or two of every reclaim, and never appears at all on small or fast reclaims. |

Vanilla's cutoff produces the odd effect that reclaiming a small rock with a powerful
constructor shows *no beam at all*, because the counter drops below 15 in a single callback.
The default of `0` fixes that.

**Separate from `kBeamMin`, and not tunable:** the beam also goes dark whenever the resource
gate fails, so a browned-out reclaimer is visibly idle. That is intentional feedback and is
tracked independently of the decrement — which is what fixes the 33% strobe present in the
Cheat Engine script this module was ported from.

---

### `kSlotsPerPlayer` — assist table capacity

**Default `256`. Must be a power of two.**

Maximum concurrent **distinct reclaim jobs** per player.

**The single most common misreading: this is not per unit.** A slot is per
*(feature tile, player)* pair.

- Twenty air constructors all reclaiming **one** big wreck → **1 slot**. That is the entire
  point of the shared counter.
- Twenty constructors area-reclaiming **twenty different trees** → **20 slots**.

So what consumes capacity is how many *separate* things you are reclaiming at once, not how
many units you own. A realistic heavy case is roughly 30 concurrent distinct targets per
player; 256 is about 8× that headroom.

Memory cost is `10 players × kSlotsPerPlayer × 16 bytes` — **40 KB at the default**. Doubling
to 512 costs 80 KB. This is not a meaningful constraint on any machine that runs TA.

**Lookup cost does not grow with this value** (the table is hash-indexed, not scanned), which
is precisely why it can be generous. See `kProbeWindow`.

---

### `kProbeWindow` — hash probe depth

**Default `8`. Valid `1` … `kSlotsPerPlayer`.**

How many hash buckets a lookup examines before giving up.

**What happens when it is exceeded:** that unit falls back to **scaled-solo**. It still gets
full Build-Power speed scaling and still pays energy — it just does not pool with anyone else
on that tile. **Progress is never lost**, only stacking. This is the deliberate safe fallback:
degrading to "fast but alone" is far less surprising to a player than a unit suddenly
dropping from 4× to 1× speed mid-reclaim.

Raising it makes the table tolerate denser collisions at slightly higher lookup cost; lowering
it makes lookups marginally cheaper but gives up stacking sooner under pressure. At 256 slots
with a realistic ~30 concurrent jobs, the default of 8 is not close to being stressed.

**Interacts with:** `kSlotsPerPlayer` (must not exceed it — enforced by `static_assert`), and
`kAssistOn` (both are dead weight when stacking is off).

---

### `kVanillaSpeedUnits` — the scavenge exception

**Default `{ "armflea", "corsc", "armnh", "cornh" }`.**

Units listed here are pinned to vanilla reclaim behaviour regardless of their Build Power.
All four defaults are WorkerTime 30, verified by parsing the Escalation HPI archives directly.

**Matched by unit-def name, case-insensitively — not by editing stats.** Their Build Power is
untouched for every other purpose: building, repairing, being repaired, and every other
mechanic behaves exactly as before. No `.fbi` edits required, and adding or removing entries
is a one-line change here.

For these units, and only these units:

| Behaviour | Rule |
|---|---|
| **Speed** | `D = kSpeedNum` always (2 at defaults) — flat vanilla rate, never Build-Power-scaled. `kMode` and `kSpeedDen` are ignored for them. |
| **Energy** | They **still pay** at the normal rate. A feature's total cost stays invariant no matter who reclaims it. |
| **Stacking** | They **never** participate in the assist table — not with each other, not with anyone, even with `kAssistOn = true`. Two fleas on one wreck race exactly as vanilla; only the first to finish gets the metal. |

**Why the exception exists:** under the normal formula at default tuning, a WorkerTime-30 unit
gets `D = 0.67` — **three times slower than vanilla**. For a unit class whose entire purpose is
scavenging, that inverts the design intent. Pinning restores the vanilla feel for exactly those
units while every other constructor keeps full scaling.

**Why they still pay energy and still cannot stack:** the dangerous combination would have
been *free* **and** *stacking* — N cheap scavengers pooling into an N× swarm at zero cost.
Neither half is present. Cost invariance holds, and mass-scavenger reclaim gains nothing over
vanilla.

**To disable the exception entirely**, empty the array. Every unit then uses the normal
Build-Power-scaled formula, including the scavengers (and they become 3× slower than vanilla
at default tuning).

**To add units**, just add names. There is no length limit and the per-callback cost is a
handful of integer compares.

---

## 5. Interaction map

Which knobs actually touch each other:

```
  kMode ─────────┐
  kSpeedNum ─────┼──> D (decrement/callback) ──> reclaim SPEED
  kSpeedDen ─────┘            │
        │                     │
        │                     └──> drain RATE (energy/sec)
        │                                  ▲
        │                                  │
        └──> D==0 band threshold           │
                                           │
  kEnergyOn ───┐                           │
  kEnergyNum ──┼──> charge/callback ───────┘
  kEnergyDen ──┘        │
                        └──> TOTAL energy per feature  (INDEPENDENT of speed)

  kAssistOn ──────┬──> stacking on/off
  kAssistStale ───┼──> slot expiry + resume-after-cancel window
  kSlotsPerPlayer─┼──> concurrent distinct jobs per player
  kProbeWindow ───┘──> collision tolerance (fallback = scaled-solo)

  kVanillaSpeedUnits ──> OVERRIDES kMode, kSpeedDen, kAssistOn for listed units
                         (but NOT kEnergyOn/kEnergyNum/kEnergyDen — they still pay)

  kBeamMin ──> nothing. Visual only.
```

**The three genuinely surprising couplings:**

1. **`kSpeedNum` also sets scavenger speed.** Changing it silently changes
   `kVanillaSpeedUnits` behaviour. Prefer `kSpeedDen` for speed tuning.
2. **Total energy is independent of every speed knob.** Changing `kMode`, `kSpeedNum`,
   `kSpeedDen` or `kAssistOn` changes how *fast* the energy drains, never how *much*.
3. **`kSpeedDen` moves the `D == 0` band.** Harmless mathematically, but it determines which
   units spend some callbacks contributing nothing (and carrying a remainder instead).

---

## 6. Tuning recipes

Worked starting points. All assume `RECLAIM_ASSIST_ENABLE 1`.

### "I want the speed and stacking, but not the economy change"

```c
kEnergyOn = false;
```

Reclaim stays free like vanilla. Build Power still scales speed, units still stack. The least
disruptive way to introduce the feature.

### "Reclaim is too strong in the late game"

Late-game constructors are the problem, not early ones — so compress the curve rather than
slowing everyone:

```c
kMode = 1;        // SQRT
kSpeedDen = 90;   // unchanged; the anchor holds at WT 90
```

`coruck` drops from 144.00 to 16.96 decrement per callback (0.12×) while `armflea` actually
*gains* (0.67 → 1.13). Early constructors stay relevant.

### "Reclaim is too fast across the board"

```c
kSpeedDen = 180;  // was 90
```

Everyone halves. Ratios preserved. The anchor moves to WT 180 — units at WT 90 now take twice
vanilla's time.

### "Make reclaim a real economic decision"

```c
kEnergyNum = 3;
kEnergyDen = 2;   // 1.5 energy per work unit
```

`corsumo_dead` costs 735 energy for 950 metal. Reclaiming while energy-stalled becomes
genuinely impossible rather than merely expensive.

### "Vanilla speed, but let units stack"

```c
kMode = 0;
kSpeedNum = 2;
kSpeedDen = 90;   // defaults
kEnergyOn = false;
kAssistOn = true;
```

A WT-90 constructor performs exactly as vanilla. The only change a player notices is that
piling more units on a wreck now actually helps.

### "Restore vanilla's beam behaviour"

```c
kBeamMin = 15;    // 15 ground / 30 VTOL was vanilla
```

Cosmetic only. Some players prefer the original look.

---

## 7. Hard constraints

Enforced at compile time — violate one and the build fails with a named message rather than
misbehaving at runtime:

```c
static_assert((kSlotsPerPlayer & (kSlotsPerPlayer - 1)) == 0);  // power of two
static_assert(kProbeWindow > 0 && kProbeWindow <= kSlotsPerPlayer);
static_assert(kSpeedDen  >= 1 && kSpeedDen  < 0x10000);         // 16-bit accumulator packing
static_assert(kEnergyDen >= 1 && kEnergyDen < 0x10000);         // 16-bit accumulator packing
static_assert(kSpeedNum  >= 1);
static_assert(kEnergyNum >= 0);
```

The two 16-bit limits come from packing both accumulator remainders into the single 32-bit
field at `order+0x3A` (speed remainder low, energy remainder high).

**Range headroom, for the paranoid:** at a modded `WorkerTime` of 65535 (the engine's maximum,
it is a `movzx` load), LINEAR mode yields `D = 1456` and `charge = 1092` per callback.
Intermediates peak at `131,159` (speed) and `5,898,150` (sqrt input). The largest feature in
Escalation is `corusub_dead` at `C = 32,780`. Everything stays comfortably inside int32 — no
overflow is reachable through any tuning this document describes.

---

## 8. Multiplayer and determinism

**Read this before shipping a tuned build.**

- TA is **lockstep deterministic**. Every client simulates independently and must agree
  bit-for-bit.
- This feature is a **uniform behaviour change**: identical on all clients, but different from
  an unpatched client.
- **Every player in a match must run the same build with the same values.** A mismatch in
  *any* number in this document will desync the moment someone issues a reclaim order.
- **Old demos will desync** against a patched build, by design.
- All arithmetic in the module is integer. The only int→float conversion happens at the
  resource-gate boundary and uses the engine's own `fild`/`fstp` idiom, so it is bit-identical
  to the original x87 codegen.
- The assist table is keyed on engine-packed tile coordinates, the engine-assigned owner byte,
  and the feature-def index. **No pointer is ever recorded, hashed, or compared across time** —
  a rule that exists because free-and-realloc aliasing makes stored pointers a desync source.

---

## 9. Verification status

The module's behaviour was verified in-game against falsifiable numeric predictions. Results:

| Check | Predicted | Measured | Status |
|---|---|---|---|
| Calibration anchor (1× `corck`, `corsumo_dead`) | 488 ticks | 486 ticks | Pass (within 1 callback) |
| 4× Build Power (Commander) | 122 ticks | — | Pass (qualitative) |
| Energy drain rate, Commander | −90 e/s | −90 e/s | Pass |
| Total energy invariant | 367, speed-independent | consistent | Pass |
| 2× `corck` stacking | 244 ticks, 2.0× | — | Pass (qualitative) |
| Mixed crew (`corck` + Commander) | 96 ticks | — | Pass (qualitative) |
| Cross-player isolation | no stacking | no stacking | Pass |
| Energy stall | beam off, counter frozen | confirmed | Pass |
| Beam on small feature | visible throughout | confirmed | Pass |
| Scavenge exception | vanilla speed, no strobe | confirmed | Pass |
| Resume after cancel | resumes / restarts by window | confirmed | Pass |
| Patrol-reclaim routing | must route through module | confirmed | Pass |

**Known imprecision, not blocking:** the calibration anchor measured 486 ticks against a
predicted 488 — a 0.41% difference, exactly one callback period. A single hand-timed
measurement cannot distinguish observer reaction time from a one-callback accounting edge in
the prediction. Gameplay impact is nil.

**Not verified:** assist-table exhaustion (≥257 concurrent distinct reclaim targets for a
single player) is not reachable by hand in a normal game and was not tested empirically. The
fallback path is implemented and reviewed but rests on code inspection, not measurement.

---

## 10. See also

| File | What |
|---|---|
| `ReclaimAssist.h` | The constants themselves, plus a condensed version of this guide |
| `ReclaimAssist.cpp` | Implementation, hook installation, assist table |
| `ENGINE_NOTES.md` | Engine struct offsets, hook framework semantics, determinism rules |
| `ai-reference/TA-previous-files/TA_PROJECT4_RECLAIM_FINDINGS.md` | Full reverse-engineering record for feature reclaim |
| `ESCALATION_SHARE_GUARD_DESIGN.md` | Precedent for an opt-in, config-gated balance module |
