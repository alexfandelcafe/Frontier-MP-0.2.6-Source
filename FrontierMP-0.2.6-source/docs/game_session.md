# Frontier Game Session

## Purpose

FrontierMP must eventually start an RDR process into a server-driven free-roam session instead of presenting the normal single-player frontend. The current project stage deliberately separates observation from mutation.

## Current 0.2.0 stage

The client now resolves an internal native-registration table using build-gated signatures and exposes a minimal read-only native invocation path. `FrontierSession` polls:

- `GET_GAME_STATE` (`0xDD9BD22B`)
- `STREAMING_IS_WORLD_LOADED` (`0x87B74064`)
- `IS_SIMULATE_START_MULTIPLAYER` (`0x9A73C2CD`)
- `IS_STARTPOS_IN_COMMANDLINE` (`0x814D97E8`)

These calls are used only for diagnostics and readiness decisions.

## Why this is staged

The public native database exposes `GET_GAME_STATE`, but not a corresponding public setter. There are also `SET_START_POS` and `CLEAR_MISSION_INFO`, but their intended internal sequencing is not documented well enough to treat them as a complete multiplayer bootstrap. A simple “skip menu” would therefore be an unsafe shortcut: it could leave story scripts, save state, mission state, or the frontend active underneath the multiplayer runtime.

## Target state

```text
Frontier Launcher
    -> RDR.exe
    -> FrontierClient.dll
    -> Game Compatibility Layer
    -> FrontierSession
       -> suppress single-player frontend
       -> start/load multiplayer world
       -> prevent story session ownership
       -> prevent single-player save/load ownership
       -> wait for Frontier server spawn
    -> Entity/Player runtime
```

The next implementation stage is to identify the exact build-specific transition that exits the frontend into a world session, then guard it behind a feature capability.
