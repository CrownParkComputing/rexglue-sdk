# Third-party provenance

This private working snapshot preserves upstream file notices. It does not assign
a new blanket licence to the title code, assets or every component in the tree.

| Component | Location and notice |
|---|---|
| ReXGlue / Xenia shader translator, codegen contract and helpers | `runtime/gfx/shader/`, portions of `runtime/ppc/`, XDBF format definitions and renderer helpers; [notice](thirdparty/licenses/ReXGlue-Xenia.txt), plus per-file headers |
| glslang | `thirdparty/glslang/`; [licence](thirdparty/glslang/LICENSE.txt); imported file hashes in [UPSTREAM.json](UPSTREAM.json) |
| SIMDe | `thirdparty/simde/`; [notice](thirdparty/licenses/SIMDe.txt), plus per-file headers |
| libmspack LZX decoder | `thirdparty/mspack/`; [LGPL notice](thirdparty/mspack/COPYING.LIB) |
| Rijndael implementation | `thirdparty/crypto/rijndael-alg-fst.*`; public-domain notice in the files |
| TinySHA1 | `thirdparty/crypto/TinySHA1.hpp`; MIT notice in the header |
| Stephan Brumme SHA-256 with Xenia state accessors | `thirdparty/crypto/sha256.*`; zlib licence and copyright notice in both files |

SDL3, Vulkan, fmt, xxHash, toml++, SQLite3 and FFmpeg are supplied by the build/host system.
Their own notices apply. Generated title C++ and game files retain their
respective ownership and remain outside this vendored source snapshot.
[UPSTREAM.json](UPSTREAM.json) records the local source snapshot and imported
file hashes, including files subsequently changed for Forza integration.
