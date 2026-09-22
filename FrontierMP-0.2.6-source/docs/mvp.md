# MVP roadmap

## M0 — transport harness (implemented)

- protocol encoding/decoding;
- dedicated server;
- simulated clients;
- reliable handshake;
- player ID assignment;
- snapshot replication;
- interest radius;
- reconnect token plumbing;
- protocol unit tests.

## M1 — game fingerprinting (implemented, intentionally incomplete)

- real RDR executable fingerprinting;
- build registry;
- fail-closed unknown-build handling;
- symbol/pattern scanner.

## M2 — first in-game bridge

- stable bootstrap host;
- verified build symbol(s);
- read local player position/heading;
- send real position state to the server;
- spawn/read one remote player actor.

## M3 — movement presentation

- remote actor placement;
- interpolation;
- gait/movement state;
- LOD/streaming priority where verified.

## M4 — gameplay state

- models;
- weapons;
- damage/death/respawn;
- remote animation state.

## M5 — mounts and vehicles

- horse ownership;
- mount/dismount;
- horse state;
- vehicle seats;
- trains/wagons where stable.

## M6 — resources/Lua

- resource manifest;
- client/server Lua VMs;
- events;
- commands;
- timers;
- exports and permissions.

## M7 — persistence/admin

- database backend;
- accounts/identifiers;
- inventory/state persistence;
- admin console;
- audit logs.

## M8 — distribution

- server browser;
- launcher;
- update channel;
- diagnostics uploader (opt-in);
- compatibility reporting.

## Added in 0.2.0
- Build-gated native invoker bootstrap.
- Frontier session state machine and runtime-state diagnostics.
- Read-only game-state/world-readiness probes.

The dedicated multiplayer bootstrap is intentionally not marked complete until the frontend-to-world transition and story-session suppression are verified on the target build.
