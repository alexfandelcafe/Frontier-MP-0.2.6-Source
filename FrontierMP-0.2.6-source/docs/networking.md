# Networking model

## Channels

### Reliable

Use for:

- hello/welcome;
- spawn/despawn;
- death/respawn;
- weapon/equipment changes;
- inventory and economy transactions;
- resource lifecycle events;
- commands and admin events.

Reliable packets are sequenced and acknowledged by the packet header's `ack` plus 32-bit acknowledgement window.

### Unreliable

Use for:

- movement;
- rotation;
- velocity;
- aim state;
- animation state;
- other high-frequency state that is safe to replace with a newer update.

## Snapshot flow

The server runs at 20 Hz in the MVP. Each snapshot includes only relevant entities. Clients render remote entities with a presentation delay of about 100 ms so that two snapshots are normally available for interpolation.

The final interpolation policy will be:

`render = lerp(snapshot[n], snapshot[n+1])`

with short bounded extrapolation when a packet is late. Extrapolation is never allowed to grow without limit.

## Interest management

MVP uses a spherical radius and one dimension. Later it becomes:

`distance + sector + dimension + streaming state + priority class`.

The game documentation includes sector references and actor streaming/LOD related natives; these will inform future prioritization, but the server's interest manager remains independent of game-specific details.

## Ownership

Ownership is a network concept, not the same thing as an RDR native slot. A local player may own its input stream while the server remains authoritative over resulting gameplay state.

## Anti-cheat baseline

The server will reject or flag:

- malformed messages;
- sequence abuse;
- impossible player IDs;
- state updates attributed to another player;
- movement outside configured physical bounds;
- unauthorized state-changing events;
- excessive request rates.

MVP deliberately does not pretend to provide a complete anti-cheat system.
