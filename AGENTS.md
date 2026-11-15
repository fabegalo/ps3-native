# Guide for AI maintainers

Single policy for Claude, Codex and other agents. `CLAUDE.md` is only a pointer here. Read the current request, then only relevant code and the current blocker. Historical reports describe evidence, not permanent architecture restrictions.

## 1. Shared architecture — mandatory

**x360-native is the reference architecture for x360-native, ps2-native, xone-native and ps3-native. All four share the same seven logical boundaries, contracts and original-system -> game -> original-system lifecycle, not identical CPU or platform implementations.**

CPU, firmware and service implementations are console-specific; no second runtime, invented original UI or duplicate system state.

| Module | Responsibility and current location |
| --- | --- |
| Management launcher | `src/launcher.cpp`, `src/ps3native/cli.py`: import, inspect, prepare and start; never imitate XMB. |
| Content/catalog | `src/ps3native/catalog.py`: original identities, revisions, hashes, firmware and dependencies without copying game trees. |
| Preparation and native execution | `src/ps3native/preparation.py`, CMake and upstream PPU/SPU lifters; generation is distinct from a compiled, runnable module. |
| Compatibility runtime + HLE | Pinned ps3recomp LV2, PRX, memory, threads, sync, filesystem, users, audio and input; no second runtime. |
| Original GPU / Native GPU | Upstream RSX/backends; both modes must share guest state, effects and timing. PC enhancements are not implemented yet. |
| Original system and games | Original VSH/XMB and games; original code/resources stay local and separate from HLE. This integration remains pending. |
| Session supervisor / host adapters | CLI host launch today; original system -> game -> system, persistence and recovery remain pending. |

**Universal compatibility rule:** A newly tested title is evidence about shared CPU/AOT, GPU, kernel/HLE, devices and session behavior, not a separate game port. Fix the demonstrated root cause in the shared component and add a reusable regression; use title/revision-specific metadata or behavior only where an original difference is verified. Never treat an imported or compiled title as playable without end-to-end evidence.

Windows x64/ARM64, Linux x64/ARM64, macOS Intel/Apple Silicon are targets;
Steam Deck uses Linux. A successful build or fixture is not gameplay certification.

## 2. Architecture requires maintainer validation

The initial maintainer request approves ps3recomp as the PPU/SPU AOT, runtime,
HLE and RSX base, these seven boundaries, read-only import, original firmware
study and Windows x64 first. Do not ask again for those settled choices.
Before a NEW architectural choice, present the current implementation, proposed
boundary, alternatives, tradeoffs and shared-architecture impact; ask the
maintainer to validate it and wait. Continue independent approved work.
Interpreter/JIT fallback, a new renderer, runtime replacement, firmware/HLE
ownership changes and process-model changes require that explicit evaluation.
Upstream containing an interpreter or successful HLE placeholders does not
authorize using them to claim production support. No silent fallback.
Routine fixes within approved boundaries need no repeated approval.

## 3. Repository implementation map

- Pinned upstream: `external/ps3recomp/`, `deps.lock.json`, `patches/ps3recomp/` and `src/ps3native/dependency.py`; patches are hash-locked, not an alternate runtime.
- Startup diagnostics: `src/ps3native/boot_subset.py`, `src/ps3native/codegen.py`, `src/ps3native/session.py`, `src/native_guard.cpp`, `src/module_hooks.cpp` and `cmake/NativeModule.cmake`; a diagnostic PPU entry is not gameplay.
- Original VSH/XMB, module bindings and the system -> game -> system supervisor remain integration targets; preserve the upstream RSX contract.

## 4. Implementation, correctness and validation

**Ponytail `full` means the smallest complete outcome, not the smallest isolated diff.** Establish the requested functional acceptance path before editing. Fix the first proven shared blocker, inspect affected callers and tests, then continue through related blockers in that same authorized scope until the behavior works or a real limit stops progress (missing authorized inputs, permissions, architecture approval, environment or budget). Do not repeatedly replan, ask again about settled decisions or end a functional task with only a diagnosis, a successful build or a synthetic fixture. Do not expand into unrelated features. If blocked, commit only independently correct, tested progress and report the precise remaining boundary; never invent evidence.

**Validation cadence:** During investigation use the cheapest focused compile, negative control or regression that can falsify the hypothesis, reusing incremental builds and existing artifacts. Validate the affected integration path after coherent fixes when real inputs and permissions are available; reserve broad test matrices and CI for affected integration or publication boundaries, not every small edit. Never rerun builds or game sessions for instructions-only changes. Keep guest-visible state, timing, output and renderer fidelity intact.

**Only skill: [Ponytail](https://github.com/DietrichGebert/ponytail/blob/main/skills/ponytail/SKILL.md), in `full` mode for coding tasks.** Read its installed copy when available, otherwise consult upstream without cloning. If unavailable, say so and follow its essentials; do not claim it ran or activate other project skills. Inspect affected callers and actual upstream code first; reuse functioning components. Preserve the upstream base at
`deps.lock.json` and the submodule gitlink; only exact hash-verified locked
patches may modify it. `dependency.py` applies/verifies them at configuration.
Prefer upstream fixes/pin updates;
necessary generic adaptations require versioned patches with patch, before and
after hashes in the lock, never unrecorded submodule edits or another fork.

Keep originals read-only; preserve settings, saves, users, ownership, Unicode
paths and valid caches. Validate lengths, offsets, paths and revision hashes.
Do not execute content-provided scripts. No invented hardware values, success
stubs, fake tasks, skipped draws, weakened assertions or fake online/entitlement.
Preserve PPU/SPU/RSX coherence, endian/ABI, FP, synchronization and timing.
No unsupported global SIMD/fast-math assumptions beyond explicit upstream needs.

Build affected Release targets with the installed toolchain; do not duplicate
toolchains. Rebuild outputs invalidated by headers, ABI, compiler, codegen,
shader or input changes. Verify real results with focused regressions and
negative controls. Report synthetic, real GPU, software GPU, platform build,
original boot and gameplay evidence separately, naming the exact dependency pin.
Never call imported/encrypted firmware executable. Keep original LLE modules,
their exports/imports and provenance separate from HLE registrations.

## 5. Disk and test-artifact budget

Inspect available space before large downloads/builds; estimate new bytes and
file counts. Use one incremental build per necessary toolchain/configuration
and one owned scratch area. Default diagnostic budget is **1 GiB / 200 files**
per task, with bounded duration/logs. Build products still need capacity planning.
Larger diagnostics need maintainer approval. Stop safely at limits; never label
truncated or incomplete evidence a pass. Keep only compact evidence and useful
reproducers. Delete only verified task-owned disposable paths; verify resolved
absolute deletion targets first. No blanket build/cache deletion or copied
game trees, archives, toolchains, backup trees or unnecessary worktrees.

## 6. Documentation and token budget

Do not create extra Markdown reports or duplicate roadmaps without a request.
Update the shortest authoritative section; README translations describe public
use and the current blocker. Keep this guide stable and concise. Instructions-only
changes need content/reference checks, not game sessions or repeated CI runs.

## 7. Publication and provenance

Work directly on **main**, no PR. Use English Conventional Commits and identifiers;
reply in the user's language. No assistant co-author/footer. Preserve concurrent
changes, upstream credits and licenses. Own code is BSD-3-Clause; ps3recomp is MIT
and carries additional third-party licenses, including its default LGPL ATRAC
decoder. RPCS3 is a technical/behavioral reference; do not copy its code under BSD.
No proprietary SDKs, games, firmware, keys, personal state or proprietary generated
code in Git, CI artifacts or distribution. Use own synthetic fixtures unless
authorized originals are available. No ZIP, Release, tag, force-push, visibility
change, installation or changes to other repositories without authorization.
