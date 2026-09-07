#pragma once

#include "config.h"

#if SHARE_PERCENT_ENABLE

// Percentage-based resource share thresholds (Escalation only).
//
// THE PROBLEM: `+setsharemetal <n>` / `+setshareenergy <n>` (Cmd_SetShareMetalThreshold
// @0x00419340, Cmd_SetShareEnergyThreshold @0x00419400) write an ABSOLUTE float in
// resource units to PlayerStruct::ShareMetal / ::ShareEnergy, and nothing in the engine
// ever re-derives it when max storage changes. A threshold set at 500 when max energy
// storage is 1,000 (50% retained) is still 500 once storage reaches 26,000 (1.9%
// retained). See ai-reference/share-resources-percentage/CLAUDE.md and ENGINE_NOTES.md
// SS41 for the full derivation.
//
// THE FIX: accept a `%` suffix on both existing commands (`+setshareenergy 50%`) and,
// once armed, keep the absolute field in sync with max storage every tick. A plain
// integer argument (no `%`) clears percentage mode and reproduces vanilla exactly --
// this is what keeps `+setshareenergy 500` and the recorder's Alt+Shift+Z hotkey
// (KeyboardHook.pas) working unchanged.
//
// WHY THIS CANNOT DESYNC: ShareMetal/ShareEnergy are read by exactly one function,
// Share_AutoAllyResourcesAndMap (0x00457D30), which has exactly one caller and is
// passed only the LOCAL player -- every client evaluates its own threshold and nobody
// else's. The resulting transfer is put on the wire as an explicit float; nothing
// recomputes it from a threshold. And a `+command` received over the network is never
// executed -- Cmd_DispatchLineText (0x00417B50) has exactly two callers, both local UI.
// So this module writes one float on one player struct on the machine that typed the
// command, and nothing it does is compared against, or has to agree with, any other
// client. See ENGINE_NOTES.md SS1 X-24 and SS41.6.
//
// HOOK STRATEGY: both command handlers are replaced WHOLE via SingleHook +
// INLINE_SINGLEJMP (a plain `E9 rel32` + NOP pad -- see hook/Hook.cpp), not an
// entry-hook-and-cancel. ENGINE_NOTES SS9.0a documents a reproducible crash from the
// LAGGERJMP relocator on a non-5-byte guard; INLINE_SINGLEJMP never calls that
// relocator at all, so the whole failure mode is structurally absent. Verified before
// building this: neither handler address has any direct `call` reference anywhere in
// the image, and no reference lands inside the 6 bytes being replaced (both are
// call-table-dispatched only, via File_DispatchNamedCommand @0x004B7900, which also
// ignores the handler's return value) -- so nothing can jump into a spliced
// instruction and nothing depends on the handler returning a particular value.
//
// SCOPE: local-only, per-client display/config-adjacent state. No new command name
// (avoids the level-4 unit-spawn cheat's fallback -- ENGINE_NOTES SS32.3), no
// SHARE.gui changes, no change to the share rate/amount formula, no persistence
// across games -- every new game starts in vanilla absolute-threshold-0 mode.
//
// KNOWN VANILLA LIMITATION, not something this module can affect: auto-share
// (Share_AutoAllyResourcesAndMap @0x00457D30) only ever transfers to a candidate
// whose My_PlayerType == 3 (Player_RemoteHuman) -- VERIFIED from disassembly at
// 0x00457DD6/0x00457EF4, independently corroborated by GridClaimTieBreak.h:19's
// annotation of the same byte at the same offset. A local AI ally (My_PlayerType
// == 2) can never receive a share, no matter what threshold is armed. This is
// vanilla behaviour that predates this module.
//
// It therefore CANNOT be exercised in a skirmish against AI allies at all --
// a second human client over the network is the only way to see auto-share
// fire. In particular the engine's own `+ai` command does NOT help: Cheat_AI
// @0x00416280 only ever calls Player_SetPlayerType(@0x00463C60) with 1
// (LocalHuman) or 2 (LocalAI) -- VERIFIED at 0x004162F2/0x004162FC -- and
// neither value satisfies the `== 3` gate. Only the lobby/network join path
// produces My_PlayerType == 3. See ENGINE_NOTES.md SS50.2.

namespace SharePercent
{
	// Installs the two command-handler hooks and registers the per-tick refresh
	// callback. Idempotent. Verifies both handlers' prologue bytes before hooking;
	// if TotalA.exe does not match the expected build, installation is skipped and
	// logged rather than corrupting unrelated code.
	void Install();

	// Restores both handlers and clears all state. Safe to call multiple times.
	void Shutdown();

	// -1 = absolute mode (vanilla). 0..100 = the armed percentage. For the H-dialog
	// (sharedialog.cpp) to render the slider/label; the dialog also uses these to
	// derive a display-only percentage from the current absolute threshold when not
	// armed (see GetDisplayPercent).
	int GetMetalPercent();
	int GetEnergyPercent();

	// Returns the percentage the dialog should SHOW for one resource: the armed
	// percentage if set, otherwise the current threshold expressed as a percentage
	// of max storage (0 if max storage is not positive). Always clamped to 0..100.
	// `isMetal` selects metal (true) or energy (false).
	int GetDisplayPercent(bool isMetal);
}

#endif // SHARE_PERCENT_ENABLE
