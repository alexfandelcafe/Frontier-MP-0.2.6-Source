# Research sources — checked 2026-09-22

This document records the public sources used for the initial FrontierMP architecture audit. URLs are provided for human review; FrontierMP does not vendor game binaries or proprietary game assets.

## Rockstar / game updates

- Rockstar Games, "Red Dead Redemption Title Update Notes" — PC 1.06 released 2026-02-24; the notes describe general bug fixes and improvements.
  https://support.rockstargames.com/articles/4Kzr5DrodN1JYu4mixkMxE/red-dead-redemption-title-update-notes-ps5-ps4-xbox-series-x-or-s-switch-2-switch-pc

## Hook / SDK / runtime research

- K3rhos/RedHookSDK — MIT, C++23 / Visual Studio 2022 example SDK for the RDR PC port.
  https://github.com/K3rhos/RedHookSDK
- Red-Mods/RedHook-Docs — API and scripting reference for RedHook.
  https://github.com/Red-Mods/RedHook-Docs
- RedHook (Nexus Mods) — public release history; v0.5b documents support work for PC build 1.0.42.46611; v0.8 adds keyboard-input natives.
  https://www.nexusmods.com/reddeadredemption/mods/192
- Rage-Modding-Collective/RDR1-Example-Script — MIT, CMake-based ScriptHookRDR example.
  https://github.com/Rage-Modding-Collective/RDR1-Example-Script

## Reverse engineering

- Red-Mods/G.R.E-Lab — MIT reverse-engineering laboratory for RDR; includes pattern scanner, hooks, native/enum headers and Switch symbol research.
  https://github.com/Red-Mods/G.R.E-Lab
- G.R.E-Lab Switch-symbol notes.
  https://github.com/Red-Mods/G.R.E-Lab/tree/main/Research/RDR/Switch%20Symbols
- ShinyWasabi/scrDbg — RAGE script debugger; the RDR1 GitHub topic showed an update on 2026-08-23.
  https://github.com/ShinyWasabi/scrDbg
- K3rhos/RDR-PC-Natives-DB — MIT native database for the RDR PC port.
  https://github.com/K3rhos/RDR-PC-Natives-DB
- TheRouletteBoi/rdr-nativedb-data — current RDR1 native database; license was not verified during this audit, so FrontierMP does not bundle its data.
  https://github.com/TheRouletteBoi/rdr-nativedb-data
- DeusMaveriX/RAGE-Assembler-Disassembler (RASM) — RDR script format tooling; license was not verified during this audit, so FrontierMP does not bundle its code.
  https://github.com/DeusMaveriX/RAGE-Assembler-Disassembler

## Historical multiplayer reference

- Red-Mods/RDRMP-Docs — MIT documentation covering historical RDRMP Lua/event/native APIs and game references.
  https://github.com/Red-Mods/RDRMP-Docs
- Red-Mods/RDRMP-Server-Resources — MIT example resources (chat, freeroam, transport, nametags, noclip, etc.).
  https://github.com/Red-Mods/RDRMP-Server-Resources
- RDRMP (Nexus Mods) — abandoned historical multiplayer release; last update 2025-05-26.
  https://www.nexusmods.com/reddeadredemption/mods/394

## Licensing policy used by FrontierMP

MIT-licensed material may be selectively reused with its copyright/license notice preserved. Material whose license was not verified is treated as reference-only. RDRMP is not used as the source-code base. No Rockstar executable, game asset, DRM bypass, authentication bypass, or unauthorized copy is included.
