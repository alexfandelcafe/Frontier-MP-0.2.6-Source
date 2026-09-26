# Architecture

## Process boundaries

```
FrontierMP Launcher
    |
    +-- verifies RDR.exe fingerprint
    +-- starts RDR1 suspended
    +-- loads FrontierClient.dll
    +-- resumes RDR1

RDR1 process
    |
    +-- FrontierClient.dll
          |
          +-- Game Compatibility Layer
          +-- Network Client
          +-- Remote Interpolation

Dedicated Server
    |
    +-- protocol
    +-- connection manager
    +-- player manager
    +-- replication
    +-- interest management

## Frontend ownership

The RDR Title Screen 3D scene remains the visual base. Native Title Screen button input is not the primary control path.

The CEF frontend is windowless/off-screen. Its BGRA frames are composited into the game's D3D11 swapchain. The renderer subprocess exposes app.connect(host, port) and app.quit(); browser-side handlers translate those commands into Frontier runtime actions.

FRONTIER_NATIVE_UI_BOOTSTRAP is retained only as an explicit legacy diagnostic path. It is not part of the normal CEF-owned startup sequence.

## Session ownership

Frontend visibility, world loading, local-player readiness and gameplay activation are independent signals.

The session state machine is:

    Booting
      -> native invoker ready
      -> game-thread dispatcher ready
      -> Frontend
      -> explicit historical online bootstrap requested
      -> WaitingForWorld
      -> WaitingForLocalPlayer
      -> Active

worldLoaded alone never transitions the session to Active.
