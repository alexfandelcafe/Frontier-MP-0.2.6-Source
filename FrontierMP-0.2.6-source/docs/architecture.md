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
