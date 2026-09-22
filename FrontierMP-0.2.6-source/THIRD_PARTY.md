# Third-party research and reuse policy

This project does not copy RDRMP source code as its architectural base.

## Sources currently audited

### RedHookSDK
`K3rhos/RedHookSDK` is MIT-licensed. Its README describes a C++23 Visual Studio 2022 SDK/example for the Red Dead Redemption PC port. The project currently uses it as a reference for native invocation, game-facing type conventions and plugin lifecycle ideas; the FrontierMP core is not linked against `RedHook.lib`.

### G.R.E-Lab
`Red-Mods/G.R.E-Lab` is MIT-licensed. Its README describes a reverse-engineering laboratory for RDR, including patterns, hooks, structs, enums and symbol research. The README also says it is intended for single-player research rather than online use. We therefore treat it as research material and keep FrontierMP's runtime boundaries independent. Embedded research artifacts should be individually license-checked before redistribution.

### RDRMP-Docs
`Red-Mods/RDRMP-Docs` is MIT-licensed and is used as a historical reference for Lua events, resource concepts, native groupings and observed multiplayer behavior.

### RDRMP-Server-Resources
`Red-Mods/RDRMP-Server-Resources` is MIT-licensed. It is a reference for resource organization, not a dependency.

### RDR-PC-Natives-DB
`K3rhos/RDR-PC-Natives-DB` is MIT-licensed. It is suitable as a reference source for RDR1 PC native hashes/signatures and may be imported later with the required copyright notice. It is not copied into the MVP source tree yet.

### TheRouletteBoi native database
The public `rdr-nativedb-data` repository is historically useful, but no license file was found during this audit. Treat it as reference-only until licensing is verified.

### RDR1-Example-Script
`Rage-Modding-Collective/RDR1-Example-Script` is MIT-licensed and uses CMake. It is useful as a build and ScriptHook integration reference.

### RASM
The publicly visible RASM repository supports RDR script formats and PC targeting. No license file was found during this audit, so FrontierMP does not bundle or copy its code.


## Game bridge research inputs

The 0.1.5 RDR1 game bridge uses the following public G.R.E-Lab research inputs for the identified PC build: the local-player signature, actor-manager signature, and the documented `sagPlayer`/`sagActor`/actor-component/transform layouts. FrontierMP reimplements its own resolver and guarded read path; it does not link against G.R.E-Lab or RedHook binaries. The G.R.E-Lab repository is MIT-licensed, and its README identifies these materials as reverse-engineering research for RDR.
