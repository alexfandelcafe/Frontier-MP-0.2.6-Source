# FrontierMP

Independent multiplayer framework prototype for Red Dead Redemption 1 PC.

## Current milestone — 0.1.9

This version adds the first standalone client-launch path and the first build-gated RDR1 game bridge:

1. Inspect the user's existing `RDR.exe`.
2. Match the exact executable fingerprint.
3. Start RDR1 suspended.
4. Load `FrontierClient.dll` into that local process.
5. Resume RDR1.
6. Start the Frontier network runtime.
7. Connect to the Frontier dedicated server.
8. Resolve the local-player/actor-manager paths with signatures for the identified build.
9. Wait for the local player object to exist after world load.
10. Read the local actor transform position and replicate it to the server at 20 Hz when available.
11. Accept the first real client transform as the authoritative bootstrap state, then apply movement validation.
12. Write client diagnostics to `%LOCALAPPDATA%\FrontierMP\logs\client.log`.

The launcher does not replace or patch the game executable or game files, and it does not implement or bypass Rockstar licensing, DRM, or mandatory game authentication.

## Build

Use Visual Studio 2026 and run:

```text
scripts_build_vs2026.bat
```

Then run the server from:

```text
build\vs2026-x64\bin\Release\frontier_server.exe
```

and start the game via:

```text
scripts_run_frontier.bat
```

The launcher and DLL are expected beside each other:

```text
build\vs2026-x64\bin\Release\FrontierMP.exe
build\vs2026-x64\bin\Release\FrontierClient.dll
```

Unknown builds are rejected by the client game bridge.


## 0.1.8 validation

For build `rdr-pc-1.0.42.46611-fingerprint-a`, the client currently resolves two game data paths using signature scanning and follows the local player GUID to its actor transform. The bridge does not patch the executable or install files into the game directory.

A successful log should eventually contain:

```text
[FrontierClient] game bridge initialized; local-player symbol and actor-manager paths resolved
[FrontierClient] local-player state replication active position=(X, Y, Z)
```

If the first line is present but the second is not, the signatures matched but the local player object is not ready yet or one of the guarded pointer reads is unavailable.

## 0.1.9 validation

The dedicated server now accepts the first real local-player transform as a bootstrap state instead of comparing it against the synthetic `(0,0,0)` pre-spawn state. Subsequent updates remain subject to the movement validation window.



## CEF frontend overlay

The native RDR Title Screen is left in place as the visual scene. FrontierClient does not drive Title Screen UI events by default. When FRONTIER_CEF_PACKAGE_DIR points to a full CEF Windows distribution, the client builds a CEF host and displays cef/cef_ui/mainmenu/index.html in a child window over the RDR window.

The CEF package used for the build must contain at least:

```text
include/
libcef_dll/CMakeLists.txt
Release/libcef.lib
```

Configure with:

```text
cmake --preset windows-vs2026-x64 -DFRONTIER_CEF_PACKAGE_DIR="C:/path/to/cef"
cmake --build build\vs2026-x64 --config Release
```

The build also produces FrontierCefSubprocess.exe, which is placed beside FrontierClient.dll. The external CEF package is copied into the client's cef/ directory so Chromium resources and locales stay outside the RDR installation.

Without FRONTIER_CEF_PACKAGE_DIR, the client continues to build without CEF support.

## Game session bootstrap

FrontierMP 0.2.3 adds a build-gated native invoker and a read-only FrontierSession state machine. It observes the game frontend/world transition before any mutation is attempted. The launcher passes `FRONTIER_SESSION_MODE=freeroam`.
