# Frontier Game Session

## Purpose

FrontierMP starts RDR in its normal process context, keeps the native Title Screen 3D scene as the initial visual layer, and transfers multiplayer session ownership to the CEF frontend. Native menu buttons are not required for the normal path.

## Current 0.2.6 stage

The client resolves the build-gated native registration table, dispatches native work on the RDR game thread, hosts transparent CEF OSR rendering, and exposes the historical online transition as an explicit runtime operation. `FrontierSession` polls:

- `GET_GAME_STATE` (`0xDD9BD22B`)
- `STREAMING_IS_WORLD_LOADED` (`0x87B74064`)
- `IS_SIMULATE_START_MULTIPLAYER` (`0x9A73C2CD`)
- `IS_STARTPOS_IN_COMMANDLINE` (`0x814D97E8`)

These calls are used for diagnostics and readiness decisions. They do not, by themselves, prove that the multiplayer gameplay session is active.

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
