# ps3-native

[Português brasileiro](README.pt-BR.md)

Generic PlayStation 3 static AOT recompilation and native execution research,
built on [ps3recomp](https://github.com/sp00nznet/ps3recomp), by Ned Heller /
sp00nznet. The goal is original games and the **original VSH/XMB**, including
system → game → system transitions. The management CLI is not an imitation XMB.

**M0 bootstrap:** own PPU and SPU fixtures run natively, and an upstream RSX
test verifies pixels on D3D12. Commercial games and original XMB do not run
through this project's launcher yet. No compatibility claims follow from a build.

## Build and check

Requirements: Git, CMake 3.25+, Ninja, Python 3.11+ and Clang with a C/C++20
toolchain. On Windows use **clang-cl**, MSVC headers/libraries and Windows SDK;
`cl.exe` alone cannot compile the upstream builtins. Reuse an installed toolchain.
On Linux install SDL2 development files; on macOS install SDL2 (e.g. Homebrew).
The checked-in CI builds Windows, Linux and macOS; ARM targets remain planned.

```sh
git clone --recurse-submodules https://github.com/fabegalo/ps3-native.git
cd ps3-native
# Existing checkouts: git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

Windows, from an x64 developer prompt with clang-cl, CMake and Ninja on PATH:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
.\build\ps3-native.exe --help
```

`scripts/devshell.cmd` can initialize an existing Visual Studio x64 environment;
it does not install tools or change the machine environment. This source-tree
launcher delegates to the configured Python interpreter and CLI. It is not yet
a relocatable end-user distribution. No ZIP or Release is produced in M0.

## Command contracts

On POSIX the launcher is `build/ps3-native`; Python can also invoke the same CLI:

```sh
python src/ps3native/cli.py inspect "/path/to/game/PS3_GAME"
python src/ps3native/cli.py --catalog .local/catalog import "/path/to/PS3UPDAT.PUP"
python src/ps3native/cli.py prepare "/path/to/readable/EBOOT.elf"
python src/ps3native/cli.py prepare --fixture ppu
python src/ps3native/cli.py run --fixture ppu
python src/ps3native/cli.py prepare --fixture spu
python src/ps3native/cli.py run --fixture spu
python src/ps3native/cli.py system
```

Suggested local layout: `content/games/<title>/PS3_GAME/` and
`firmware/PS3UPDAT.PUP`, both ignored by Git. Originals may also stay on another
drive; pass their existing paths to `import` without moving or duplicating them.

| Command | Actual M0 behavior |
| --- | --- |
| `inspect PATH` | Read-only recursive inventory, file sizes/SHA-256, PARAM.SFO metadata, PUP table/version text and ELF/SELF/PKG identification. JSON output. |
| `import PATH` | Same inventory, atomically saves a manifest outside originals; references original paths, never copies game trees. Identical imports reuse the content ID. |
| `prepare PATH [--cache PATH]` | Single readable big-endian PPU ELF64 ET_EXEC: upstream parsing, OPD/TOC/import analysis and actual PPU AOT C generation. Receipts hash input, adapter, upstream and outputs. Returns **3** because general native build/dependency certification remains blocked. |
| `prepare --fixture ppu\|spu [--build-dir PATH]` | CMake generates, compiles and links the explicitly synthetic fixture with the existing upstream runtime. Incremental compiler/header/tool dependencies are tracked by CMake/Ninja. |
| `run --fixture ppu\|spu` | Checks locked upstream and fixture hashes, executes the compiled synthetic fixture and returns its real result. |
| `run PATH` | Inventories current bytes and returns **3**, with a concrete unsupported-execution diagnostic. |
| `system` | Returns **3**: original VSH/XMB boot and system/game lifecycle are not implemented. |

Exit codes: **0** completed operation; **2** invalid/missing input, I/O or tool
failure; **3** unavailable execution/preparation stage. Fixture preparation/run
preserves compiler/process error codes. Content IDs include relative names,
sizes, kinds and file hashes; no signed firmware authenticity is claimed.
Links/junctions and malformed SFO/PUP bounds are rejected. A codegen cache hit
rechecks all recorded output hashes; changed input or adapter uses another cache.
Interrupted stages never replace valid published caches. M0 temporarily stages
one executable (maximum 256 MiB); it does not stage entire games. Treat originals
as stable during inspection; this is not a filesystem snapshot or security sandbox.

## Architecture and dependency

The seven shared boundaries follow
[x360-native](https://github.com/fabegalo/x360-native), with
[ps2-native](https://github.com/fabegalo/ps2-native) and
[xone-native](https://github.com/fabegalo/xone-native) as companion references:
management launcher; content/catalog; preparation/native execution; compatibility
runtime/HLE; Original GPU/Native GPU; original system/games; session supervisor/
host adapters. Their current ownership and approval rules are in [AGENTS.md](AGENTS.md).

The pristine submodule and `deps.lock.json` fix ps3recomp to
**`de4f090328ea08bef6b2f15caec0b85e343a7ece`** (upstream HEAD inspected on
2026-11-14). No upstream patches or copied runtime are introduced. PPU register,
VMX, endian, OPD/TOC and memory behavior, and SPU local store, DMA and channels
come from upstream. These tiny fixtures do not certify complete ISA, exceptions,
SPURS, synchronization or floating-point fidelity. Upstream has interpreter and
title-specific paths: they are not approved production fallbacks in ps3-native.

| Backend | Upstream availability / current evidence |
| --- | --- |
| D3D12 | Windows; real pixel regression locally validated on NVIDIA RTX 4070. |
| Vulkan | Linux-only opt-in `-DPS3RECOMP_RSX_VULKAN=ON`; requires Vulkan headers, guest shaders need glslang. Not locally validated. |
| Metal | Apple-only; guest shader translation needs glslang + spirv-cross. Not locally validated. |

Original fidelity and Native enhancements must share RSX contracts and coherent
PPU/SPU/RSX state. M0 adds no renderer or enhanced GPU mode. Hosted CI excludes
the GPU runtime test because its runners have no certified D3D12 adapter; the
test remains mandatory on an appropriate local GPU host.

## Validation and the next real blocker

Local Windows x64 Release, Clang 22.1.8, CMake 3.31.6, Python 3.14.8:

- PPU: own PPC64 ELF → upstream OPD analysis/lifter → native compilation →
  upstream ELF loader/dispatch → big-endian result **42**. Wrong-result control fails.
- SPU: own instructions → upstream lifter → native execution → real MFC GET,
  four-word increment and PUT; all **16 bytes** checked in LS and main memory.
  Corrupt-input control fails. No interpreter is invoked by this straight-line test.
- RSX: upstream NV4097/guest vertex and fragment program draw, D3D12 hardware
  render/readback; four Z16/Z24S8 depth-clear cases, **256 pixels per case**.
  Three draws must be white; the occluded draw must remain black.
- Catalog/preparation: SFO/PUP bounds, Unicode paths, idempotency, originals,
  revisions, actual generated-code changes, cache reuse/tampering and unavailable
  commands. A symlink case is explicitly skipped if the host lacks link permission.

No commercial game, firmware or Sony SDK was used. Windows x64 is the local
validated build; Linux/macOS evidence belongs to the CI run for the actual commit.
Windows/Linux ARM64, macOS Intel/Apple Silicon coverage and Steam Deck runtime
must be reported separately from the CI runner's particular architecture.

**Next task:** implement a fail-closed generic module completeness/binding gate
before building a user module: unresolved calls/NIDs, unsupported instructions,
PRX relocation/dependencies and SPU image registration must produce diagnostics,
not upstream placeholder success, omitted work or interpreted execution. Bind
verified compiled modules to input/toolchain/runtime/backend receipts; then prove
the generic loader path with own multi-module fixtures. Existing title-specific
upstream hooks need an audit before general execution is exposed.

Dynamic SPU images/overlays need content fingerprints, LS load address/span,
runtime/lifter revision and relocation identity, AOT preparation outside active
execution, and invalidation on DMA/code writes. Unknown images must block with
an actionable diagnostic. No JIT/interpreter fallback is authorized by M0.

## Original firmware / XMB preparation

PUP import records identity, bounded entry tables and a readable version entry
(0x100). It does **not** verify Sony signatures/HMAC, decrypt, extract dev_flash,
load a PRX, or execute VSH. Encrypted SELF, readable ELF, AOT-generated output,
compiled module and executable session are distinct states.

The upstream [FIRMWARE_LLE.md](external/ps3recomp/docs/FIRMWARE_LLE.md) documents
`tools/lift_prx.py`: user-supplied **already decrypted** PRX → relocated image,
functions, imports and exports → `ppu_lifter.py --raw --base` → compiled module.
Its SPURS example is not proof of XMB boot. Keep such outputs in ignored local
storage, separate from HLE, preserving original module hashes, firmware revision,
base/relocations and explicit NID ownership. Never distribute derived output.

VSH/XMB additionally needs authenticated firmware extraction, its complete module
graph and loader contracts, dev_flash resources/fonts/icons, LV2 services,
users/storage/saves, controls, library discovery, and supervised game launch/return.
These dependencies and the generic execution gate block original XMB today.
ISO/disc and PKG extraction, DLC/install management and GUI packaging are future
work; identifying a container is not supporting its execution.

## Licensing

Own code: [BSD-3-Clause](LICENSE). ps3recomp: MIT with additional bundled licenses,
including its default LGPL-2.1 ATRAC decoder. See [third-party notices](THIRD_PARTY_NOTICES.md).
[RPCS3](https://github.com/RPCS3/rpcs3) is a behavioral/technical reference;
its implementation is not copied. Supply your own lawful originals; no games,
firmware, keys, proprietary SDK or proprietary generated code are distributed.
