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
	//   00402B7F  8B 46 36          mov eax,[esi+0x36]   ; esi = UnitOrdersStruct*
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

	DWORD g_fix2aRejects = 0;
	DWORD g_fix2bRejects = 0;
	DWORD g_fix2aSane = 0;   // valid-index calls that fell through untouched
	DWORD g_fix2bSane = 0;

	// A rejection is never silent, but a HEALTHY run produces no log line at all from
	// either fix -- which leaves no way to tell "this played a full session with the
	// path never exercised" apart from "this played a full session and it worked",
	// exactly the ambiguity the project's regression test (plan SS5 L3, "sane > 0,
	// else INDETERMINATE -- the path was never exercised") is designed to catch.
	// Matches the GetTickCount()-throttled idiom SoundLimitHeartbeat already uses in
	// TABugFix.cpp for the same reason.
	DWORD g_lastHeartbeatMs = 0;
	void MaybeHeartbeat()
	{
		const DWORD now = GetTickCount();
		if (g_lastHeartbeatMs != 0 && (now - g_lastHeartbeatMs) < 30000u)
			return;
		g_lastHeartbeatMs = now;
		IDDrawSurface::OutptFmtTxt(
			"[BuildWeaponSlotGuard][heartbeat] clamped=%lu sim(sane=%lu rejected=%lu) "
			"hud(sane=%lu rejected=%lu)",
			g_fix1Clamped, g_fix2aSane, g_fix2aRejects, g_fix2bSane, g_fix2bRejects);
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
	// Fix 2 routers -- one per consumer. Both share the same predicate: an order
	// pointer that is not safely readable, or a BuildUnitID outside {0,1,2}, is
	// treated identically -- redirect to that function's own existing bail-out
	// rather than let anything dereference an out-of-range slot.
	// ---------------------------------------------------------------------------
	int __stdcall Fix2aSimBoundsProc(PInlineX86StackBuffer buf)
	{
		const UnitOrdersStruct* order = reinterpret_cast<const UnitOrdersStruct*>(buf->Esi);

		const bool readable = !SafeIsBadReadPtr(
			order, offsetof(UnitOrdersStruct, BuildUnitID) + sizeof(DWORD));
		const DWORD idx = readable ? order->BuildUnitID : 0xFFFFFFFFu;

		if (readable && idx <= kMaxValidWeaponSlot)
		{
			++g_fix2aSane;
			MaybeHeartbeat();
			return 0;
		}

		++g_fix2aRejects;
		CrashTrace_RecordEvent(TRACE_CAT_BWSG, reinterpret_cast<DWORD>(order), idx,
			readable ? 1u : 0u, 0);
		if (ShouldLog(g_fix2aRejects))
		{
			IDDrawSurface::OutptFmtTxt(
				"[BuildWeaponSlotGuard] sim: BAD weapon slot order=%08X readable=%d "
				"idx=%lu -- bailing out to 0x%08X instead of an out-of-bounds unit read/"
				"write (#%lu)",
				reinterpret_cast<DWORD>(order), (int)readable, idx, kFix2aBailoutAddr,
				g_fix2aRejects);
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

		if (readable && idx <= kMaxValidWeaponSlot)
		{
			++g_fix2bSane;
			MaybeHeartbeat();
			return 0;
		}

		++g_fix2bRejects;
		CrashTrace_RecordEvent(TRACE_CAT_BWSG, reinterpret_cast<DWORD>(order), idx,
			readable ? 1u : 0u, 1);
		if (ShouldLog(g_fix2bRejects))
		{
			IDDrawSurface::OutptFmtTxt(
				"[BuildWeaponSlotGuard] hud: BAD weapon slot order=%08X readable=%d "
				"idx=%lu -- bailing out to 0x%08X instead of reading garbage as a "
				"WeaponStruct* (#%lu)",
				reinterpret_cast<DWORD>(order), (int)readable, idx, kFix2bBailoutAddr,
				g_fix2bRejects);
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
		UnitOrdersStruct order;
		std::memset(&order, 0, sizeof(order));

		InlineX86StackBuffer buf;
		std::memset(&buf, 0, sizeof(buf));

		bool ok = true;

		// Valid indices 0,1,2, through BOTH consumers -> always fall through untouched.
		for (DWORD idx = 0; idx <= kMaxValidWeaponSlot; ++idx)
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

		// Bad index (the smallest out-of-range value, 3) -> redirect to the bailout,
		// counted exactly once per consumer.
		order.BuildUnitID = 3;

		buf.Esi = reinterpret_cast<DWORD>(&order);
		buf.rtnAddr_Pvoid = NULL;
		DWORD before = g_fix2aRejects;
		int rcSim = Fix2aSimBoundsProc(&buf);
		ok = ok && (rcSim == X86STRACKBUFFERCHANGE)
			&& (buf.rtnAddr_Pvoid == reinterpret_cast<LPVOID>(kFix2aBailoutAddr))
			&& (g_fix2aRejects == before + 1);

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

		return ok;
	}

	bool RunSelfTest()
	{
		struct Case { bool passed; const char* name; };
		const Case cases[] = {
			{ SelfTestFix1Clamp(),      "Fix1 clamp/no-clamp predicate (4 sub-cases)" },
			{ SelfTestFix2SlotBounds(), "Fix2 slot-bounds predicate, both consumers (valid/bad/huge/null)" },
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
