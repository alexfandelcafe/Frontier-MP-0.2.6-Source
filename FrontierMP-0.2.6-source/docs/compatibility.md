# Game compatibility strategy

## Fingerprint

The detector records:

- PE timestamp;
- image size;
- `.text` RVA;
- `.text` size;
- `.text` FNV-1a 64 hash;
- file version string when available;
- PE machine architecture.

FNV-1a is a lookup fingerprint, not a cryptographic integrity guarantee. A future release can add SHA-256 without changing the public build descriptor.

## Matching policy

1. Inspect the loaded executable.
2. Compare the fingerprint against a versioned registry.
3. If matched, load the descriptor.
4. Resolve only symbols declared by that descriptor.
5. Validate every result lies in a committed image range and, for executable functions, in executable memory.
6. Run per-symbol sanity checks before enabling a feature.
7. Unknown build = network/gameplay bridge disabled; diagnostics stay available.

## Why pattern scanning is not enough

A signature may match multiple locations, may survive a patch while changing calling convention/semantics, or may resolve to a thunk rather than the intended function. FrontierMP therefore treats a pattern as one input to resolution, not as proof of correctness.

## Build registry

The registry now contains an exact observed fingerprint entry for the executable reported by the local probe:

- file version: `1.0.42.46611`
- PE timestamp: `0x673783f3`
- image size: `0x5a5ec600`
- `.text` RVA: `0x1000`
- `.text` size: `0x104a140`
- `.text` FNV-1a64: `0xb213cbf3dee9b6bf`
- machine: `AMD64`

This means `known_build_match=yes` means **the executable fingerprint matches our registry**. It does **not** mean that the complete runtime/game bridge has been validated yet. The descriptor therefore keeps `runtime_compatibility_verified=no` and only enables the generic pattern-scanning capability.

Rockstar's official 1.06 update for PC was published on February 24, 2026, while public RedHook documentation still identifies `1.0.42.46611` as the game build it was updated for. We therefore preserve both facts rather than treating the file version string as proof of full 1.06 runtime compatibility.
