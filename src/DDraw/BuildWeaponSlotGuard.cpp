#include "BuildWeaponSlotGuard.h"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <memory>

#include "config.h"
#include "iddrawsurface.h"
#include "hook/hook.h"
#include "tamem.h"
#include "TAbugfix.h"

namespace
{
	// All `[bin]` VERIFIED against TotalA.exe, Escalation GOLD 10.2.0, 1,178,624 bytes,
	// md5 1e677a7f92c79b5ab35440853d822c17 (10.1 is code-identical apart from 8 bytes
	// elsewhere -- these addresses hold on both). The install-time byte check below is
	// what makes trusting these addresses on an unverified build safe rather than a
	// guess: a mismatch disables the module instead of patching the wrong bytes.
	//
	// Corrected 2026-09-14: this project only tested against Escalation's exe, and
	// earlier comments here said the addresses were Escalation-specific as if that
	// were established. It was not -- PR #26's review independently re-derived every
	// signature below against all seven shipped TotalA.exe builds (tacc/taesc/
	// tamayhem/tatw/tavmod/tazero/GOG) and found all of them byte-identical, because
	// this is stock TA engine code no mod patches. This project has not re-run that
	// verification itself, so the gate (config.h et al.) stays Escalation-only -- not
	// because the addresses are believed to differ, but because nobody on this side
	// has checked.

	// ---- Fix 1: clamp the reload divisor at weapon-load time ----------------------
	//
	// WeaponDef_LoadTdfProperties epilogue:
	//   0042F313  55                push ebp          ; ebp = the fully-parsed WeaponStruct*
	//   0042F314  E8 F7 EC 06 00    call 0x49E010
	// EBP is written exactly once in this whole function, at 0x0042E489, and never
	// reassigned before this point -- re-verified this session by disassembling the
	// complete function body and enumerating every instruction that writes EBP.
	const DWORD kFix1HookAddr = 0x0042F313u;
	const DWORD kFix1HookLen  = 6u;
	const BYTE  kFix1ExpectedBytes[6] = { 0x55, 0xE8, 0xF7, 0xEC, 0x06, 0x00 };

	// Offsets into WeaponStruct. Not named fields in tamem.h (they fall inside its
	// filler arrays -- +0xE4 is the first two bytes of `data6[0x26]`) -- ReloadBars.cpp
	// already reads both under these exact names for its own, unrelated draw feature;
	// matched here rather than re-guessed.
	const size_t kWeaponReloadTimeOffset = 0xE4u;  // WORD: (int)(reloadtime*30), 0x42E561
	const size_t kWeaponTypeMaskOffset   = 0x111u; // DWORD bitmask, WTM_Stockpile = bit 28

	DWORD g_fix1Clamped = 0;

	// ---- Fix 2: bounds-check the weapon-slot index in both consumers --------------
	//
	// UnitOrdersStruct::BuildUnitID (+0x36) is a generic scratch field reused by other,
	// unrelated mission-tick handlers with different range semantics (confirmed by a
	// whole-.text scan of every `[reg+0x36]` reference: other functions compare it
	// against 2 or 3 for entirely different order types). Only 0,1,2 are ever valid
	// HERE, because these two functions only run for a BuildWeapon order and a unit
	// has exactly three weapon slots (UnitStruct::Weapon1/2/3). The check is therefore
	// scoped to these two hook sites, not to the field globally -- a global clamp would
	// silently reinterpret data other order types rely on.
	const DWORD kMaxValidWeaponSlot = 2u;

	// Sim: MissionTick_BuildWeapon @0x00402B70.
	//   00402B73  8B 54 24 0C       mov edx,[esp+0xC]    ; edx = UnitStruct* (set once,
	//                                                       never reassigned before the
	//                                                       hook fires -- re-verified this
	//                                                       session, see WeaponDivisorIsSafe)
	//   00402B7A  8B 74 24 1C       mov esi,[esp+0x1C]   ; esi = UnitOrdersStruct*
	//   00402B7F  8B 46 36          mov eax,[esi+0x36]   ; <- hook site
	//   00402B82  8B C8             mov ecx,eax
	//   00402B84  C1 E1 03          shl ecx,3            ; (only the first byte, 0xC1, is
	//                                                       inside the requested 6-byte
	//                                                       window -- InlineSingleHook's
	//                                                       own instruction-length walk
	//                                                       (GetMinValidLenWithMatchOpcode,
	//                                                       hook/etc.cpp) extends the real
	//                                                       footprint to the next whole
	//                                                       instruction, 8 bytes, on its
	//                                                       own; re-verified by reading
	//                                                       that code this session, not
	//                                                       assumed)
	//   00402B87  2B C8             sub ecx,eax          ; ecx = idx*7 (outside the stolen
	//                                                       window, but this is the
	//                                                       instruction that makes the
	//                                                       stride 0x1C, not 0x20 -- PR #26
	//                                                       review nit: this line was
	//                                                       missing from this listing)
	// Must sit before 0x00402B89, the unconditional `mov edi,[edx+ecx*4+0x10]` weapon load.
	const DWORD kFix2aHookAddr = 0x00402B7Fu;
	const DWORD kFix2aHookLen  = 6u;
	const BYTE  kFix2aExpectedBytes[8] =
		{ 0x8B, 0x46, 0x36, 0x8B, 0xC8, 0xC1, 0xE1, 0x03 };

	// 0x00402BA4: vanilla's own "unrecognised State" return -- mov eax,7; pop
	// edi/esi/ebp/ebx; add esp,8; ret 0xC. Stack-correct as a redirect target from
	// 0x00402B7F: at that address ebx,ebp,esi,edi have ALL already been pushed (in
	// that order, at 00402B77-7E) and esp already carries the function's initial
	// `sub esp,8`, which is exactly what this epilogue unwinds -- re-verified by
	// disassembling the function's prologue this session.
	const DWORD kFix2aBailoutAddr = 0x00402BA4u;
	const BYTE  kFix2aBailoutExpectedBytes[5] = { 0xB8, 0x07, 0x00, 0x00, 0x00 };

	// HUD: Unit_GetLinkedBuildWeaponPercent @0x00439D20.
	//   00439D41  8B 48 36          mov ecx,[eax+0x36]   ; eax = UnitOrdersStruct*
	//   00439D44  8B 40 3E          mov eax,[eax+0x3e]
	// Exactly one caller in the whole binary (0x0046B446, scan_call_refs VERIFIED), and
	// exactly one branch enters [0x439D60,0x439D80) (the function's own `je` at
	// 0x439D2A) -- so redirecting into that block cannot collide with any other entry.
	const DWORD kFix2bHookAddr = 0x00439D41u;
	const DWORD kFix2bHookLen  = 6u;
	const BYTE  kFix2bExpectedBytes[6] = { 0x8B, 0x48, 0x36, 0x8B, 0x40, 0x3E };

	// 0x00439D6B: the function's own `xor eax,eax; pop esi; ret 4` -- already an
	// exercised return-0 path (BackgroundOrder null / order list exhausted). Only
	// `esi` has been pushed by the time our hook fires (00439D24), matching exactly
	// what this epilogue pops.
	const DWORD kFix2bBailoutAddr = 0x00439D6Bu;
	const BYTE  kFix2bBailoutExpectedBytes[6] = { 0x33, 0xC0, 0x5E, 0xC2, 0x04, 0x00 };

	// ---- Fix 2, extended 2026-09-14: check the divisor itself, not just the index ------
	//
	// PR #26 review (Axle1975) recovered FIVE independent production crashes at the HUD's
	// faulting instruction (0x00439D65) from real `game_logs` and reconstructed the
	// faulting WeaponStruct* in every one: all five are `&WeaponsTypedefArray[0]` -- TA's
	// own permanent "no weapon" sentinel entry, not a degenerate stockpile TDF weapon and
	// not an out-of-range index. `LoadUNITINFO` (0x0042CDE3-0x0042CE0C, re-verified this
	// session by disassembly) falls every unarmed weapon slot back to that exact entry --
	// `lea esi,[eax+0x2CF3]` is `&WeaponsTypedefArray[0]`, and `mov eax,esi` after a failed
	// name lookup is the fallback -- and `UNITS_StartWeaponsScripts` (0x0049E070) copies it
	// straight into `UnitWeapons[n].p_Weapon` for any unit type whose slot n has no weapon.
	// The sentinel's `reloadtime` (+0xE4) is 0 by construction: `LoadWeapons_Tdf`
	// (0x0042E31C) initialises the whole 256-entry array with `ID=index` and an empty
	// name, and it is never subsequently parsed from any TDF, so Fix 1's clamp -- which
	// only ever runs from inside the TDF loader -- can never reach it.
	//
	// This is the exact mechanism `tamem.h`'s `WeaponStruct::weaponvelocity` comment (the
	// neighbouring `+0x68` field, already `#include`d by this file) and `config.h`'s
	// `WEAPONFIRE_DISPATCH_FROM_SLOT` block already document: a unit-identity divergence
	// (this client's local copy of a remote unit has the wrong type) makes a legitimately
	// issued order reference a weapon slot that, locally, is unarmed. `DrawUnitBottomState`
	// (0x0046A860, which reaches this whole call chain) filters the displayed unit by
	// `UnitINFOID != 0` and LOS only -- there is no owner check on this path -- so the unit
	// under the cursor can be a remote player's unit whose identity has diverged on THIS
	// client alone. That is why this crash kills only the diverged client and not everyone
	// in the game: a genuinely bad TDF would kill every player who inspects that weapon; a
	// wrong local copy kills only whoever's copy is wrong. (One prod game had 9 players in
	// the bundle and exactly 2 crashed here -- consistent with a per-client cause, not a
	// shared-data one.)
	//
	// Bounds-checking the index alone (above) does not catch this: the index is perfectly
	// valid (0, 1, or 2) and the pointer it resolves to is perfectly readable -- entry 0 of
	// a real, live, permanently-allocated array. The actual fix is to check the value this
	// code is about to divide by, at the point of use, regardless of why it might be zero
	// -- which also makes Fix 1's TDF-time clamp strictly redundant for anything this check
	// already covers, though it is left in as free, zero-behaviour-change insurance for a
	// genuinely misconfigured TDF (a case Fix 1 protects and this check also would, but
	// only once such a weapon is actually the one being divided by).
	const size_t kUnitWeaponSlotStride = 0x1Cu;  // 28: idx*28, tamem.h Weapon1/2/3 strides
	const size_t kUnitWeaponSlotBase   = 0x10u;  // Weapon1@0x10 / Weapon2@0x2C / Weapon3@0x48
	// WeaponDivisorIsSafe() itself is defined below, after SafeIsBadReadPtr (it calls
	// it) -- see the definition just below that function for the full comment.

	DWORD g_fix2aRejects = 0;
	DWORD g_fix2bRejects = 0;
	DWORD g_fix2aZeroDivisorRejects = 0;   // subset of g_fix2aRejects: index was VALID,
	DWORD g_fix2bZeroDivisorRejects = 0;   // the resolved weapon's divisor was the problem
	DWORD g_fix2aSane = 0;   // valid-index calls that fell through untouched
	DWORD g_fix2bSane = 0;

	// A rejection is never silent, but a HEALTHY run produces no log line at all from
	// either fix -- which leaves no way to tell "this played a full session with the
	// path never exercised" apart from "this played a full session and it worked",
	// exactly the ambiguity the project's regression test (plan SS5 L3, "sane > 0,
	// else INDETERMINATE -- the path was never exercised") is designed to catch.
	// Matches the GetTickCount()-throttled idiom SoundLimitHeartbeat already uses in
	// TABugFix.cpp for the same reason.
	//
	// PR #26 review nit: this used to be called only from the SANE branch of each
	// router, so a session where every call was rejected (the exact scenario a
	// heartbeat exists to make visible) never produced one -- inverting the comment's
	// own intent. Both routers now call this unconditionally, on the sane path and the
	// reject path alike; the 30s throttle below is what keeps it cheap either way.
	DWORD g_lastHeartbeatMs = 0;
	void MaybeHeartbeat()
	{
		const DWORD now = GetTickCount();
		if (g_lastHeartbeatMs != 0 && (now - g_lastHeartbeatMs) < 30000u)
			return;
		g_lastHeartbeatMs = now;
		IDDrawSurface::OutptFmtTxt(
			"[BuildWeaponSlotGuard][heartbeat] clamped=%lu "
			"sim(sane=%lu rejected=%lu zerodiv=%lu) hud(sane=%lu rejected=%lu zerodiv=%lu)",
			g_fix1Clamped, g_fix2aSane, g_fix2aRejects, g_fix2aZeroDivisorRejects,
			g_fix2bSane, g_fix2bRejects, g_fix2bZeroDivisorRejects);
	}

	// Matches the OrderDispatchShouldLog idiom in TABugFix.cpp: log the first 20
	// occurrences in full, then only every 1000th, so a permanently-bad order cannot
	// spam every tick forever while a rare one is still fully visible.
	bool ShouldLog(DWORD n)
	{
		return n <= 20 || (n % 1000) == 0;
	}

	// Copied, not exported, from TABugFix.cpp (it is file-static there too): probes
	// with our own __try/__except rather than IsBadReadPtr directly, because on Wine a
	// probe that lands on a thread-stack guard page can escape IsBadReadPtr as an
	// unhandled exception instead of returning cleanly.
	bool SafeIsBadReadPtr(const void* p, size_t n)
	{
		if (!p) return true;
		__try
		{
			const volatile unsigned char* b = (const volatile unsigned char*)p;
			for (size_t i = 0; i < n; ++i) (void)b[i];
			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return true;
		}
	}

	// True iff `unit`'s weapon at `idx` (idx already known to be in {0,1,2}) is safe to
	// divide by: the slot resolves to a readable WeaponStruct* whose reload-tick total
	// (+0xE4) is non-zero. False covers an unreadable pointer (the original index-
	// corruption hazard) and a readable-but-zero divisor (the sentinel, or any other
	// degenerate weapon) identically, because both produce the exact same crash at the
	// exact same instruction and the fix -- do not use this weapon's number -- is the
	// same either way. See the big comment above kUnitWeaponSlotStride for why this
	// exists: PR #26's review matched this exact shape to five real production crashes.
	bool WeaponDivisorIsSafe(DWORD unit, DWORD idx)
	{
		const DWORD slotAddr = unit + static_cast<DWORD>(kUnitWeaponSlotStride * idx) + kUnitWeaponSlotBase;
		if (SafeIsBadReadPtr(reinterpret_cast<const void*>(slotAddr), sizeof(DWORD)))
			return false;

		const DWORD weapon = *reinterpret_cast<const DWORD*>(slotAddr);
		if (SafeIsBadReadPtr(reinterpret_cast<const void*>(weapon),
			kWeaponReloadTimeOffset + sizeof(WORD)))
			return false;

		const WORD divisor = *reinterpret_cast<const WORD*>(
			reinterpret_cast<const BYTE*>(weapon) + kWeaponReloadTimeOffset);
		return divisor != 0;
	}

	bool CheckBytes(DWORD address, const BYTE* expected, size_t len, const char* what)
	{
		if (std::memcmp(reinterpret_cast<const void*>(address), expected, len) == 0)
			return true;

		IDDrawSurface::OutptFmtTxt(
			"[BuildWeaponSlotGuard] DISABLED: unexpected TotalA.exe bytes at 0x%08X (%s)",
			address, what);
		return false;
	}

	// ---------------------------------------------------------------------------
	// Fix 1 router -- runs once per weapon, at the end of TDF loading. Always
	// returns 0 (never redirects): this fix only ever writes one field, it never
	// changes control flow, so the original stolen bytes always replay exactly as
	// vanilla intended.
	//
	// Corrected 2026-09-14 (PR #26 review): this is NOT what fixed the production
	// crashes this module was built for. It cannot be -- WeaponsTypedefArray[0], the
	// weapon every real crash divided by, never passes through this function at all
	// (it is a reserved sentinel, initialised directly by LoadWeapons_Tdf, never
	// parsed) and its WeaponTypeMask is 0, so this router would skip it even if it
	// did. Kept as free, zero-behaviour-change insurance against a genuinely
	// misconfigured stockpile weapon's TDF -- a real, if so-far unobserved, mistake
	// this still catches. The fix for the actual observed crashes is
	// WeaponDivisorIsSafe, below, called from both Fix 2 routers.
	// ---------------------------------------------------------------------------
	int __stdcall Fix1WeaponLoadEpilogueProc(PInlineX86StackBuffer buf)
	{
		WeaponStruct* w = reinterpret_cast<WeaponStruct*>(buf->Ebp);

		// EBP is VERIFIED stable across this whole function for every real call, but
		// probing costs nothing and this project has already been burned once by
		// trusting "cannot happen" without checking it.
		if (SafeIsBadReadPtr(w, kWeaponTypeMaskOffset + sizeof(DWORD)))
			return 0;

		const DWORD typeMask = *reinterpret_cast<const DWORD*>(
			reinterpret_cast<const BYTE*>(w) + kWeaponTypeMaskOffset);
		if ((typeMask & WTM_Stockpile) == 0)
			return 0;

		WORD* reloadTicks = reinterpret_cast<WORD*>(
			reinterpret_cast<BYTE*>(w) + kWeaponReloadTimeOffset);
		if (*reloadTicks != 0)
			return 0;

		*reloadTicks = 1;
		++g_fix1Clamped;

		char name[0x21];
		std::memcpy(name, w->WeaponName, sizeof(w->WeaponName));
		name[sizeof(w->WeaponName)] = '\0';   // WeaponName has no guaranteed NUL if fully used
		IDDrawSurface::OutptFmtTxt(
			"[BuildWeaponSlotGuard] '%s': stockpile weapon with reloadtime<=0 (or an "
			"exact 65536-tick wraparound) -- clamped WeaponStruct+0xE4 0 -> 1 (#%lu). "
			"This is a safety net; the correct fix is this weapon's TDF.",
			name, g_fix1Clamped);
		return 0;
	}

	// ---------------------------------------------------------------------------
	// Fix 2 routers -- one per consumer. Two predicates, checked in order, either of
	// which redirects to that function's own existing bail-out: (1) the order pointer
	// is not safely readable, or its BuildUnitID is outside {0,1,2} -- the original
	// out-of-bounds-read/write hazard; (2) the index IS valid but the weapon it
	// resolves to has a zero reload divisor -- the sentinel/degenerate-weapon hazard
	// PR #26's review found in five real production crashes (see the big comment
	// above WeaponDivisorIsSafe). (2) is only ever evaluated once (1) has already
	// passed, so a bad index never drives an out-of-range unit+idx*28 computation.
	// ---------------------------------------------------------------------------
	int __stdcall Fix2aSimBoundsProc(PInlineX86StackBuffer buf)
	{
		const UnitOrdersStruct* order = reinterpret_cast<const UnitOrdersStruct*>(buf->Esi);

		const bool readable = !SafeIsBadReadPtr(
			order, offsetof(UnitOrdersStruct, BuildUnitID) + sizeof(DWORD));
		const DWORD idx = readable ? order->BuildUnitID : 0xFFFFFFFFu;
		const bool indexOk = readable && idx <= kMaxValidWeaponSlot;
		// buf->Edx = unit pointer at this hook site -- re-verified 2026-09-14 by
		// disassembling 0x00402B70..0x00402B7F: set once at 0x00402B73 from [esp+0xC],
		// never reassigned before the hook fires (see the site comment above).
		const bool divisorOk = indexOk && WeaponDivisorIsSafe(buf->Edx, idx);

		if (indexOk && divisorOk)
		{
			++g_fix2aSane;
			MaybeHeartbeat();
			return 0;
		}

		++g_fix2aRejects;
		if (indexOk) ++g_fix2aZeroDivisorRejects;
		MaybeHeartbeat();
		CrashTrace_RecordEvent(TRACE_CAT_BWSG, reinterpret_cast<DWORD>(order), idx,
			readable ? 1u : 0u, 0);
		if (ShouldLog(g_fix2aRejects))
		{
			IDDrawSurface::OutptFmtTxt(
				"[BuildWeaponSlotGuard] sim: BAD weapon slot order=%08X readable=%d "
				"idx=%lu reason=%s -- bailing out to 0x%08X instead of an out-of-bounds "
				"unit read/write or a divide by zero (#%lu)",
				reinterpret_cast<DWORD>(order), (int)readable, idx,
				indexOk ? "zeroDivisor" : "badIndex", kFix2aBailoutAddr, g_fix2aRejects);
		}

		buf->rtnAddr_Pvoid = reinterpret_cast<LPVOID>(kFix2aBailoutAddr);
		return X86STRACKBUFFERCHANGE;
	}

	int __stdcall Fix2bHudBoundsProc(PInlineX86StackBuffer buf)
	{
		const UnitOrdersStruct* order = reinterpret_cast<const UnitOrdersStruct*>(buf->Eax);

		const bool readable = !SafeIsBadReadPtr(
			order, offsetof(UnitOrdersStruct, BuildUnitID) + sizeof(DWORD));
		const DWORD idx = readable ? order->BuildUnitID : 0xFFFFFFFFu;
		const bool indexOk = readable && idx <= kMaxValidWeaponSlot;
		// buf->Edx = unit pointer at this hook site -- re-verified 2026-09-14 by
		// disassembling 0x00439D20..0x00439D41: set once at 0x00439D20 from [esp+4],
		// never reassigned before the hook fires.
		const bool divisorOk = indexOk && WeaponDivisorIsSafe(buf->Edx, idx);

		if (indexOk && divisorOk)
		{
			++g_fix2bSane;
			MaybeHeartbeat();
			return 0;
		}

		++g_fix2bRejects;
		if (indexOk) ++g_fix2bZeroDivisorRejects;
		MaybeHeartbeat();
		CrashTrace_RecordEvent(TRACE_CAT_BWSG, reinterpret_cast<DWORD>(order), idx,
			readable ? 1u : 0u, 1);
		if (ShouldLog(g_fix2bRejects))
		{
			IDDrawSurface::OutptFmtTxt(
				"[BuildWeaponSlotGuard] hud: BAD weapon slot order=%08X readable=%d "
				"idx=%lu reason=%s -- bailing out to 0x%08X instead of reading garbage "
				"as a WeaponStruct* or dividing by zero (#%lu)",
				reinterpret_cast<DWORD>(order), (int)readable, idx,
				indexOk ? "zeroDivisor" : "badIndex", kFix2bBailoutAddr, g_fix2bRejects);
		}

		buf->rtnAddr_Pvoid = reinterpret_cast<LPVOID>(kFix2bBailoutAddr);
		return X86STRACKBUFFERCHANGE;
	}

	// ---------------------------------------------------------------------------
	// Self-test -- calls the three router functions above DIRECTLY, exactly as the
	// trampoline would, against synthetic WeaponStruct/UnitOrdersStruct/
	// InlineX86StackBuffer instances that never touch real game memory or the real
	// hook addresses. This is a deliberate departure from the project plan's
	// original T1a/T1b design, which called for forcing a REAL divide-by-zero at
	// the live 0x00439D65 pre-hook: TADR installs a VEH (InstallCrashTrace) that
	// runs ahead of frame-based SEH, so deliberately faulting there risks a
	// spurious crash report -- or actually terminating the process -- on every
	// single game launch. That cost is not acceptable for a self-test that runs
	// unconditionally at Install(). Calling the production router functions
	// directly instead exercises the exact same decision logic that will run for
	// real, with none of that risk, and is not weaker for it: it is a test of the
	// actual code, not a stand-in for it.
	//
	// A FAILED self-test aborts installation entirely -- logic that misbehaves on
	// controlled synthetic input is not trusted against live game data either.
	// ---------------------------------------------------------------------------
	bool SelfTestFix1Clamp()
	{
		WeaponStruct w;
		std::memset(&w, 0, sizeof(w));
		std::strncpy(w.WeaponName, "SELFTEST", sizeof(w.WeaponName) - 1);

		InlineX86StackBuffer buf;
		std::memset(&buf, 0, sizeof(buf));
		buf.Ebp = reinterpret_cast<DWORD>(&w);

		DWORD* typeMask = reinterpret_cast<DWORD*>(
			reinterpret_cast<BYTE*>(&w) + kWeaponTypeMaskOffset);
		WORD* reload = reinterpret_cast<WORD*>(
			reinterpret_cast<BYTE*>(&w) + kWeaponReloadTimeOffset);

		bool ok = true;

		// Degenerate stockpile weapon (absent/zero reloadtime) -> clamp 0 -> 1.
		*typeMask = WTM_Stockpile;
		*reload = 0;
		DWORD before = g_fix1Clamped;
		Fix1WeaponLoadEpilogueProc(&buf);
		ok = ok && (*reload == 1) && (g_fix1Clamped == before + 1);

		// Healthy stockpile weapon -> untouched, bit for bit (false-positive control).
		*typeMask = WTM_Stockpile;
		*reload = 150;
		before = g_fix1Clamped;
		Fix1WeaponLoadEpilogueProc(&buf);
		ok = ok && (*reload == 150) && (g_fix1Clamped == before);

		// Non-stockpile weapon with reloadtime==0 (a normal instant-hit weapon
		// legitimately has this) -> untouched (false-positive control: only stockpile
		// weapons divide by this field).
		*typeMask = 0;
		*reload = 0;
		before = g_fix1Clamped;
		Fix1WeaponLoadEpilogueProc(&buf);
		ok = ok && (*reload == 0) && (g_fix1Clamped == before);

		// The predicate operates on the STORED field, so the 65536-wrap case (plan
		// SS1.4/SS2.2) looks identical to the absent/zero case once it reaches here --
		// this exercises that it is still caught.
		*typeMask = WTM_Stockpile;
		*reload = 0;
		before = g_fix1Clamped;
		Fix1WeaponLoadEpilogueProc(&buf);
		ok = ok && (*reload == 1) && (g_fix1Clamped == before + 1);

		return ok;
	}

	bool SelfTestFix2SlotBounds()
	{
		// Synthetic weapons: one healthy (nonzero divisor), one mimicking
		// WeaponsTypedefArray[0] -- a perfectly readable, valid WeaponStruct whose
		// +0xE4 is 0. This is the EXACT shape PR #26's review found in all five real
		// production crashes: the index is valid, the pointer is valid, only the
		// divisor is zero.
		WeaponStruct healthyWeapon;
		std::memset(&healthyWeapon, 0, sizeof(healthyWeapon));
		*reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(&healthyWeapon) + kWeaponReloadTimeOffset) = 100;

		WeaponStruct sentinelWeapon;
		std::memset(&sentinelWeapon, 0, sizeof(sentinelWeapon));   // +0xE4 stays 0

		// Synthetic unit: only the three weapon-slot pointers matter here.
		BYTE unit[0x60];
		std::memset(unit, 0, sizeof(unit));
		*reinterpret_cast<WeaponStruct**>(unit + 0x10) = &healthyWeapon;   // slot 0
		*reinterpret_cast<WeaponStruct**>(unit + 0x2C) = &healthyWeapon;   // slot 1
		*reinterpret_cast<WeaponStruct**>(unit + 0x48) = &sentinelWeapon;  // slot 2

		UnitOrdersStruct order;
		std::memset(&order, 0, sizeof(order));

		InlineX86StackBuffer buf;
		std::memset(&buf, 0, sizeof(buf));
		buf.Edx = reinterpret_cast<DWORD>(unit);

		bool ok = true;

		// Slots 0 and 1: valid index, healthy weapon -> fall through untouched.
		for (DWORD idx = 0; idx <= 1; ++idx)
		{
			order.BuildUnitID = idx;

			buf.Esi = reinterpret_cast<DWORD>(&order);
			buf.rtnAddr_Pvoid = NULL;
			int rcSim = Fix2aSimBoundsProc(&buf);
			ok = ok && (rcSim == 0) && (buf.rtnAddr_Pvoid == NULL);

			buf.Eax = reinterpret_cast<DWORD>(&order);
			buf.rtnAddr_Pvoid = NULL;
			int rcHud = Fix2bHudBoundsProc(&buf);
			ok = ok && (rcHud == 0) && (buf.rtnAddr_Pvoid == NULL);
		}

		// Slot 2: valid index, but the resolved weapon's divisor is 0 -- the sentinel
		// shape. Must redirect, NOT fall through, even though the index itself is
		// perfectly valid. This is the exact case Fix 2 could not catch before the
		// 2026-09-14 fix, and the exact case that killed all five real players.
		order.BuildUnitID = 2;

		buf.Esi = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		DWORD beforeZDa = g_fix2aZeroDivisorRejects;
		int rcSimZ = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSimZ == X86STRACKBUFFERCHANGE)
			&& (buf.rtnAddr_Pvoid == reinterpret_cast<LPVOID>(kFix2aBailoutAddr))
			&& (g_fix2aZeroDivisorRejects == beforeZDa + 1);

		buf.Eax = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		DWORD beforeZDb = g_fix2bZeroDivisorRejects;
		int rcHudZ = Fix2bHudBoundsProc(&buf);
		ok = ok && (rcHudZ == X86STRACKBUFFERCHANGE)
			&& (buf.rtnAddr_Pvoid == reinterpret_cast<LPVOID>(kFix2bBailoutAddr))
			&& (g_fix2bZeroDivisorRejects == beforeZDb + 1);

		// Bad index (the smallest out-of-range value, 3) -> redirect to the bailout,
		// counted exactly once per consumer, and NOT counted as a zero-divisor reject
		// -- the index check must short-circuit before the weapon is ever resolved.
		order.BuildUnitID = 3;

		buf.Esi = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		DWORD before = g_fix2aRejects;
		DWORD beforeZD = g_fix2aZeroDivisorRejects;
		int rcSim = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSim == X86STRACKBUFFERCHANGE)
			&& (buf.rtnAddr_Pvoid == reinterpret_cast<LPVOID>(kFix2aBailoutAddr))
			&& (g_fix2aRejects == before + 1)
			&& (g_fix2aZeroDivisorRejects == beforeZD);

		buf.Eax = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		before = g_fix2bRejects;
		int rcHud = Fix2bHudBoundsProc(&buf);
		ok = ok && (rcHud == X86STRACKBUFFERCHANGE)
			&& (buf.rtnAddr_Pvoid == reinterpret_cast<LPVOID>(kFix2bBailoutAddr))
			&& (g_fix2bRejects == before + 1);

		// A wildly out-of-range value, as a genuinely corrupted field would produce,
		// not just "one past the end" -> same redirect.
		order.BuildUnitID = 0xFFFFFFFFu;
		buf.Esi = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		rcSim = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSim == X86STRACKBUFFERCHANGE);

		// A null order pointer -> redirect, not a dereference (SafeIsBadReadPtr(NULL)
		// is defined to be true).
		buf.Esi = 0;
		buf.rtnAddr_Pvoid = NULL;
		rcSim = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSim == X86STRACKBUFFERCHANGE);

		// A valid index but an unreadable unit pointer (buf.Edx = NULL) -> redirect,
		// not a dereference. This is the "unreadable weapon slot" half of
		// WeaponDivisorIsSafe, distinct from a readable-but-zero divisor.
		buf.Edx = 0;
		order.BuildUnitID = 0;
		buf.Esi = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		rcSim = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSim == X86STRACKBUFFERCHANGE);
		buf.Edx = reinterpret_cast<DWORD>(unit);

		return ok;
	}

	bool RunSelfTest()
	{
		struct Case { bool passed; const char* name; };
		const Case cases[] = {
			{ SelfTestFix1Clamp(),      "Fix1 clamp/no-clamp predicate (4 sub-cases)" },
			{ SelfTestFix2SlotBounds(), "Fix2 slot-bounds + zero-divisor predicate, both consumers "
			  "(healthy/sentinel-zero-divisor/bad/huge/null-order/null-unit)" },
		};
		const int total = sizeof(cases) / sizeof(cases[0]);
		int passed = 0;
		for (int i = 0; i < total; ++i)
		{
			IDDrawSurface::OutptFmtTxt("[BuildWeaponSlotGuard][selftest] %s: %s",
				cases[i].passed ? "PASS" : "FAIL", cases[i].name);
			if (cases[i].passed) ++passed;
		}
		IDDrawSurface::OutptFmtTxt("[BuildWeaponSlotGuard][selftest] %s: %d/%d",
			(passed == total) ? "PASS" : "FAIL", passed, total);
		return passed == total;
	}

	bool g_installed = false;
	std::unique_ptr<InlineSingleHook> g_fix1Hook;
	std::unique_ptr<InlineSingleHook> g_fix2aHook;
	std::unique_ptr<InlineSingleHook> g_fix2bHook;
}

namespace BuildWeaponSlotGuard
{
	void Install()
	{
		if (g_installed)
			return;
		g_installed = true;

#if !BUILD_WEAPON_SLOT_GUARD_ENABLE
		// Say so out loud, the same way GridClaimTieBreak's control arm does: a module
		// that failed its byte check would otherwise be indistinguishable in the log
		// from one that was compiled out entirely.
		IDDrawSurface::OutptTxt(
			"[BuildWeaponSlotGuard] DISABLED at compile time (BUILD_WEAPON_SLOT_GUARD_ENABLE 0) "
			"-- a stockpile weapon with an absent/zero reloadtime, or an out-of-range weapon-"
			"slot index on a BuildWeapon order, can still divide by zero / read out of bounds "
			"exactly as vanilla does today. This is the CONTROL arm.");
		return;
#else
		// Validate every site BEFORE installing any of them -- a partial application
		// (e.g. Fix 1 active but Fix 2 not) is not a state this project has reasoned
		// about, so an all-or-nothing gate is the only safe default. A byte mismatch
		// anywhere means this is not the exact TotalA.exe build these addresses were
		// verified against.
		bool ok = true;
		ok = CheckBytes(kFix1HookAddr, kFix1ExpectedBytes, sizeof(kFix1ExpectedBytes),
			"Fix1 weapon-load epilogue") && ok;
		ok = CheckBytes(kFix2aHookAddr, kFix2aExpectedBytes, sizeof(kFix2aExpectedBytes),
			"Fix2 sim hook") && ok;
		ok = CheckBytes(kFix2aBailoutAddr, kFix2aBailoutExpectedBytes,
			sizeof(kFix2aBailoutExpectedBytes), "Fix2 sim bailout target") && ok;
		ok = CheckBytes(kFix2bHookAddr, kFix2bExpectedBytes, sizeof(kFix2bExpectedBytes),
			"Fix2 HUD hook") && ok;
		ok = CheckBytes(kFix2bBailoutAddr, kFix2bBailoutExpectedBytes,
			sizeof(kFix2bBailoutExpectedBytes), "Fix2 HUD bailout target") && ok;
		if (!ok)
			return;

		if (!RunSelfTest())
		{
			IDDrawSurface::OutptTxt(
				"[BuildWeaponSlotGuard] DISABLED: self-test failed against synthetic data -- "
				"see [selftest] lines above. Refusing to trust this logic against live game "
				"data.");
			return;
		}

		// RunSelfTest() called the production routers directly against synthetic data,
		// which is the whole point (it exercises the exact code that will run for
		// real) -- but those routers increment these same module-level counters and
		// feed the same throttled heartbeat/log path production does. Left alone, a
		// live game's tdrawlog would forever carry a fixed synthetic baseline (2
		// clamps, several rejects) that a human -- or validate_bwsg_log.py -- reading
		// only the aggregate counters cannot tell apart from a real finding. Confirmed
		// live 2026-09-12: this baseline caused the log validator to report a false
		// "Fix 2 actually fired" WARN pointing at the self-test's own synthetic stack
		// address. Reset to a clean slate so every counter and every future heartbeat
		// reflects live gameplay only; the [selftest] and per-case log lines already
		// written stay in the log as proof the test ran, only the counters roll back.
		g_fix1Clamped = 0;
		g_fix2aRejects = 0;
		g_fix2bRejects = 0;
		g_fix2aZeroDivisorRejects = 0;
		g_fix2bZeroDivisorRejects = 0;
		g_fix2aSane = 0;
		g_fix2bSane = 0;
		g_lastHeartbeatMs = 0;

		g_fix1Hook.reset(new InlineSingleHook(
			kFix1HookAddr, kFix1HookLen, INLINE_5BYTESLAGGERJMP, Fix1WeaponLoadEpilogueProc));
		g_fix2aHook.reset(new InlineSingleHook(
			kFix2aHookAddr, kFix2aHookLen, INLINE_5BYTESLAGGERJMP, Fix2aSimBoundsProc));
		g_fix2bHook.reset(new InlineSingleHook(
			kFix2bHookAddr, kFix2bHookLen, INLINE_5BYTESLAGGERJMP, Fix2bHudBoundsProc));

		IDDrawSurface::OutptFmtTxt(
			"[BuildWeaponSlotGuard] installed: reload-divisor clamp @0x%08X, weapon-slot "
			"bounds checks @0x%08X (sim) and @0x%08X (hud)",
			kFix1HookAddr, kFix2aHookAddr, kFix2bHookAddr);
#endif
	}

	void Shutdown()
	{
		g_fix1Hook.reset();
		g_fix2aHook.reset();
		g_fix2bHook.reset();
		g_installed = false;
	}
}
