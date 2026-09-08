#pragma once

// Replaces the COB bytecode interpreter's opcode-dispatch ladder -- a 28-node,
// 141-instruction linear compare chain over 57 opcodes -- with a 256-entry jump
// table. Measured at 19-23% of main-thread wall time across three independent
// profile captures of real late-game matches (ENGINE_NOTES.md S1 X-8/X-9, SS24,
// SS27). Full derivation, the equivalence argument, and the independent
// re-derivation that verified the opcode map, the splice-window signature, and
// EDX/EFLAGS register liveness at all 57 handler entries:
// ai-reference/simulation-performance/COB_DISPATCH_PROJECT.md. Reproduce all of
// that verification against the shipped binary with
// ai-reference/tools/exe/cob_dispatch.py --verify.
//
// CLASS A PATCH: bit-identical simulation output, purely faster. The table
// reproduces vanilla's opcode-match decision exactly (same 57 opcodes accepted,
// same error path for everything else) and every one of the 57 handlers has EDX
// and EFLAGS dead at entry, so leaving different values in them changes nothing
// observable. Nothing here is keyed on pointer values, timing, or thread
// identity, and there is no new floating point.
//
// This is a raw byte splice into the middle of UnitScript_ExecSlot, not an
// InlineSingleHook: the window has no function-call boundary and no return path
// to preserve, so the hook framework's register-save-frame machinery would be
// pure overhead. Install() verifies the original 38 bytes match exactly before
// writing anything, and refuses to patch (leaving the vanilla ladder running) on
// any mismatch -- the only failure mode, and it fails safe.
//
// STATUS (see "Where this stands" in COB_DISPATCH_PROJECT.md before enabling this
// anywhere but a private test build):
//   - Opcode map and splice-window signature: independently re-derived, verified.
//   - EDX dead at handler entry: 57 of 57 PROVEN (closed 2026-09-08). The last
//     one, opcode 0x10045000 -> handler 0x4B16C4's `call [eax+0x4C]`, is an
//     indirect call no walk can follow, so it was closed by resolving the vtable
//     instead: [edi] is UnitScriptContext, and only two vtables in the image can
//     be it -- 0x4FD698 (concrete, +0x4C = 0x481470) and 0x4FDB00 (abstract base,
//     +0x4C = 0x4B06A0). Both are leaf functions with zero EDX access.
//     ENGINE_NOTES.md SS27.6.
//   - Differential replay against a recorded demo: ABANDONED, not deferred. Replay
//     watching is not bit-reproducible on this engine even same-build-vs-itself
//     (ENGINE_NOTES.md SS15.5, diverged at tick 28). Do not retry it.
//   - Performance A/B measurement: NOT RUN, and out of scope -- the DLL ships to
//     every player, so there is no unpatched arm to measure against. The 12-20%
//     wall-clock estimate in the project doc is a GUESS bounded above by ~23%, not
//     a measurement, and must not be quoted as one. Note also that the sim is
//     cadence-paced at 30.000 Hz (SS39.1), so freed time reappears as frames
//     rather than headroom (SS23.2 trap 6): state any payoff as frame rate or
//     unit-count headroom, never as "N% less CPU".
//   - Solo soak: PASSED 2026-09-08. Fresh skirmish, ~95 minutes, escalating to
//     heavy late-game combat (the engine's own explosion-count telemetry cap
//     saturated twice under real fire). No crash, no exception, clean shutdown,
//     no anomaly reported by the tester watching the game directly (the class
//     of bug -- wrong animation, silently non-firing weapon -- that neither a
//     log nor this file can see; that is what the solo soak is for).
//   - Install-time self-check: PASSES ON EVERY LAUNCH. Install() reads the
//     window back out of live memory, re-derives the embedded table pointer from
//     that readback and walks all 256 entries, logging one line to tdrawlog.txt.
//     No Cheat Engine needed. This exists because SingleHook::Hook() returns
//     void -- without it, a failed splice would be silent.
//   - Multiplayer soak test: SKIPPED 2026-09-08, deliberately, with every other
//     item green. What that costs, stated honestly: MP was never the correctness
//     oracle, because with every peer on the same binary lockstep desync detects
//     nondeterminism, not incorrectness -- a mis-mapped opcode would make all
//     peers wrong identically and silently. The oracle is the static 57/57 case
//     above, and that one is complete. What the soak WOULD have screened is
//     crashes and gross anomalies under real network timing, in a DLL that also
//     carries upstream subsystems this patch had never shared a process with
//     until the 2026-09-08 rebase. That screen is absent, not passed.
//     ENGINE_NOTES.md SS27.8; project CLAUDE.md, "What was skipped".
//   - Before you touch the enable flag, know this: ChallengeResponse hashes this
//     DLL (it locates itself by address, so renaming it changes nothing), so
//     every peer needs the byte-identical file or they get a HUD verification
//     warning -- and because random_code_seg_keys.h is regenerated on every
//     build, two people building this same commit do NOT produce the same DLL.
//     Distribute the built artifact, not the recipe. ENGINE_NOTES.md SS29.7.
// Gated behind COB_DISPATCH_TABLE_ENABLE. ON for Escalation as of 2026-09-08;
// config.h defaults it to 0, so every other config stays off unless someone
// re-verifies the splice window against that config's own exe.

namespace CobDispatchTable {

    // Verifies the 38-byte splice-window signature at 0x4B0E69 and, only if it
    // matches exactly, splices in the jump-table dispatch. Idempotent -- a second
    // call is a no-op. Logs and leaves the engine untouched if the signature does
    // not match (wrong build) or if a table entry is ever found to point outside
    // the VM region (a bug in the table, not a build mismatch).
    //
    // On a successful install, also reads the window straight back out of live
    // memory and logs one self-check line to tdrawlog.txt (added 2026-09-08 --
    // "Tier A" in COB_DISPATCH_PROJECT.md): confirms the write actually landed
    // (SingleHook::Hook() is void and surfaces no failure), that the embedded
    // jump-table pointer resolves to &g_table[0], and the 256-entry error-path
    // vs handler count. This runs on EVERY launch with the feature flag on --
    // no CE attach needed to confirm the patch installed. It is not a substitute
    // for checkPatchState() (ai-reference/simulation-performance/
    // cob_dispatch_test_helper.lua): that one reads from outside the process,
    // this one is the module grading its own homework.
    void Install();

    // Restores the original 38 bytes and releases the hook. Safe to call even if
    // Install() never patched anything (signature mismatch, or never called).
    void Shutdown();

    // There is deliberately no runtime enable/disable toggle. One existed
    // (SetEnabled/IsActive) to make a same-process perf A/B possible; it never
    // acquired a caller, that measurement is out of scope (see STATUS), and
    // rewriting 38 bytes of live code underneath a running VM is a hazard with no
    // remaining purpose. Install() at startup, Shutdown() at teardown, nothing else.

}
