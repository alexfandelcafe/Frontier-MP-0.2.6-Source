# FrontierMP 1.0.0 — build and run

## Toolchain

- Windows x64
- Visual Studio 2022 or newer
- CMake 3.25+
- CEF package for the CEF-enabled client
- Internet access during first CMake configure because ENet is fetched with FetchContent

## Build

From FrontierMP-0.2.6-source:

    scripts_build_final.bat

The script keeps compiler warnings and errors visible. It does not use /WX.

For VS 2026:

    set FRONTIER_VS_PRESET=windows-vs2026-x64
    scripts_build_final.bat

## RDRMP-compatible transport

The compatibility transport uses the recovered packet layer plus ENet:

- 2-byte big-endian packet ID
- 2 ENet channels
- delivery 0: reliable path
- delivery 1: unsequenced path
- packet 0: ClientWelcome
- packet 1: client event
- packet 2: 24-byte position/rotation state
- packet 3: server event
- packets 6-10: player/time/chat channels

Start frontier_rdrmp_server.exe with port 4674.

Set FRONTIER_RDRMP_COMPAT=1 in the client process before launching FrontierClient.

scripts_run_rdrmp.bat automates the local server plus launcher flow.

## CEF

Configure with FRONTIER_CEF_PACKAGE_DIR pointing to the root of your CEF binary distribution.

Example:

    cmake --preset windows-vs2022-x64 -DFRONTIER_CEF_PACKAGE_DIR=C:\path\to\cef

## RDR1 ABI

The game bridge is fingerprint-gated. An unknown RDR.exe build is rejected before engine hooks are enabled.

The known descriptor is kept in docs/compatibility.md. A matching fingerprint is not, by itself, proof that every runtime behavior is identical across RDR1 builds.

## Repository contents

Do not add RDR1 executable files, game assets, or CEF proprietary binaries to the repository. FrontierMP contains implementation code and the independently maintained compatibility layer.