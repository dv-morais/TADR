# Reclaim Assist — Player's Guide

**What changes when your mod turns Reclaim Assist on, why, and how to play around it.**

This is written for players. You do not need to read any code to understand it. Mod owners
looking for the knobs will find them in section 10 at the bottom.

The feature is **off by default in every mod**. If your mod has not enabled it, reclaim works
exactly the way it always has and nothing here applies to you.

---

## 1. The short version

| | Normal TA | With Reclaim Assist |
|---|---|---|
| **How fast does a unit reclaim?** | Same speed for everyone — a Commander is no faster than the cheapest builder | Scales with the unit's **Build Power**, exactly like building does |
| **What does it cost?** | Nothing. Reclaim is free | Energy, based on the size of the wreck |
| **Do extra units help?** | No. They race, and only the winner gets the metal — everyone else's work is thrown away | Yes. They pool their Build Power, like assisting a build |
| **Who gets the metal?** | Whoever finishes first | Unchanged — still whoever finishes first |

Everything else about reclaim is untouched.

---

## 2. What was wrong with normal reclaim

In unmodified Total Annihilation, reclaiming a wreck is the **worst-scaling mechanic in the
game**, and it works nothing like the rest of it.

Everywhere else, Build Power means something. A Commander builds faster than a Construction
Kbot. Ten builders on one factory finish it faster than one. That is the core economic loop of
the game.

Reclaim ignores all of it:

- **A Commander reclaims a wreck at exactly the same speed as the cheapest construction unit
  in the game.** Build Power is not consulted at all.
- **It is completely free.** No metal, no energy.
- **Extra units do nothing.** Send ten constructors to one big wreck and they do not cooperate.
  They each run a private, invisible countdown from scratch. The first one to finish takes the
  whole payout and the other nine wasted their time entirely.

That last point is the one that catches people out. Piling units onto a wreck *feels* like it
should help, the way it does with building. It does not. It never has.

**Why it is like that:** wrecks and rocks are not units. They have no per-instance memory in
the engine — a map cell is 13 bytes with nowhere to record "this is 40% reclaimed". Progress
lives inside each *unit's own order*, privately, which is why it cannot be shared. The engine
was built this way in 1997 and it has never been changed.

Reclaim Assist gives wrecks a shared progress counter so that multiple units can work on the
same one, and makes speed and cost behave like the rest of the game.

---

## 3. How fast will my unit reclaim?

Reclaim speed now scales with **Build Power** — the same stat that decides how fast a unit
builds and repairs. Nothing about your units changed; the mechanic just started paying
attention to a stat that was already there.

### The reference point

**A unit with Build Power 90 reclaims at exactly the same speed as it always did.**

That is the calibration anchor. Build Power 90 is the standard Construction Kbot
(`armck` / `corck`). If your mod has not changed that setting, then:

- **Build Power 90** → identical to old behaviour
- **Above 90** → faster than before, proportionally
- **Below 90** → slower than before, proportionally

A Commander has Build Power 360 — four times 90 — so it reclaims **four times faster**. That
is the whole rule.

### Real numbers

Time to clear a **Sumo wreck** (`corsumo_dead`), which is the big, obvious wreck most people
picture. In normal TA every one of these units takes **16.27 seconds**.

| Unit | Build Power | Time | vs normal TA |
|---|---|---|---|
| `armflea` (scavenger) | 30 | 16.27 s | same — *see section 6* |
| `corca` construction aircraft | 60 | 24.47 s | slower |
| `corck` construction kbot | 90 | **16.27 s** | **identical — the reference point** |
| `coraca` adv. construction aircraft | 150 | 9.73 s | 1.7× faster |
| `corcom` Commander | 360 | 4.07 s | **4× faster** |
| `coreca` adv. construction aircraft | 450 | 3.20 s | 5× faster |
| high-tier constructor | 6480 | 0.20 s | ~80× faster |

So a Commander now clears a big wreck in about four seconds instead of sixteen. A cheap
construction aircraft is *slower* than it used to be, because 60 is below the reference point
of 90.

### Other wrecks

Different wrecks take different amounts of work. A wreck's "size" for reclaim purposes comes
from the metal and energy it holds — bigger payout, longer job. Roughly:

| Wreck / feature | Metal | Time for a Commander | Time for a construction kbot |
|---|---|---|---|
| small rock | 24 | 0.3 s | 0.9 s |
| `armrock_dead` | 62 | 0.4 s | 1.5 s |
| tree cluster | 250 | 1.2 s | 4.7 s |
| `corsumo_heap` | 474 | 2.1 s | 8.4 s |
| `corsumo_dead` | 950 | 4.1 s | 16.3 s |
| `armwalk_heap` (huge) | 17340 | 72.4 s | 289.5 s |

**To estimate any wreck yourself:** hover it with a constructor selected and read its Metal
value off the tooltip. For a reasonably large wreck, time in seconds is about `Metal ÷ 240`
for a Commander and `Metal ÷ 60` for a standard construction kbot.

Those shortcuts only hold for big wrecks. Every reclaim job carries a small fixed overhead, so
small targets take proportionally longer than the division suggests — a 24-metal rock works out
nearer `Metal ÷ 90` for a Commander. It stops mattering above a few hundred metal.

---

## 4. What does it cost?

Reclaiming now costs **energy**. Metal income from reclaim is unchanged — you still get the
full payout.

### The one rule worth remembering

**A wreck costs the same total energy no matter who reclaims it or how fast.**

Your Commander clearing a Sumo wreck in 4 seconds pays exactly the same as a construction kbot
grinding through it over 16 seconds: **367 energy**. Speed changes how *hard* the drain hits,
never how much you pay in total.

| Wreck | Metal you get | Energy it costs |
|---|---|---|
| small rock | 24 | 20 |
| `armrock_dead` | 62 | 34 |
| tree cluster | 250 | 105 |
| `corsumo_heap` | 474 | 189 |
| `corsumo_dead` | 950 | **367** |
| `armwalk_heap` | 17340 | 6513 |

At default settings you are paying roughly **0.39 energy per metal recovered**. Reclaim is
still strongly profitable — it is just no longer literally free.

### The drain while it works

**A reclaiming unit drains energy equal to 25% of its Build Power, per second.**

| Unit | Build Power | Energy/sec while reclaiming |
|---|---|---|
| `armflea` | 30 | 7.5 |
| `corca` | 60 | 15 |
| `corck` | 90 | 22.5 |
| `corcom` Commander | 360 | **90** |
| `coreca` | 450 | 112.5 |
| high-tier constructor | 6480 | 1620 |

This is the practical thing to plan around. A Commander reclaiming pulls 90 energy per second
— comparable to running a serious production line. **Ten construction aircraft reclaiming at
once pull 150 energy/second between them.** A big reclaim operation is now a real load on your
grid, and you need the generation to support it.

The trade-off is deliberate: faster reclaim costs the same total, but it *concentrates* that
cost into a shorter, sharper spike. Reclaiming fast is an energy-economy decision, not a free
action.

---

## 5. Does piling on more units help now?

**Yes.** This is the biggest change to how you actually play.

Units reclaiming the same wreck now pool their Build Power into one shared job, exactly like
assisting a construction. Their contributions add up proportionally — a Commander plus a
construction kbot contributes Commander-plus-kbot, not "Commander plus a token helper".

Time to clear a Sumo wreck:

| What you send | With Reclaim Assist | Normal TA | Speedup |
|---|---|---|---|
| 1× construction kbot | 16.27 s | 16.27 s | — |
| 2× construction kbot | **8.13 s** | 16.27 s | **2×** |
| 4× construction kbot | **4.07 s** | 16.27 s | **4×** |
| 8× construction aircraft | **3.00 s** | 24.47 s | **8.2×** |
| 1× Commander | 4.07 s | 4.07 s | — |
| 1× Commander + 1× kbot | 3.20 s | 4.07 s | 1.27× |
| 1× Commander + 4× kbot | 2.00 s | 4.07 s | 2.03× |
| 20× construction aircraft | **1.20 s** | 24.47 s | **20×** |

That last row is the headline: a late-game air constructor swarm can strip a big wreck in
about a second. In normal TA those same twenty aircraft would take 24 seconds and nineteen of
them would have wasted the entire trip.

### Things worth knowing about stacking

- **Only your own units pool.** Allies and enemies reclaiming the same wreck do not combine
  with you and cannot inherit your progress. If an enemy is racing you for a wreck, that is
  still a straight race exactly like normal TA — whoever finishes first takes it.
- **Nobody can steal your progress.** Your work is tracked separately from every other
  player's, always.
- **The energy total does not change when you stack.** Four units clearing a wreck 4× faster
  pay the same total as one unit doing it slowly — they just pay it four times as fast.
  Watch your energy bar.
- **Cancelling and re-issuing quickly keeps your progress.** If a unit gets interrupted and
  you re-task it onto the same wreck within about a quarter of a second, it picks up where it
  left off. Longer than that and the job resets.

---

## 6. Scavenger units are deliberately exempt

**`armflea`, `corsc`, `armnh`, `cornh` reclaim at normal TA speed, always.**

These four are the scavenger units — cheap, disposable, and built specifically to go pick
things up. They all have Build Power 30, which is well below the reference point of 90. Under
the normal rule they would reclaim **three times slower than they used to**, which would gut
the one thing they exist to do.

So they are pinned to the old speed. Specifically:

| | Scavenger behaviour |
|---|---|
| **Speed** | Normal TA speed, always. Build Power is ignored for them. |
| **Energy** | They **still pay**, at the normal rate. A wreck costs the same no matter who clears it. |
| **Stacking** | They **never** pool — not with each other, not with anything. Two fleas on one wreck race exactly like normal TA, and only the winner gets the metal. |

**Why they still pay and still cannot stack:** the dangerous combination would have been
*free* **and** *stacking* — spam fifty of the cheapest unit in the game and strip the map at no
cost. Neither half is available. Mass-scavenger reclaim gains nothing over normal TA, and a
scavenger alongside your Commander neither speeds it up nor gets dragged along by it.

Their Build Power is untouched for everything else — building, repairing, being repaired all
work exactly as before. This is a reclaim-only exception.

---

## 7. What happens when you run out of energy

If your energy runs dry mid-reclaim, the unit **stops**.

- The nano beam goes dark.
- The wreck stops shrinking.
- No metal comes in.
- No progress is lost — the moment you have energy again, it resumes from where it stopped.

A unit that cannot pay contributes nothing. It also cannot free-ride on an ally's paid-for
progress, so a broke player parking constructors on someone else's reclaim job gains nothing.

**Practical consequence:** during a hard energy stall, reclaim stops being an emergency metal
source. Plan for that, especially if you are used to reclaiming your way out of trouble.

---

## 8. The nano beam looks different

Two visual changes, neither of which affects the simulation:

**The beam now stays on for the whole job.** In normal TA the beam cuts out for the last second
or two of every reclaim, and on small targets — a rock, a tree — it often never appears at all,
because the job finishes before the beam's display threshold is reached. Reclaiming a small
rock with a powerful constructor showed *no beam whatsoever*. Now it is visible start to
finish.

**The beam goes dark when you cannot pay.** That is your visual cue that a unit is stalled on
energy rather than working. See section 7.

If your mod owner prefers the classic look, the old cutoff can be restored — it is purely
cosmetic either way.

---

## 9. What did NOT change

Worth stating plainly, because it is easy to assume more changed than did:

- **Metal payout is identical.** You get exactly the same metal from every wreck as before.
- **Payout is still all-or-nothing at the end.** You do not get partial metal for partial
  reclaim, and you never did.
- **Whoever finishes first still gets everything.** Racing an enemy for a wreck works exactly
  as before.
- **Unit stats are untouched.** No `.fbi` file was modified. Build Power, cost, health, build
  speed — all unchanged. The mechanic simply started reading a stat that was already there.
- **Reclaiming units (not wrecks) is unchanged.** This only affects wrecks, rocks, trees and
  other map features.
- **Resurrect is unchanged.**
- **Reclaim-on-patrol works normally** and follows all the same rules — same speed scaling,
  same energy cost. It is not a loophole.

---

## 10. For mod owners — the settings

Everything above describes **default settings**. All of it is adjustable. The values live in
`src/DDraw/ReclaimAssist.h`; the on/off switch lives in your mod's config file:

```c
// src/DDraw/config_<yourmod>.h
#define RECLAIM_ASSIST_ENABLE 1     // 0 in every shipped config
```

| Setting | Default | What it does |
|---|---|---|
| `kSpeedDen` | `90` | **The master dial.** The Build Power that reclaims at exactly normal-TA speed. Raise it and everyone slows down; the ratios between units never change. |
| `kMode` | `0` | `0` = speed scales linearly with Build Power. `1` = square-root scaling, which compresses the gap between weakest and strongest constructor from 216× down to about 26×. Use it if high-tier constructors trivialising every wreck is your problem. |
| `kSpeedNum` | `2` | Global speed multiplier. **Also sets scavenger speed** — see the warning below. |
| `kEnergyOn` | `true` | `false` makes reclaim free again while keeping speed scaling and stacking. The gentlest way to introduce this feature. |
| `kEnergyNum` / `kEnergyDen` | `3`/`4` | Energy per unit of work. `3/4` gives the "25% of Build Power per second" drain. |
| `kAssistOn` | `true` | Multi-unit stacking on/off. |
| `kAssistStale` | `8` | Ticks (30 = 1 second) a job survives without progress before resetting. Controls the resume-after-cancel window. |
| `kBeamMin` | `0` | **Cosmetic only.** `15` restores normal TA's beam cutoff. |
| `kSlotsPerPlayer` | `256` | Concurrent *distinct* reclaim jobs per player. Note this is per **wreck**, not per unit — twenty units on one wreck use one slot. |
| `kProbeWindow` | `8` | Collision tolerance. If exceeded, that unit reclaims solo at full speed instead of pooling — it never loses progress. |
| `kVanillaSpeedUnits` | 4 names | The scavenger exemption list, matched by unit name. Add or remove freely; no `.fbi` edits needed. Empty it to remove the exemption entirely. |

### Three things that will surprise you

1. **`kSpeedNum` does double duty.** It is the global speed multiplier *and* the flat reclaim
   rate for scavengers. Setting it to `4` does not just double everyone's speed — it also makes
   every scavenger reclaim at **twice** normal TA speed. The default of `2` works precisely
   because it equals the engine's own flat rate. **Tune `kSpeedDen` instead** for normal
   balance work.
2. **Total energy per wreck is independent of every speed setting.** Changing `kMode`,
   `kSpeedNum`, `kSpeedDen` or `kAssistOn` changes how fast the energy drains, never how much.
3. **`kSlotsPerPlayer` counts wrecks, not units.** Twenty air constructors on one wreck cost
   one slot. Twenty constructors on twenty different trees cost twenty. Capacity is about how
   many *separate* jobs you run at once.

### Speed reference

Seconds to clear `corsumo_dead` at different `kSpeedDen` values. The **bold** diagonal is the
reference point for that column — whatever Build Power equals `kSpeedDen` reclaims at normal
TA speed.

| Build Power | den=30 | den=60 | **den=90** | den=120 | den=180 | den=360 |
|---|---|---|---|---|---|---|
| 30 | **16.27** | 32.60 | 48.93 | 65.27 | 97.93 | 195.93 |
| 60 | 8.13 | **16.27** | 24.47 | 32.60 | 48.93 | 97.93 |
| 90 | 5.40 | 10.87 | **16.27** | 21.73 | 32.60 | 65.27 |
| 120 | 4.07 | 8.13 | 12.20 | **16.27** | 24.47 | 48.93 |
| 150 | 3.20 | 6.47 | 9.73 | 13.00 | 19.53 | 39.13 |
| 180 | 2.67 | 5.40 | 8.13 | 10.87 | **16.27** | 32.60 |
| 360 | 1.33 | 2.67 | 4.07 | 5.40 | 8.13 | **16.27** |
| 450 | 1.07 | 2.13 | 3.20 | 4.33 | 6.47 | 13.00 |
| 6480 | 0.07 | 0.13 | 0.20 | 0.27 | 0.40 | 0.87 |
| 19200 | 0.00 | 0.00 | 0.07 | 0.07 | 0.13 | 0.27 |

### Energy reference

Total energy per wreck at different `kEnergyNum`/`kEnergyDen` ratios:

| Wreck | OFF | 1/4 | 1/2 | **3/4** | 1/1 | 3/2 |
|---|---|---|---|---|---|---|
| small rock | 0 | 6 | 13 | **20** | 27 | 40 |
| `armrock_dead` | 0 | 11 | 23 | **34** | 46 | 69 |
| tree cluster | 0 | 35 | 70 | **105** | 140 | 210 |
| `corsumo_heap` | 0 | 63 | 126 | **189** | 252 | 378 |
| `corsumo_dead` | 0 | 122 | 245 | **367** | 490 | 735 |
| `armwalk_heap` | 0 | 2171 | 4342 | **6513** | 8685 | 13027 |

After changing anything, rebuild:

```
msbuild src/DDraw/ddraw.vcxproj /p:Configuration=ReleasePublic /p:Platform=x86 \
        /p:TDRAW_CONFIG=TDRAW_CONFIG_ESCALATION
```

**There is no ini file and no in-game settings menu, deliberately.** TA is lockstep
multiplayer: every player's game simulates every tick independently and they must agree
exactly. A settings file players could edit would desync games. Retuning requires a rebuild.

---

## 11. Multiplayer

**Every player in a match must run the same build.** This changes how the simulation behaves,
so a player running a different build — or the same build with different settings — will
desync the moment anyone reclaims anything.

Old recorded demos will also desync against a patched build. That is expected and unavoidable
for any change of this kind.

---

## 12. How well is this tested?

Verified in-game against numeric predictions made before testing:

| Check | Expected | Result |
|---|---|---|
| Build Power 90 matches normal TA speed | 488 ticks | 486 — pass, within one update |
| Commander is 4× faster | 122 ticks | pass |
| Energy drain, Commander | 90/sec | 90/sec — pass |
| Total cost is speed-independent | 367 either way | pass |
| Two units stack to 2× | 244 ticks | pass |
| Mixed crew adds proportionally | 96 ticks | pass |
| Enemies cannot pool or steal progress | no stacking | pass |
| Energy stall halts reclaim cleanly | beam off, no progress | pass |
| Beam visible on small targets | visible throughout | pass |
| Scavengers keep normal speed, no flicker | normal speed | pass |
| Resume after cancel | resumes / resets by timing | pass |
| Patrol-reclaim follows the same rules | no loophole | pass |

**Known imprecision:** the Build Power 90 reference measured 486 game ticks against a
predicted 488 — a 0.4% difference, exactly one update period. A single hand-timed measurement
cannot separate observer reaction time from a one-update rounding edge in the prediction.
Gameplay impact is nil.

**Not tested in-game:** the behaviour when a single player exceeds 256 simultaneous *distinct*
reclaim jobs. That is not reachable by hand in a normal match. The fallback (that unit reclaims
solo at full speed instead of pooling, and never loses progress) is implemented and reviewed
but rests on code inspection rather than measurement.

---

## 13. More detail

| File | What |
|---|---|
| `ReclaimAssist.h` | The settings themselves, with inline notes |
| `ReclaimAssist.cpp` | Implementation |
| `ENGINE_NOTES.md` | Engine internals, determinism rules |
| `ai-reference/TA-previous-files/TA_PROJECT4_RECLAIM_FINDINGS.md` | Full reverse-engineering record for how reclaim works |
| `ESCALATION_SHARE_GUARD_DESIGN.md` | Another opt-in, config-gated balance module |
