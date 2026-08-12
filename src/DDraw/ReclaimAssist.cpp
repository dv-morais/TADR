#include "ReclaimAssist.h"
#include "tamem.h"
#include "hook/hook.h"
#include "iddrawsurface.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstddef>

using namespace ReclaimAssistTuning;

// ---- static_assert prerequisites checked once at build time --------------------------------
//
// sizeof(UnitStruct)==0x118 / sizeof(FeatureDefStruct)==0x100 / sizeof(UnitDefStruct)==0x249:
// all three strides VERIFIED [bin] in ENGINE_NOTES.md section 3/4/6. The three named-field
// offsets below were independently confirmed by disassembly during this port (see the plan's
// section 1.4 and ShareGuard.cpp's identical guard for cOwnerID).
static_assert(sizeof(UnitStruct)        == 0x118, "UnitStruct must be the engine's 0x118 stride");
static_assert(sizeof(FeatureDefStruct)  == 0x100, "FeatureDefStruct must be the engine's 0x100 stride");
static_assert(sizeof(UnitDefStruct)     == 0x249, "UnitDefStruct must be the engine's 0x249 stride");
static_assert(offsetof(UnitStruct, UnitType)   == 0x92,  "UnitStruct.UnitType");
static_assert(offsetof(UnitStruct, UnitID)     == 0xA6,  "UnitStruct.UnitID (unit TYPE index)");
static_assert(offsetof(UnitStruct, cOwnerID)   == 0xFF,  "UnitStruct.cOwnerID (player-array index)");
static_assert(offsetof(UnitDefStruct, Name)        == 0x000, "UnitDefStruct.Name");
static_assert(offsetof(UnitDefStruct, nWorkerTime) == 0x1FE, "UnitDefStruct.nWorkerTime");
static_assert(offsetof(TAdynmemStruct, UnitDef)    == 0x1439B, "TAdynmemStruct.UnitDef");
static_assert(offsetof(TAdynmemStruct, FeatureDef) == 0x1426F, "TAdynmemStruct.FeatureDef");
static_assert(offsetof(TAdynmemStruct, GameTime)   == 0x38A47, "TAdynmemStruct.GameTime");

namespace {

// =============================================================================================
// Splice sites. All four VERIFIED [bin] not to be branch targets (byte-pattern scan of every
// relative jcc/jmp/call form plus an absolute-dword scan of all five PE sections -- see the
// plan doc section 1.2). The tick sites are moved 6 bytes forward from the CE reference
// (0x404C7C/0x41495E) to the STORE that follows the load, per ENGINE_NOTES section 1 X-2:
// "choose the hook site so the replay is something you want re-run with your modified
// register -- usually the store, not the load." Hooking the load produces a silent no-op,
// because LAGGERJMP replays the displaced bytes AFTER the router returns and would reload
// [edi+0x36] over whatever the router wrote.
// =============================================================================================
constexpr DWORD kGroundTickAddr = 0x00404C82u;  // 8B C1 89 4F 36 = mov eax,ecx / mov [edi+0x36],ecx
constexpr DWORD kGroundTickLen  = 5u;
constexpr DWORD kVTOLTickAddr   = 0x00414964u;  // same 5 bytes
constexpr DWORD kVTOLTickLen    = 5u;

constexpr DWORD kGroundBeamAddr = 0x00404CB2u;  // 8B 47 36 83 F8 0F = mov eax,[edi+0x36]/cmp eax,0xF
constexpr DWORD kGroundBeamLen  = 6u;
constexpr DWORD kGroundBeamDrawTarget = 0x00404CBEu;
constexpr DWORD kGroundBeamSkipTarget = 0x00404D51u;

constexpr DWORD kVTOLBeamAddr   = 0x00414997u;  // 8B 4F 36 83 F9 1E = mov ecx,[edi+0x36]/cmp ecx,0x1E
constexpr DWORD kVTOLBeamLen    = 6u;
constexpr DWORD kVTOLBeamDrawTarget = 0x004149A3u;
constexpr DWORD kVTOLBeamSkipTarget = 0x00414A36u;

constexpr DWORD kGetFeatureTypeFromOrderAddr = 0x00421DA0u;
constexpr DWORD kResourceGateAddr            = 0x00401180u;
constexpr unsigned kTaMainPtrAddr             = 0x00511DE8u;

// order+0x36 ("BuildUnitID" in tamem.h -- ENGINE_NOTES section 5.2 documents that name as
// actively misleading; it is the reclaim countdown counter) and order+0x3A (undeclared in
// tamem.h; VERIFIED [bin] unused by either feature-reclaim handler -- zero [reg+0x3A] disp8
// encodings anywhere in 0x404AD0..0x404D90 or 0x414770..0x414A70 -- so it is safe storage for
// our two accumulators). Reached by raw offset rather than through UnitOrdersStruct's named
// field, matching the convention RepairRateFix.cpp uses for Unit+0xBC.
constexpr int kOrderCounterOff = 0x36;
constexpr int kOrderAccumOff   = 0x3A;

constexpr int kMaxPlayers = 10;  // TAdynmemStruct::Players[10], fixed engine limit

inline int32_t& OrderCounter(void* order)
{
    return *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(order) + kOrderCounterOff);
}
inline uint32_t& OrderAccum(void* order)
{
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(order) + kOrderAccumOff);
}

inline TAdynmemStruct* GetTA()
{
    return *reinterpret_cast<TAdynmemStruct**>(kTaMainPtrAddr);
}

// ---- vanilla calls ---------------------------------------------------------------------------

typedef unsigned short(__stdcall* _GetFeatureTypeFromOrder)(void* pos, uint32_t* outKey, uint32_t* outFootprint);
_GetFeatureTypeFromOrder GetFeatureTypeFromOrder = (_GetFeatureTypeFromOrder)kGetFeatureTypeFromOrderAddr;

// Resource gate call, using the engine's own fild/fstp int->float idiom (ENGINE_NOTES section
// 8.3), copied from the one vanilla caller at 0x41BDA4, rather than a plain static_cast<float>.
// This makes the conversion bit-identical to vanilla and sidesteps any x87-vs-SSE2 question
// entirely, even though static_cast<float> would in fact be provably exact here too: charge is
// bounded well under 2^24 for every legal (and every currently reachable modded) WorkerTime --
// see "X-10" in the plan doc. RepairRateFix.cpp already relies on that same exactness for a
// larger value range; the asm idiom here is belt-and-braces, not a response to a found bug.
int32_t ChargeEnergy(void* resourceBlock, int32_t charge)
{
    int result;
    __asm
    {
        fild dword ptr [charge]
        mov  ecx, resourceBlock
        push ecx
        fstp dword ptr [esp]
        push ecx
        mov  eax, kResourceGateAddr
        call eax                     ; __stdcall, ret 8, returns 1 = paid
        mov  result, eax
    }
    return result;
}

// ---- integer square root (SQRT mode) ----------------------------------------------------------
//
// Bit-by-bit integer sqrt, a direct port of reclaim_assist_v5.CEA's doIsqrt. VERIFIED against
// math.isqrt over 300,003 cases with zero mismatches before this hook was ever written.
int32_t IntSqrt(uint32_t x)
{
    uint32_t bit = 0x40000000u;
    while (bit > x)
        bit >>= 2;
    uint32_t root = 0;
    while (bit != 0)
    {
        uint32_t probe = root + bit;
        if (x >= probe)
        {
            x -= probe;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<int32_t>(root);
}

// ---- scavenge-unit exception (D6) --------------------------------------------------------------

constexpr int kVanillaSpeedCount =
    sizeof(kVanillaSpeedUnits) / sizeof(kVanillaSpeedUnits[0]);

int16_t        g_vanillaSpeedIds[kVanillaSpeedCount];
int            g_vanillaSpeedResolvedCount = 0;
UnitDefStruct* g_vanillaSpeedCachedBase    = nullptr;

// Resolved lazily (Install() runs at DLL_PROCESS_ATTACH, before the engine has loaded any
// unit defs) and re-checked cheaply on every call, same EnsureTable() shape RepairRateFix.cpp
// uses for its accumulator table.
void ResolveVanillaSpeedUnits(TAdynmemStruct* taPtr)
{
    if (!taPtr || !taPtr->UnitDef || taPtr->UNITINFOCount == 0)
        return;
    if (g_vanillaSpeedCachedBase == taPtr->UnitDef)
        return;

    g_vanillaSpeedResolvedCount = 0;
    for (unsigned i = 0; i < taPtr->UNITINFOCount && g_vanillaSpeedResolvedCount < kVanillaSpeedCount; ++i)
    {
        const char* name = taPtr->UnitDef[i].Name;
        for (int k = 0; k < kVanillaSpeedCount; ++k)
        {
            if (_stricmp(name, kVanillaSpeedUnits[k]) == 0)
            {
                g_vanillaSpeedIds[g_vanillaSpeedResolvedCount++] = static_cast<int16_t>(i);
                break;
            }
        }
    }
    g_vanillaSpeedCachedBase = taPtr->UnitDef;
}

bool IsVanillaSpeedUnit(int16_t unitId)
{
    for (int i = 0; i < g_vanillaSpeedResolvedCount; ++i)
        if (g_vanillaSpeedIds[i] == unitId)
            return true;
    return false;
}

// ---- assist table: partitioned per player, hash-indexed (D7) -----------------------------------
//
// Owner is the PARTITION INDEX, not a stored field -- Unit+0xFF (cOwnerID, VERIFIED [bin] to be
// the player-array index, see the plan doc section 1.4) selects which player's 256 slots a unit
// can ever touch. This makes cross-player interference structurally impossible: an enemy cannot
// occupy, evict, or collide with a slot in your partition because their lookups never address
// it. Closes E-1/E-5 from TA_PROJECT4_RECLAIM_FINDINGS.md by construction, not by staleness
// heuristics, and closes the shared-table bucket-collision griefing vector a single shared
// table would otherwise expose.
//
// Lookup is a hash probe, not a linear scan, so kSlotsPerPlayer can be generous without paying
// for it every callback -- see the plan doc section 7 for the measured cost difference (a
// 256-slot linear scan is roughly 4x this design's total per-callback cost, and that gap widens
// linearly with slot count). Hashing the packed TILE KEY is compatible with CLAUDE.md rule 5:
// that rule forbids hashing or doing arithmetic on a POINTER VALUE to pick a table slot; the
// tile key is engine-packed simulation state ((z<<16)|(x&0xFFFF) from GetFeatureTypeFromOrder),
// identical on every client by construction, not a pointer.
struct Slot
{
    uint32_t key;         // packed root tile key BIASED BY +1; a stored 0 means "empty slot".
                          // The bias exists because map tile (0,0) packs to a legitimate 0 --
                          // see the slotKey derivation in ComputeDecrement.
    uint16_t featDefIdx;  // feature-identity check -- see GetFeatureTypeFromOrder's contract
    uint16_t pad;
    int32_t  lastTick;
    int32_t  counter;     // the shared reclaim counter
};

Slot     g_table[kMaxPlayers][kSlotsPerPlayer];
int32_t  g_lastGlobalTick     = 0;
bool     g_haveLastGlobalTick = false;

static_assert((kSlotsPerPlayer & (kSlotsPerPlayer - 1)) == 0, "kSlotsPerPlayer must be a power of two");
static_assert(kProbeWindow > 0 && kProbeWindow <= kSlotsPerPlayer, "kProbeWindow out of range");
static_assert(kSpeedDen  >= 1 && kSpeedDen  < 0x10000, "kSpeedDen must fit the 16-bit accumulator packing");
static_assert(kEnergyDen >= 1 && kEnergyDen < 0x10000, "kEnergyDen must fit the 16-bit accumulator packing");
static_assert(kSpeedNum  >= 1, "kSpeedNum must be positive");
static_assert(kEnergyNum >= 0, "kEnergyNum must be non-negative");

// A new match (or a save load) restarts the tick counter, so a backwards jump is the signal to
// drop every player's table wholesale -- closes the "quit to menu, rejoin, inherit progress on
// whatever now sits at those coordinates" hole (E-5 in TA_PROJECT4_RECLAIM_FINDINGS.md, S-6 in
// the CE script's own audit) with no game-start hook of our own.
void MaybeWipeTable(int32_t now)
{
    if (g_haveLastGlobalTick && now < g_lastGlobalTick)
        std::memset(g_table, 0, sizeof(g_table));
    g_lastGlobalTick     = now;
    g_haveLastGlobalTick = true;
}

inline uint32_t MixKey(uint32_t k)
{
    k *= 0x9E3779B1u;
    k ^= k >> 16;
    return k;
}

// Pass 1: search only, no claim, no mutation. Called BEFORE the resource gate so a browned-out
// unit can still learn the current shared counter to cap its billable work against (D4), without
// ever being able to touch the table if it turns out it cannot pay (E-4).
Slot* FindSlot(Slot* partition, uint32_t key, uint16_t featDefIdx, int32_t now)
{
    const uint32_t bucket = MixKey(key) & (kSlotsPerPlayer - 1);
    for (int i = 0; i < kProbeWindow; ++i)
    {
        Slot& s = partition[(bucket + i) & (kSlotsPerPlayer - 1)];
        if (s.key == key && s.featDefIdx == featDefIdx && (now - s.lastTick) <= kAssistStale)
            return &s;
    }
    return nullptr;
}

// Pass 2: claim an empty or expired slot within the same probe window. Only ever called after
// the resource gate has passed. Returns nullptr if the window is exhausted -- the caller falls
// back to scaled-solo, never to a progress reset (see the header's kProbeWindow doc).
Slot* ClaimSlot(Slot* partition, uint32_t key, int32_t now)
{
    const uint32_t bucket = MixKey(key) & (kSlotsPerPlayer - 1);
    for (int i = 0; i < kProbeWindow; ++i)
    {
        Slot& s = partition[(bucket + i) & (kSlotsPerPlayer - 1)];
        if (s.key == 0 || (now - s.lastTick) > kAssistStale)
            return &s;
    }
    return nullptr;
}

// ---- frame reuse: the handler already resolved the tile and the FeatureDef this callback -----
//
// VERIFIED [bin] (plan doc section 4.2): both handlers call GetFeatureTypeFromOrder once at
// order-state-dispatch time and leave BOTH results live in their own stack frame all the way
// through the tick body -- the packed root tile key sits in the caller's own outPackedTile
// local (ground: buf->Esp+0x40, VTOL: buf->Esp+0x3C -- derived by exhaustive symbolic ESP
// tracking through every push/call/ret between function entry and our splice, since ESP is net
// unchanged by every intervening call), and esi holds the FeatureDefStruct* untouched (both
// callees at 0x47F780 and 0x439E80 properly push/pop esi, and the state-3/4 tick body itself
// never writes it). Reusing this removes a whole 0x421DA0 call plus its internal map-cell
// lookups from every callback instead of adding one.
//
// One-time self-validation per path: on first use, ALSO call GetFeatureTypeFromOrder directly
// and compare. A match latches the fast path forever. A mismatch (wrong binary build, a shifted
// frame from some other patch) latches a permanent fallback that calls 0x421DA0 every callback
// instead -- correct, just slower -- and logs once, rather than silently reading garbage.
struct TileInfo
{
    uint32_t key;
    uint16_t featDefIdx;
    bool     valid;
};

bool g_groundFrameValidated = false;
bool g_groundFrameOk        = false;
bool g_vtolFrameValidated   = false;
bool g_vtolFrameOk          = false;

TileInfo DirectResolve(void* order)
{
    uint32_t key = 0, foot = 0;
    unsigned short idx = GetFeatureTypeFromOrder(reinterpret_cast<char*>(order) + 0x22, &key, &foot);
    TileInfo info;
    info.key        = key;
    info.featDefIdx = idx;
    info.valid       = (idx != 0xFFFFu);
    return info;
}

uint16_t FeatDefIndexOf(void* featDefPtr, TAdynmemStruct* taPtr)
{
    if (!featDefPtr || !taPtr || !taPtr->FeatureDef)
        return 0xFFFFu;
    const ptrdiff_t delta = reinterpret_cast<char*>(featDefPtr) - reinterpret_cast<char*>(taPtr->FeatureDef);
    if (delta < 0)
        return 0xFFFFu;
    return static_cast<uint16_t>(delta >> 8);  // sizeof(FeatureDefStruct) == 0x100
}

TileInfo ResolveGroundTile(void* order, uint32_t frameKey, uint16_t frameFeatIdx)
{
    if (!g_groundFrameValidated)
    {
        g_groundFrameValidated = true;
        TileInfo direct = DirectResolve(order);
        g_groundFrameOk = direct.valid && direct.key == frameKey && direct.featDefIdx == frameFeatIdx;
        if (!g_groundFrameOk)
        {
            IDDrawSurface::OutptTxt(
                "ReclaimAssist: ground frame-reuse self-check failed -- "
                "falling back to a direct GetFeatureTypeFromOrder call every callback.");
            return direct;
        }
    }
    if (g_groundFrameOk)
    {
        TileInfo info;
        info.key = frameKey;
        info.featDefIdx = frameFeatIdx;
        info.valid = true;
        return info;
    }
    return DirectResolve(order);
}

TileInfo ResolveVTOLTile(void* order, uint32_t frameKey, uint16_t frameFeatIdx)
{
    if (!g_vtolFrameValidated)
    {
        g_vtolFrameValidated = true;
        TileInfo direct = DirectResolve(order);
        g_vtolFrameOk = direct.valid && direct.key == frameKey && direct.featDefIdx == frameFeatIdx;
        if (!g_vtolFrameOk)
        {
            IDDrawSurface::OutptTxt(
                "ReclaimAssist: VTOL frame-reuse self-check failed -- "
                "falling back to a direct GetFeatureTypeFromOrder call every callback.");
            return direct;
        }
    }
    if (g_vtolFrameOk)
    {
        TileInfo info;
        info.key = frameKey;
        info.featDefIdx = frameFeatIdx;
        info.valid = true;
        return info;
    }
    return DirectResolve(order);
}

// ---- core: pure integer, mirrors recCore from reclaim_assist_v5.CEA ---------------------------

struct DecrementResult
{
    int32_t newCounter;
    bool    gatePassed;   // drives the beam gate -- see the header's "THE BEAM BUG" note
};

// beam-gate statics: the per-path "did this callback's gate pass" flag the beam routers read.
// Safe as plain globals -- ENGINE_NOTES section 21 establishes the simulation is single-
// threaded over unguarded globals, same basis RepairRateFix.cpp relies on for its own statics.
// VERIFIED [bin] that the beam gate is reachable ONLY from this callback's own tick site (the
// full branch scan into 0x404C8B..0x404CB1 / VTOL equivalent finds exactly the tick body's own
// fall-through and one early-return epilogue), so a static cannot go stale between the tick
// router setting it and the beam router reading it.
bool g_lastGatePassedGround = false;
bool g_lastGatePassedVTOL   = false;

DecrementResult ComputeDecrement(UnitStruct* unit, void* order, const TileInfo& tile)
{
    DecrementResult result{ 0, false };
    if (!order)
        return result;   // order cannot safely be read at all; the replayed store that
                          // follows will fault the same way vanilla itself would have --
                          // this can only happen if the splice site's own invariants
                          // (edi always a live order at this point) are already broken.

    // unit==nullptr or unit->UnitType==nullptr: matches recCore's own defensive early-exits
    // in reclaim_assist_v5.CEA, which fall through to coreExit with vD left at its default
    // of 2 (vanilla decrement) rather than stalling or zeroing the counter. "Should never
    // happen at a live splice site," same as the CE's own reasoning -- unit and its UnitDef
    // are dereferenced unconditionally by the surrounding engine code both before and after
    // our splice, so if either were truly null the engine would already have faulted. The
    // safest fallback available to us is therefore "behave exactly like vanilla," matching
    // the WT==0 guard immediately below rather than inventing a third, untested behaviour.
    UnitDefStruct* def = unit ? unit->UnitType : nullptr;
    if (!unit || !def)
    {
        result.newCounter = OrderCounter(order) - kSpeedNum;
        result.gatePassed = true;
        return result;
    }

    const uint32_t workerTime     = def->nWorkerTime;   // already unsigned short: movzx-equivalent
    const int32_t  currentCounter = OrderCounter(order);

    if (workerTime == 0)
    {
        // Vanilla-equivalent, never-stall path: WT=0 would otherwise divide-by-zero the speed
        // formula and permanently stall this order. Matches recCore's own guard.
        result.newCounter = currentCounter - kSpeedNum;
        result.gatePassed = true;  // no gate to fail on an already-free decrement
        return result;
    }

    TAdynmemStruct* taPtr = GetTA();
    ResolveVanillaSpeedUnits(taPtr);   // no-op after the first successful resolve per array instance
    const bool vanillaSpeed = IsVanillaSpeedUnit(unit->UnitID);

    uint32_t packedAccum = OrderAccum(order);
    uint32_t accS = packedAccum & 0xFFFFu;
    uint32_t accE = (packedAccum >> 16) & 0xFFFFu;
    if (accS >= static_cast<uint32_t>(kSpeedDen))   accS = 0;  // recycled-order garbage
    if (accE >= static_cast<uint32_t>(kEnergyDen))  accE = 0;

    int32_t D;
    if (vanillaSpeed)
    {
        // Scavenge exception (D6): full vanilla reclaim semantics for kVanillaSpeedUnits.
        // Speed is flat, never Build-Power-scaled; no speed accumulator to carry (nothing
        // fractional is ever produced by a fixed decrement), so accS is simply zeroed rather
        // than fed through the division below.
        D = kSpeedNum;
        accS = 0;
    }
    else
    {
        const int32_t ebp = (kMode == 0)
            ? static_cast<int32_t>(workerTime)
            : IntSqrt(workerTime * static_cast<uint32_t>(kSpeedDen));
        const uint32_t t = static_cast<uint32_t>(ebp) * static_cast<uint32_t>(kSpeedNum) + accS;
        D    = static_cast<int32_t>(t / static_cast<uint32_t>(kSpeedDen));
        accS = t % static_cast<uint32_t>(kSpeedDen);
    }

    const int32_t now = taPtr ? taPtr->GameTime : 0;

    // ---- assist pass 1: search only, before the gate (D4) ----
    // Scavenge units (D6) never participate -- cap always comes from their own order counter,
    // so they race exactly as vanilla and never stack, with or without kAssistOn.
    Slot* slot = nullptr;
    int32_t cap = currentCounter;
    const bool ownerInRange = unit->cOwnerID < kMaxPlayers;
    // Slot keys are stored BIASED BY +1 so that a stored 0 unambiguously means "empty".
    // The packed tile key for map tile (0,0) is legitimately 0, and without the bias a
    // feature sitting there would match a never-written slot whose key/featDefIdx/lastTick
    // are all still zero: cap would come back 0, needsSeed would evaluate false (nothing
    // differs from the zeroed slot), and the shared counter would go 0-D negative -- an
    // instant, free reclaim. Narrow (it needs featDefIdx 0 and the first kAssistStale ticks
    // after a table wipe) but reachable, so it is closed by construction here rather than
    // argued away.
    const uint32_t slotKey = tile.key + 1u;
    if (kAssistOn && !vanillaSpeed && taPtr && tile.valid && ownerInRange)
    {
        MaybeWipeTable(now);
        slot = FindSlot(g_table[unit->cOwnerID], slotKey, tile.featDefIdx, now);
        if (slot)
            cap = slot->counter;
    }
    if (cap < 0)
        cap = 0;

    int32_t work = D;
    if (work > cap)
        work = cap;

    // ---- energy: exact-rational, then the resource gate BEFORE any commit ----
    // Order matters (ENGINE_NOTES gotcha, and the Project-3 v1 bug this project already hit
    // once): mutating any accumulator before the gate's return value is known risks silently
    // discarding or duplicating a fractional work unit on a boundary-crossing tick.
    int32_t charge = 0;
    if (kEnergyOn)
    {
        const uint32_t e = static_cast<uint32_t>(work) * static_cast<uint32_t>(kEnergyNum) + accE;
        charge = static_cast<int32_t>(e / static_cast<uint32_t>(kEnergyDen));
        accE   = e % static_cast<uint32_t>(kEnergyDen);
    }

    bool gatePassed = true;
    if (kEnergyOn && charge > 0)
    {
        void* resourceBlock = reinterpret_cast<char*>(unit) + 0xBC;  // Unit+0xBC, ENGINE_NOTES section 8.1
        gatePassed = ChargeEnergy(resourceBlock, charge) != 0;
    }

    if (!gatePassed)
    {
        // Browned out. Nothing commits: order+0x3A keeps its old value (we never write it),
        // order+0x36 keeps its old value too (newCounter == currentCounter, so the replayed
        // store is a no-op write of the same bytes). The slot, if any was found in pass 1, is
        // left completely untouched -- not claimed, not touched, not aged -- so a unit that
        // cannot pay contributes nothing and can never free-ride an ally's paid-for progress.
        result.newCounter = currentCounter;
        result.gatePassed = false;
        return result;
    }

    // ---- commit ----
    OrderAccum(order) = (accE << 16) | (accS & 0xFFFFu);

    int32_t newCounter;
    if (!vanillaSpeed && kAssistOn && taPtr && tile.valid && ownerInRange)
    {
        Slot* partition = g_table[unit->cOwnerID];
        if (!slot)
            slot = ClaimSlot(partition, slotKey, now);

        if (slot)
        {
            const bool needsSeed = (slot->key != slotKey)
                                 || (slot->featDefIdx != tile.featDefIdx)
                                 || (now - slot->lastTick) > kAssistStale;
            if (needsSeed)
            {
                // Freshly claimed, or reclaimed after expiring: seed from THIS order's own
                // counter so a resumed reclaim keeps its progress (matches the CE reference).
                slot->key        = slotKey;
                slot->featDefIdx = tile.featDefIdx;
                slot->counter    = currentCounter;
            }
            slot->counter -= D;
            slot->lastTick  = now;
            newCounter      = slot->counter;
        }
        else
        {
            // Probe window exhausted (D7): scaled-solo fallback. Still Build-Power-scaled,
            // still paying energy, just not pooling with anyone this callback. Progress is
            // never lost -- order+0x36 tracks correctly either way.
            newCounter = currentCounter - D;
        }
    }
    else
    {
        newCounter = currentCounter - D;
    }

    result.newCounter = newCounter;
    result.gatePassed = true;
    return result;
}

// ---- routers -----------------------------------------------------------------------------------

int __stdcall GroundTickRouter(PInlineX86StackBuffer buf)
{
    UnitStruct* unit  = reinterpret_cast<UnitStruct*>(buf->Ebx);
    void*       order = reinterpret_cast<void*>(buf->Edi);
    void*       featDefPtr = reinterpret_cast<void*>(buf->Esi);
    const uint32_t frameKey = *reinterpret_cast<uint32_t*>(buf->Esp + 0x40);
    const uint16_t frameFeatIdx = FeatDefIndexOf(featDefPtr, GetTA());

    const TileInfo tile = ResolveGroundTile(order, frameKey, frameFeatIdx);
    const DecrementResult r = ComputeDecrement(unit, order, tile);

    g_lastGatePassedGround = r.gatePassed;
    buf->Ecx = static_cast<DWORD>(r.newCounter);
    return 0;
}

int __stdcall VTOLTickRouter(PInlineX86StackBuffer buf)
{
    UnitStruct* unit  = *reinterpret_cast<UnitStruct**>(buf->Esp + 0x38);
    void*       order = reinterpret_cast<void*>(buf->Edi);
    void*       featDefPtr = reinterpret_cast<void*>(buf->Esi);
    const uint32_t frameKey = *reinterpret_cast<uint32_t*>(buf->Esp + 0x3C);
    const uint16_t frameFeatIdx = FeatDefIndexOf(featDefPtr, GetTA());

    const TileInfo tile = ResolveVTOLTile(order, frameKey, frameFeatIdx);
    const DecrementResult r = ComputeDecrement(unit, order, tile);

    g_lastGatePassedVTOL = r.gatePassed;
    buf->Ecx = static_cast<DWORD>(r.newCounter);
    return 0;
}

// Beam gates: redirect rather than register-only, because the displaced/replayed bytes at
// both original sites would reload the counter from memory (undoing our decision) AND
// recompute EFLAGS from a comparison we don't want re-run. Redirecting straight to the
// vanilla draw/skip continuation needs no naked asm and no flag plumbing.
//
// Fixes THE BEAM BUG documented in the header: gates on g_lastGatePassedGround/VTOL (whether
// the resource gate passed THIS callback), not on whether D happened to be 0 -- the two are
// different questions, and the CE reference this was ported from conflated them.
int __stdcall GroundBeamRouter(PInlineX86StackBuffer buf)
{
    void* order = reinterpret_cast<void*>(buf->Edi);
    const int32_t counter = OrderCounter(order);
    const bool draw = g_lastGatePassedGround && (counter > kBeamMin);

    // EAX is VERIFIED dead at both ground beam targets (0x404CBE immediately does
    // `lea eax,[esp+0x14]`; 0x404D51 immediately does `mov eax,2`), so nothing to preserve.
    buf->rtnAddr_Pvoid = reinterpret_cast<LPVOID>(draw ? kGroundBeamDrawTarget : kGroundBeamSkipTarget);
    return X86STRACKBUFFERCHANGE;
}

int __stdcall VTOLBeamRouter(PInlineX86StackBuffer buf)
{
    void* order = reinterpret_cast<void*>(buf->Edi);
    const int32_t counter = OrderCounter(order);
    const bool draw = g_lastGatePassedVTOL && (counter > kBeamMin);

    // EAX is VERIFIED LIVE here (pushed as an argument at the draw target, 0x4149A8) -- it must
    // hold the unit pointer, matching the ground beam draw path's equivalent push of ebx=unit
    // at 0x404CC3 (both call the same 0x43E400 with the unit as its first stdcall argument).
    // The skip target (0x414A36) overwrites eax immediately, so setting it unconditionally here
    // is simplest and harmless on that path.
    buf->Eax = *reinterpret_cast<DWORD*>(buf->Esp + 0x38);
    buf->rtnAddr_Pvoid = reinterpret_cast<LPVOID>(draw ? kVTOLBeamDrawTarget : kVTOLBeamSkipTarget);
    return X86STRACKBUFFERCHANGE;
}

InlineSingleHook* g_groundTickHook = nullptr;
InlineSingleHook* g_vtolTickHook   = nullptr;
InlineSingleHook* g_groundBeamHook = nullptr;
InlineSingleHook* g_vtolBeamHook   = nullptr;

} // namespace

namespace ReclaimAssist {

void Install()
{
    if (g_groundTickHook)
        return;

    g_groundTickHook = new InlineSingleHook(kGroundTickAddr, kGroundTickLen,
        INLINE_5BYTESLAGGERJMP, (InlineX86HookRouter)GroundTickRouter);
    g_vtolTickHook = new InlineSingleHook(kVTOLTickAddr, kVTOLTickLen,
        INLINE_5BYTESLAGGERJMP, (InlineX86HookRouter)VTOLTickRouter);
    g_groundBeamHook = new InlineSingleHook(kGroundBeamAddr, kGroundBeamLen,
        INLINE_5BYTESLAGGERJMP, (InlineX86HookRouter)GroundBeamRouter);
    g_vtolBeamHook = new InlineSingleHook(kVTOLBeamAddr, kVTOLBeamLen,
        INLINE_5BYTESLAGGERJMP, (InlineX86HookRouter)VTOLBeamRouter);

    // No table/unit-def allocation here on purpose -- Install() runs at DLL_PROCESS_ATTACH,
    // before the engine has loaded anything. The assist table is static storage (always
    // present, just all-zero/inert until used); ResolveVanillaSpeedUnits() and MaybeWipeTable()
    // both re-check their cached state on first real use, same lazy-init shape RepairRateFix.cpp
    // and WeaponIdOverflow.cpp already use for exactly this reason.
}

void Shutdown()
{
    delete g_groundTickHook; g_groundTickHook = nullptr;
    delete g_vtolTickHook;   g_vtolTickHook   = nullptr;
    delete g_groundBeamHook; g_groundBeamHook = nullptr;
    delete g_vtolBeamHook;   g_vtolBeamHook   = nullptr;

    std::memset(g_table, 0, sizeof(g_table));
    g_lastGlobalTick        = 0;
    g_haveLastGlobalTick    = false;
    g_vanillaSpeedResolvedCount = 0;
    g_vanillaSpeedCachedBase    = nullptr;
    g_groundFrameValidated = g_groundFrameOk = false;
    g_vtolFrameValidated   = g_vtolFrameOk   = false;
    g_lastGatePassedGround = g_lastGatePassedVTOL = false;
}

} // namespace ReclaimAssist
