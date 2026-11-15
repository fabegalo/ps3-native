# Third-party provenance

- **ps3recomp**, by **Ned Heller / sp00nznet**, MIT, is the upstream recompiler
  and runtime: https://github.com/sp00nznet/ps3recomp . Exact revision is in
  `deps.lock.json` and the `external/ps3recomp` gitlink. Its complete license
  remains at `external/ps3recomp/LICENSE`; preserve it with redistributed code.
  The upstream D3D12 pixel test and game-less host glue are compiled directly
  from the submodule; they are not ps3-native implementations. Game-less glue
  only serves the GPU test; PPU/SPU fixtures link the real PPU loader.
- Upstream **ATRAC3plus**, derived from FFmpeg via PPSSPP, is **LGPL-2.1** and
  enabled by upstream default. Source, attribution and license remain in
  `external/ps3recomp/third_party/at3_standalone/`. A source checkout or binary
  is not wholly BSD/MIT. No binary distribution is produced by this milestone.
- **RPCS3** is a technical/behavioral reference, not a linked dependency.
  PUP layout was checked against its public `rpcs3/Loader/PUP.h` and `PUP.cpp`:
  https://github.com/RPCS3/rpcs3 . No RPCS3 implementation or keys were copied.
  SHA-256 here is content identity, not PUP HMAC/signature verification.
  The public `Emu/Cell/Modules/sysPrxForUser.cpp` was also consulted for the
  two void process-spawn callback registration contracts. The independently
  written patch retains the first callback atomically. The optional official
  portable RPCS3 decrypt tool is local tooling, not a linked/execution dependency;
  no RPCS3 binary, keys, retail ELF or generated retail source is committed.
  Public `cellSpurs.h/.cpp`, `cellVideoOut.h`, `sceNp.cpp/.h` and
  `lv2/sys_mmapper.cpp` were consulted for ABI layouts, enums and API contracts.
  Corrections here are independently written; no RPCS3 implementation is linked.
- Architecture and maintainer-policy references:
  https://github.com/fabegalo/x360-native ,
  https://github.com/fabegalo/ps2-native ,
  https://github.com/fabegalo/xone-native . No other console CPU code is reused.
- PPU and SPU machine-code fixtures are authored in `tests/generate_fixtures.py`.
  No Sony SDK, firmware, game, key, decrypted module or proprietary AOT output
  is included. Local generated output belongs under ignored build/cache roots.
