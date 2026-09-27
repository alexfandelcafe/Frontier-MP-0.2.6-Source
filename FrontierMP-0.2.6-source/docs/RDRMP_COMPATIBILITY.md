# RDRMP compatibility layer

This document records the binary behavior recovered from the published `client-networking.dll.c` analysis and how FrontierMP isolates that behavior from the RDR1 game ABI.

## Proven packet registrations

| Packet ID | Direction | Evidence-level payload |
|---:|---|---|
| 0 | client -> server | `uint32 actorModel`, `uint16 + name`, `Vector3 position`, `Vector3 rotation` |
| 1 | client -> server | event name string followed by typed event arguments |
| 2 | client -> server | `Vector3 position`, `Vector3 rotation` |
| 3 | server -> client | event name string followed by typed event arguments |
| 4 | unresolved | no handler registration found in the recovered path |
| 5 | server -> client | TimeOfDay handler; complete field layout not yet verified |
| 6 | server -> client | `uint16 playerId`; delete-player handler |
| 7 | server -> client | `uint16 playerId`, `uint32 actorModel`, name string, `Vector3 position`, `Vector3 rotation` |
| 8 | server -> client | `uint16 playerId`, `uint32 value`; property/state handler with an engine-side native side effect |
| 9 | server -> client | `uint16 playerId`, `Vector3 position`, `Vector3 rotation` |
| 10 | server -> client | TextChat subsystem packet |

The packet IDs are read as 16-bit keys by `ENetClient::SafeReadData`, while the serializers inside individual handlers use the field widths shown above.

## Scalar encoding

The recovered stream writer emits integral and floating-point values in big-endian byte order.

A string is:

`uint16 length` + `length` raw bytes

The client-side reader rejects string lengths at or above `0x1000`, giving a practical maximum of 4095 bytes.

A `Vector3` is three 32-bit IEEE-754 floats in the same big-endian byte order.

## Typed event encoding

The generic event packet contains:

`string eventName` + `uint8 argumentCount` + repeated typed arguments

Observed type tags:

| Tag | Value |
|---:|---|
| 1 | `uint64` |
| 2 | `double` |
| 3 | `bool` as one byte |
| 4 | string |
| 5 | `Vector3` |

Event lookup uses 64-bit FNV-1a with offset basis `0xcbf29ce484222325` and multiplier `0x100000001b3`.

## Player handlers

The reconstructed handlers provide stronger evidence for packets 6-9 than the earlier prototype did:

- Packet 6 reads only a player ID and enters the delete path. The delete path triggers `core:on_player_left` and ultimately destroys the engine actor before removing the player entry.
- Packet 7 reads player ID, actor model, name, position and rotation, then enters the player-create path. That path creates the engine-side player entry and triggers `core:on_player_joined`.
- Packet 8 reads player ID and one 32-bit property value. The recovered handler writes the value into an actor-side field and invokes an engine script native. The exact semantic name of that property is not yet established, so FrontierMP treats it as a generic property channel.
- Packet 9 reads player ID plus two `Vector3` values and updates the stored transform state.

## ENet boundary

The recovered client calls `ENetClient::Connect` with a host and port and uses an ENet host with two channels. The final FrontierMP client keeps its game-independent state machine separated from the wire codec so ENet can be used as the transport without exposing ENet types to the game bridge.

The current default networking implementation in the 0.2.6 source tree remains a standalone UDP reliability layer. That layer is useful for development and automated testing, but it should not be described as byte-compatible ENet transport. The RDRMP compatibility transport is therefore a distinct integration task.

## Game ABI boundary

RDR1 internals are not stable API. The game bridge is build-gated and only runs when the executable fingerprint matches the registered build descriptor. The current descriptor records `1.0.42.46611` and the fingerprint values stored in `docs/compatibility.md`.

The recovered bridge sequence for remote actors is:

1. validate the actor enum;
2. request streaming;
3. wait until the actor reports loaded;
4. locate or create an actor layout;
5. call `CREATE_ACTOR_IN_LAYOUT`;
6. validate the returned actor;
7. confirm its actor enum;
8. run subsequent actor operations through the game-thread dispatcher.

For the default remote test actor, the source uses actor enum `837` (`ACTOR_MPPLAYER01`). This is the enum directly exercised by the previous Frontier spawn experiment.

## What remains unverified

The following items require a real RDR1 runtime capture on the target executable:

- exact ENet channel configuration and packet framing outside the payload handlers;
- complete packet 5 TimeOfDay fields;
- whether packet 8 is a specific model/LOD/priority field on every build;
- game-thread timing for packet 7 actor creation under streaming pressure;
- full animation/gait replication;
- horse, vehicle and train replication;
- compatibility after any executable update that changes the registered fingerprint.

The repository should treat these as explicit validation tasks, not inferred guarantees.
