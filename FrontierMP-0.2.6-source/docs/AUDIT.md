# Ecosystem audit — September 22, 2026

## Executive finding

The technical foundations for an independent multiplayer framework exist, but the difficult part is not UDP networking. The main risk is the game-side compatibility layer: current public modding tools were built around the PC port that the community identifies as `1.0.42.46611`, while Rockstar published a PC title update 1.06 on February 24, 2026 without exposing the internal executable fingerprint in its public notes.

Therefore FrontierMP will separate **wire protocol** from **game ABI**, and the game ABI will be build-gated. Unknown builds will remain diagnostic-only until their fingerprints and required symbols are verified.

## 1. RedHook

Public RedHook release history reaches v0.8, with keyboard input natives and plugin-load fixes. Its v0.5b notes specifically say it was updated for game build `1.0.42.46611`. The RedHook release page was last updated in March 2025, before Rockstar's February 2026 PC 1.06 update.

The separate `K3rhos/RedHookSDK` repository is MIT-licensed, C++23, and built for Visual Studio 2022. It contains a plugin example plus native headers, math/types and an application lifecycle. It links against `RedHook.lib`, which makes it inappropriate as the core architectural dependency for FrontierMP.

**Decision:** use RedHookSDK as a reference and, during early integration, optionally support a thin RedHook adapter. Do not make the protocol/server/client architecture depend on RedHook internals.

## 2. G.R.E-Lab

G.R.E-Lab is an MIT-licensed reverse-engineering laboratory. Its RDR project contains:

- pattern scanning;
- MinHook-based function hooks;
- VMT hooks;
- memory utilities;
- RDR native and enum headers;
- fibers/script thread infrastructure;
- DirectX/input hooks;
- a large RDR research area.

Its `Research/RDR/Switch Symbols` area contains a large Nintendo Switch symbol dataset. The accompanying README explicitly describes it as a reverse-engineering aid that can be ported to the PC version, rather than as PC addresses themselves.

**Decision:** reuse concepts, algorithms and selected MIT code only after preserving notices. Do not assume Switch symbols or a single pattern equals a validated PC address.

## 3. RDRMP

The historical RDRMP project successfully demonstrated the basic concept: dedicated servers, Lua resources, player IDs and multiplayer gameplay. The public distribution is now marked abandoned. The author stated in January 2026 that there were no plans to return to the project.

A public source repository named `Red-Mods/RDRMP` was not found during this audit; GitHub repository lookup returned 404. Publicly maintained pieces that remain visible are primarily the documentation and server-resource repositories.

**Decision:** no RDRMP source dependency. Treat the old implementation as behavioral archaeology and a list of failure modes.

## 4. RDRMP-Docs

The documentation remains valuable because it inventories concrete RDR1 concepts. The core API documents include resource lifecycle, player join/leave events, client/server event delivery, chat and script threads. The native reference is extensive and includes actor, entity, animation, riding, vehicles, time, weather and `net` namespaces.

Examples relevant to future networking include actor position/heading, player controllability, gait simulation, high-LOD/streaming priority, horse mount/rider relationships, vehicle seats, train control and the engine's existing network-native surface.

**Decision:** these documents are a source of observed engine capabilities and historical API design. FrontierMP will expose a new API and its own network protocol.

## 5. RDRMP server resources

The public server-resource repository is small and MIT-licensed, containing examples such as chat, freeroam, transport, nametags and noclip.

**Decision:** resource packaging is worth copying conceptually, not as a dependency.

## 6. Native research

`K3rhos/RDR-PC-Natives-DB` currently exposes a large RDR1 PC native list and is MIT-licensed. The public `TheRouletteBoi/rdr-nativedb-data` project is another valuable historical native source, but this audit did not find a license file there, so we will not ship its data until licensing is clarified.

The public `Rage-Modding-Collective/RDR1-Example-Script` repository is MIT-licensed and provides a CMake-based ScriptHook example, including credits to the native-research community.

## 7. Build status

Rockstar's official title-update notes state that PC 1.06 shipped on February 24, 2026 with general bug fixes and improvements. Public modding tools still identify `1.0.42.46611` as a supported RDR PC gamebuild, but that is not enough evidence to prove that all current installations have the same internal executable layout.

**Decision:** build detection is a first-class subsystem, not a later refactor.

## 8. Legal/technical reuse matrix

| Source | License/status | FrontierMP treatment |
|---|---|---|
| RedHookSDK | MIT | Reference; selective reuse possible with notice |
| G.R.E-Lab | MIT | Reference; selective reuse possible with notice |
| RDRMP-Docs | MIT | Reference; can reuse documentation concepts |
| RDRMP-Server-Resources | MIT | Reference/examples; not a dependency |
| RDR-PC-Natives-DB | MIT | Future data import possible with notice |
| rdr-nativedb-data | License not found | Reference-only for now |
| RDR1-Example-Script | MIT | Build/integration reference |
| RASM | License not found | Reference-only |
| RDRMP distributed mod | not audited as source; public release abandoned | Do not make it a source-code base |

## 9. What is technically reachable

### High confidence

- Dedicated UDP server.
- Reliable/unreliable transport separation.
- Player IDs, session tokens and reconnect grace windows.
- Server snapshots and interest management.
- Remote interpolation/extrapolation.
- Resource packaging and a Lua host once the game bridge is stable.
- Build detection and symbol resolution.
- Diagnostics and packet tracing.

### Medium confidence; requires runtime validation

- Remote-player actor creation and stable model assignment.
- Movement driven by game natives.
- Animation replication and gait blending.
- Horse ownership and mount relationships.
- Networked vehicles and train state.
- Server-controlled time/weather/world state.

### High-risk / build-specific

- Direct calls into non-exported engine functions.
- Internal object-manager/actor-manager access.
- VMT layouts or internal class layouts.
- Script VM injection and thread manipulation.
- Anything depending on private networking implementation details.

## 10. Main engineering rule

The framework must have three separate layers:

`Protocol/Server` → never knows game addresses.

`Client Runtime` → knows network state and entity replication policy.

`Game Adapter` → the only layer allowed to touch build-specific natives, pointers, hooks or offsets.

This is the architectural boundary that prevents a future Rockstar patch from forcing a rewrite of the server.
