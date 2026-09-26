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

The RDR Title Screen 3D scene remains the visual base. FrontierClient does not drive the native Title Screen menu by default. The historical native UI bootstrap is opt-in via FRONTIER_NATIVE_UI_BOOTSTRAP=1.

When built with a CEF SDK, the client can host the Frontier HTML frontend as a child window over the RDR window. The current CEF milestone only establishes browser hosting and page loading; JavaScript-to-native commands and CEF-owned frontend-to-gameplay transition remain subsequent work.
