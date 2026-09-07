#include "SharePercent.h"

#if SHARE_PERCENT_ENABLE

#include "GameTickHook.h"
#include "hook/hook.h"
#include "iddrawsurface.h"
#include "tafunctions.h"
#include "tamem.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

// ---- struct-layout safety nets ---------------------------------------------------
//
// tamem.h is #pragma pack(1); these anchor the fields this module touches directly
// against the offsets independently re-derived from Cmd_SetShareMetalThreshold's and
// Cmd_SetShareEnergyThreshold's own address arithmetic in the disassembly (see
// ENGINE_NOTES.md SS41.2 for the full derivation: 331*id + 0x1C07 - 0x1B63 = 0xA4,
// etc). If any of these ever fire, tamem.h changed since this module was written and
// BOTH the raw hook addresses below and this module's field access are suspect --
// do not silently patch an offset here without re-deriving it from a fresh
// disassembly first.
static_assert(offsetof(TAdynmemStruct, LocalHumanPlayer_PlayerID) == 0x2A42, "TAdynmemStruct::LocalHumanPlayer_PlayerID moved");
static_assert(offsetof(TAdynmemStruct, WorkStatusMask)            == 0x2A44, "TAdynmemStruct::WorkStatusMask moved");
static_assert(offsetof(TAdynmemStruct, Players)                   == 0x1B63, "TAdynmemStruct::Players moved");
static_assert(offsetof(PlayerStruct, PlayerRes)                   == 0x8C,   "PlayerStruct::PlayerRes moved");
static_assert(offsetof(PlayerStruct, ShareMetal)                  == 0xE4,   "PlayerStruct::ShareMetal moved");
static_assert(offsetof(PlayerStruct, ShareEnergy)                 == 0xE8,   "PlayerStruct::ShareEnergy moved");
static_assert(offsetof(PlayerResourcesStruct, fMaxEnergyStorage)  == 0x18,   "PlayerResourcesStruct::fMaxEnergyStorage moved");
static_assert(offsetof(PlayerResourcesStruct, fMaxMetalStorage)   == 0x1C,   "PlayerResourcesStruct::fMaxMetalStorage moved");

namespace
{
	// ---- raw engine addresses, VERIFIED from disassembly (ENGINE_NOTES.md SS41) --
	const DWORD kSetShareMetalAddr  = 0x00419340u;   // Cmd_SetShareMetalThreshold
	const DWORD kSetShareEnergyAddr = 0x00419400u;   // Cmd_SetShareEnergyThreshold

	// Both prologues are `mov ecx, dword ptr [0x511DE8]` -- 6 bytes exactly, confirmed
	// byte-identical at both addresses. A 5-byte splice would split this instruction
	// (ENGINE_NOTES SS9.0a documents a real crash from exactly that class of mistake
	// on a DIFFERENT hook mode -- see the note on INLINE_SINGLEJMP below for why that
	// failure mode does not apply here regardless).
	const size_t kHookLen = 6;
	const BYTE kExpectedPrologue[kHookLen] = { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 };

	// Both handlers are reached ONLY through File_DispatchNamedCommand's function-
	// pointer table (0x004B7900) -- VERIFIED by ta.scan_call_refs() returning zero
	// direct `call`/`jmp` references to either address anywhere in .text, and by
	// ta.scan_dword() finding zero references landing inside the 6 bytes being
	// replaced. The dispatcher also ignores the handler's return value (it returns a
	// local initialised to 0 and never overwritten -- 0x004B7908/0x004B7A03). So
	// INLINE_SINGLEJMP (a plain `E9 rel32` + NOP pad, see hook/Hook.cpp) is safe here:
	// no trampoline, no relocator, no resume address to get wrong, and nothing can
	// land mid-instruction inside the splice.
	bool HasExpectedBytes(DWORD address, const BYTE* expected, size_t length)
	{
		if (std::memcmp(reinterpret_cast<const void*>(address), expected, length) == 0)
			return true;

		IDDrawSurface::OutptFmtTxt(
			"[SharePercent] disabled: unexpected TotalA.exe bytes at 0x%08X", address);
		return false;
	}

	// ---- opaque engine type: the pre-tokenized command line TA itself builds and
	// passes to every console-command handler before calling it. Treated as fully
	// opaque (matches the TaTdfFile pattern in UnitDefExtensions.cpp) -- this module
	// never reads its layout directly, only through the engine's own two accessors.
	struct TaTokenLine;

	// File_TokenLine_GetArgPtr @0x004B73C0 -- __thiscall(this, index, fallback),
	// ret 8. Bounds-checked against the token count at [this+0xD0]; returns
	// `fallback` verbatim when index is out of range. VERIFIED from disassembly.
	typedef char* (__thiscall* TokenLine_GetArgPtr_t)(TaTokenLine* self, int index, char* fallback);
	TokenLine_GetArgPtr_t TokenLine_GetArgPtr = reinterpret_cast<TokenLine_GetArgPtr_t>(0x004B73C0u);

	// File_TokenLine_AtoiAt @0x004B73E0 -- __thiscall(this, index, defaultValue),
	// ret 8. Same bounds check, then atoi() on the token. This is the SAME function
	// vanilla's own handlers call for argument index 1. VERIFIED from disassembly.
	typedef int (__thiscall* TokenLine_AtoiAt_t)(TaTokenLine* self, int index, int defaultValue);
	TokenLine_AtoiAt_t TokenLine_AtoiAt = reinterpret_cast<TokenLine_AtoiAt_t>(0x004B73E0u);

	// ---- state ----------------------------------------------------------------
	//
	// -1 = absolute/vanilla mode. 0..100 = the armed percentage. Local, static,
	// per-process -- nothing here is replicated or written into a demo. Reset to -1
	// at Install() and at the start of every new game (decision: no persistence
	// across games -- see OnGameTick).
	//
	// PER PLAYER SLOT, not a bare pair of globals -- found the hard way (2026-09-07)
	// after `+control` was used to test the feature, exactly as this project's own
	// test plan recommends. `+control` (Cheat_Control @0x416AB0, VERIFIED from
	// disassembly) writes `TAdynmem+0x2A42` -- LocalHumanPlayer_PlayerID -- directly,
	// which is the EXACT field LocalPlayerOrNull() resolves "the current player"
	// against. A single pair of globals meant: arm 70% while seated as player 0,
	// then `+control 1` to hand your ally a build order, and the very next tick
	// stamped PLAYER 1's ShareEnergy with 70% of PLAYER 1's OWN storage -- a value
	// player 1 never asked for and that has nothing to do with what player 0 wanted
	// to share. Switch back to player 0 and the next tick re-stamps player 0
	// correctly, but every player you pass through in between gets its own
	// ShareEnergy/ShareMetal clobbered by whatever the LAST command-typing player
	// armed. Indexed by player slot (0..9, matching Players[10]) fixes this: each
	// slot's arm is set only when a command is typed while that slot is local
	// (HandleSetShareThreshold), and the refresh tick (OnGameTick) walks every
	// armed slot and updates it from ITS OWN storage, independent of which slot is
	// currently local. No slot's state can leak into another's again.
	int g_metalPercent[10];
	int g_energyPercent[10];

	int g_lastGameTime = 0;
	bool g_wasInGame = false;   // edge-detects the (lobby/loading) -> TAInGame transition

	SingleHook* g_metalHook = nullptr;
	SingleHook* g_energyHook = nullptr;

	void ResetPercentModes()
	{
		for (int i = 0; i < 10; ++i)
		{
			g_metalPercent[i] = -1;
			g_energyPercent[i] = -1;
		}
	}

	int ClampPercent(long v)
	{
		if (v < 0)   return 0;
		if (v > 100) return 100;
		return static_cast<int>(v);
	}

	// Clamps a float to a range that can be cast to `long`/`int` without undefined
	// behaviour, for display purposes only. The values this module actually computes
	// and stores (DerivePercentValue's output) are always within [0, maxStorage] by
	// construction, so this only matters for a pathological maxStorage from unusual
	// mod data, or for reproducing vanilla's own unclamped message (see
	// HandleSetShareThreshold) when a player types a huge literal argument.
	long SafeLongForDisplay(float v)
	{
		const float kLimit = 2000000000.0f;   // comfortably inside int32 range
		if (v > kLimit)  return static_cast<long>(kLimit);
		if (v < -kLimit) return static_cast<long>(-kLimit);
		return static_cast<long>(v);
	}

	// Accepts ONLY "<digits>%" -- optional leading digits (at least one), exactly one
	// '%', nothing after it. No sign, no interior whitespace (the engine's own
	// tokenizer already split on whitespace before this ever sees the string, so a
	// token can never contain embedded spaces -- VERIFIED from
	// File_TokenizeLineWords's disassembly, which NUL-terminates each token at the
	// delimiter). Digit count is capped at 9 so the accumulator cannot overflow
	// before ClampPercent gets a chance to run -- independent of the clamp, not
	// relying on it.
	//
	// This is a strict allow-list, not a best-effort parser, on purpose: anything
	// ambiguous (a bare integer, "%", "50%%", "abc%") must fall through to vanilla
	// atoi() behaviour, never be half-interpreted as a percentage.
	bool ParsePercent(const char* raw, int* outPercent)
	{
		if (!raw || !*raw)
			return false;

		const char* p = raw;
		if (*p < '0' || *p > '9')
			return false;

		long acc = 0;
		int digitCount = 0;
		while (*p >= '0' && *p <= '9')
		{
			if (digitCount < 9)
				acc = acc * 10 + (*p - '0');
			else
				acc = 1000000000L;   // saturate; ClampPercent bounds it to 100 regardless
			++digitCount;
			++p;
		}

		if (*p != '%' || *(p + 1) != '\0')
			return false;

		*outPercent = ClampPercent(acc);
		return true;
	}

	// threshold = pct% of maxStorage. maxStorage <= 0 (including NaN, since any
	// comparison against NaN is false) yields 0 rather than NaN/Inf propagating into
	// a field four other systems read (the auto-share tick, the HUD marker, this
	// module's own dialog display, and the Delphi recorder's ShareEnergyVal --
	// ENGINE_NOTES SS41.9).
	float DerivePercentValue(float maxStorage, int percent)
	{
		if (!(maxStorage > 0.0f))
			return 0.0f;

		float value = maxStorage * (static_cast<float>(percent) / 100.0f);
		if (value > maxStorage)   // defensive; unreachable since percent is already
			value = maxStorage;   // clamped to <=100 by ParsePercent/ClampPercent
		if (value < 0.0f)         // defensive; unreachable since percent is already
			value = 0.0f;         // clamped to >=0 by ParsePercent/ClampPercent
		return value;
	}

	// Bounds-checked accessor for "my own player struct, right now". Returns nullptr
	// whenever the caller should do nothing -- game not running, no valid local
	// player slot, or that slot not active. Every one of these guards mirrors either
	// vanilla's own gate (WorkStatusMask bit 0 -- both original handlers test this
	// FIRST and do nothing at all if it is clear) or a defensive check vanilla does
	// NOT have (vanilla indexes Players[LocalHumanPlayer_PlayerID] with no range
	// check whatsoever -- see the id>9 comment below).
	// `outId`, when given, receives the resolved player-slot index (0..9) on
	// success -- callers that need to key the per-slot arrays above (rather than
	// just dereference the struct) use this instead of re-deriving the index.
	PlayerStruct* LocalPlayerOrNull(int* outId = nullptr)
	{
		TAdynmemStruct* ta = *TAmainStruct_PtrPtr;
		if (!ta)
			return nullptr;

		// Vanilla's own first gate, reproduced verbatim. With this bit clear, vanilla
		// does nothing at all -- not even the AtoiAt call -- so neither do we.
		if ((ta->WorkStatusMask & 1) == 0)
			return nullptr;

		// LocalHumanPlayer_PlayerID is a plain (signed) char with NO range check in
		// vanilla's own code before it is used to index Players[10] -- confirmed from
		// the disassembly of both original handlers. Casting to unsigned char BEFORE
		// comparing is deliberate: a hypothetical sentinel value like 0xFF (-1 as a
		// signed char) would pass a naive `id > 9` check on the signed value (since
		// -1 is not > 9) and then index Players[-1], a large out-of-bounds write.
		// Going through unsigned char makes any such value compare > 9 correctly.
		unsigned char id = static_cast<unsigned char>(ta->LocalHumanPlayer_PlayerID);
		if (id > 9)
			return nullptr;

		PlayerStruct* me = &ta->Players[id];
		if (!me->PlayerActive)
			return nullptr;

		if (outId)
			*outId = id;
		return me;
	}

	// Shared implementation for both replacement handlers. `isMetal` selects which
	// field/format-string pair to use -- everything else is byte-for-byte symmetric
	// between the two vanilla commands (ENGINE_NOTES SS41.8).
	void HandleSetShareThreshold(TaTokenLine* line, bool isMetal)
	{
		int id = -1;
		PlayerStruct* me = LocalPlayerOrNull(&id);
		if (!me)
			return;

		const float maxStorage = isMetal ? me->PlayerRes.fMaxMetalStorage
		                                  : me->PlayerRes.fMaxEnergyStorage;
		const char* resourceName = isMetal ? "metal" : "energy";
		// Arm the slot the command was typed AS (whoever is local right now -- the
		// same player `me` points at), not a bare global. See the state-block
		// comment above for why this matters the moment `+control` is in play.
		int* pct = isMetal ? &g_metalPercent[id] : &g_energyPercent[id];

		const char* raw = TokenLine_GetArgPtr(line, /*index=*/1, /*fallback=*/const_cast<char*>(""));

		int parsedPercent = 0;
		char msg[128];

		if (ParsePercent(raw, &parsedPercent))
		{
			*pct = parsedPercent;
			const float value = DerivePercentValue(maxStorage, parsedPercent);

			if (isMetal) me->ShareMetal  = value;
			else         me->ShareEnergy = value;

			_snprintf(msg, sizeof(msg) - 1, "OK.  Will share %s if above %d%% (%ld)",
				resourceName, parsedPercent, SafeLongForDisplay(value));
			msg[sizeof(msg) - 1] = '\0';
			NewChatText(msg, 2, 0, 10);
			return;
		}

		// Not a percentage -- clear percentage mode (this is what keeps a plain
		// `+setshareenergy 500`, the recorder's Alt+Shift+Z hotkey, and the
		// dialog.cpp quick-chat preset all working exactly as before) and reproduce
		// vanilla's own logic exactly:
		//
		//   n = AtoiAt(line, 1, 0)
		//   v = (float)n
		//   if (v > maxStorage) v = maxStorage      -- UPPER bound only, verified from
		//                                               the disassembly; vanilla does
		//                                               NOT clamp negative values, and
		//                                               neither do we here.
		//   Share* = v
		//
		// One deliberate departure from a naive port: vanilla's own compiled code
		// calls AtoiAt a THIRD time to fetch `n` again for the message, printing the
		// RAW unclamped input rather than the clamped stored value (confirmed at
		// 0x0041948D/0x004193CD -- a codegen artifact, not something to "fix" here,
		// since this module's contract is to reproduce vanilla's observable
		// behaviour exactly on this path). We call AtoiAt once and reuse the result,
		// which is behaviourally identical (same TokenLine object, same index, same
		// default -> deterministic) and avoids two redundant calls.
		*pct = -1;
		const int n = TokenLine_AtoiAt(line, /*index=*/1, /*defaultValue=*/0);
		float value = static_cast<float>(n);
		if (value > maxStorage)
			value = maxStorage;

		if (isMetal) me->ShareMetal  = value;
		else         me->ShareEnergy = value;

		_snprintf(msg, sizeof(msg) - 1, "OK.  Will share %s if above %ld",
			resourceName, SafeLongForDisplay(static_cast<float>(n)));
		msg[sizeof(msg) - 1] = '\0';
		NewChatText(msg, 2, 0, 10);
	}

	// ---- the two replacement entry points --------------------------------------
	//
	// Signature and calling convention (__stdcall, ret 4, one stack argument) match
	// the original functions exactly -- VERIFIED: File_DispatchNamedCommand pushes
	// exactly one argument (the TokenLine pointer) before calling through the
	// function-pointer table, and both original handlers end in `ret 4`.
	void __stdcall SetShareMetalThresholdReplacement(TaTokenLine* line)
	{
		HandleSetShareThreshold(line, /*isMetal=*/true);
	}

	void __stdcall SetShareEnergyThresholdReplacement(TaTokenLine* line)
	{
		HandleSetShareThreshold(line, /*isMetal=*/false);
	}

	// ---- per-tick refresh -------------------------------------------------------
	void OnGameTick(int gameTime)
	{
		// GameTickHook fires 9-15 times per game tick with an UNCHANGED gameTime
		// (ENGINE_NOTES.md SS35) -- this callback only recomputes and stores, so
		// multi-firing would be wasteful, not wrong, by SS35.3's own taxonomy. Guard
		// anyway: it costs one comparison and keeps this honest if the body grows.
		if (gameTime == g_lastGameTime)
			return;

		// GameTime restarting at or below the last value seen is read as "a new
		// game (or a replay) started" -- INFERRED, not measured (ENGINE_NOTES
		// SS41.10), so it is backed by the independent TAInGame-edge signal below
		// rather than relied on alone.
		if (gameTime < g_lastGameTime)
			ResetPercentModes();
		g_lastGameTime = gameTime;

		const bool inGame = (DataShare != nullptr) && (DataShare->TAProgress == TAInGame);
		if (inGame && !g_wasInGame)
			ResetPercentModes();
		g_wasInGame = inGame;

		bool anyArmed = false;
		for (int i = 0; i < 10; ++i)
			if (g_metalPercent[i] >= 0 || g_energyPercent[i] >= 0) { anyArmed = true; break; }
		if (!anyArmed)
			return;   // nothing armed anywhere -- skip the engine reads below entirely

		// `inGame` is defined above as `DataShare != nullptr && ...`, so by the time
		// `!inGame` is false here, DataShare is already known non-null -- safe to
		// dereference in the second half of this condition without re-checking.
		if (!inGame || DataShare->PlayingDemo)
			return;

		TAdynmemStruct* ta = *TAmainStruct_PtrPtr;
		if (!ta)
			return;
		// Vanilla's own "simulation running" gate, applied once for the whole pass
		// (it is a single flag on TAdynmem, not per-player -- unlike the arms below).
		if ((ta->WorkStatusMask & 1) == 0)
			return;

		// Walk every player slot with an armed percentage, independent of which slot
		// is currently local (LocalHumanPlayer_PlayerID). This is what keeps
		// `+control`-ing between seats from making one player's arm bleed into
		// another's ShareMetal/ShareEnergy -- see the state-block comment above.
		for (int i = 0; i < 10; ++i)
		{
			if (g_metalPercent[i] < 0 && g_energyPercent[i] < 0)
				continue;

			PlayerStruct* p = &ta->Players[i];
			if (!p->PlayerActive)
				continue;

			if (g_metalPercent[i] >= 0)
				p->ShareMetal = DerivePercentValue(p->PlayerRes.fMaxMetalStorage, g_metalPercent[i]);
			if (g_energyPercent[i] >= 0)
				p->ShareEnergy = DerivePercentValue(p->PlayerRes.fMaxEnergyStorage, g_energyPercent[i]);
		}
	}
}

namespace SharePercent
{
	void Install()
	{
		if (g_metalHook || g_energyHook)
			return;

		if (!HasExpectedBytes(kSetShareMetalAddr, kExpectedPrologue, kHookLen) ||
			!HasExpectedBytes(kSetShareEnergyAddr, kExpectedPrologue, kHookLen))
		{
			return;   // logged inside HasExpectedBytes; refuse to hook an unexpected build
		}

		ResetPercentModes();
		g_lastGameTime = 0;
		g_wasInGame = false;

		// INLINE_SINGLEJMP writes a plain `E9 rel32` (5 bytes) + NOP pad up to
		// kHookLen, and never touches the LAGGERJMP relocator that ENGINE_NOTES
		// SS9.0a documents a real crash from -- see the comment on kHookLen above
		// for why that failure mode does not apply to this hook.
		g_metalHook = new SingleHook(kSetShareMetalAddr, kHookLen, INLINE_SINGLEJMP,
			reinterpret_cast<LPBYTE>(&SetShareMetalThresholdReplacement));
		g_energyHook = new SingleHook(kSetShareEnergyAddr, kHookLen, INLINE_SINGLEJMP,
			reinterpret_cast<LPBYTE>(&SetShareEnergyThresholdReplacement));

		GameTickHook::GetInstance()->addCallback(OnGameTick);

		IDDrawSurface::OutptTxt("[SharePercent] installed");
	}

	void Shutdown()
	{
		delete g_metalHook;  g_metalHook = nullptr;
		delete g_energyHook; g_energyHook = nullptr;
		ResetPercentModes();
	}

	// Both report the CURRENTLY LOCAL player's armed state (-1 if that slot is not
	// in percent mode, or if there is no valid local player right now) -- these are
	// "what should the dialog show for whoever I am seated as right now", not a
	// lookup for an arbitrary slot. Unused outside this module at the moment; kept
	// for API completeness alongside GetDisplayPercent.
	int GetMetalPercent()
	{
		int id = -1;
		if (!LocalPlayerOrNull(&id))
			return -1;
		return g_metalPercent[id];
	}

	int GetEnergyPercent()
	{
		int id = -1;
		if (!LocalPlayerOrNull(&id))
			return -1;
		return g_energyPercent[id];
	}

	int GetDisplayPercent(bool isMetal)
	{
		int id = -1;
		PlayerStruct* me = LocalPlayerOrNull(&id);
		if (!me)
			return 0;

		const int armed = isMetal ? g_metalPercent[id] : g_energyPercent[id];
		if (armed >= 0)
			return armed;

		const float maxStorage = isMetal ? me->PlayerRes.fMaxMetalStorage
		                                  : me->PlayerRes.fMaxEnergyStorage;
		const float threshold  = isMetal ? me->ShareMetal : me->ShareEnergy;
		if (!(maxStorage > 0.0f))
			return 0;

		const long pct = static_cast<long>((threshold * 100.0f) / maxStorage);
		return ClampPercent(pct);
	}
}

#endif // SHARE_PERCENT_ENABLE
