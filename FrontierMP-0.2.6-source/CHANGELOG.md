## 0.2.63
- Replace the one-shot remote actor test with a RemotePlayerManager driven by server snapshots.
- Keep each remote identity keyed by NetworkPlayerId while backing it with a normal ACTOR_MPPLAYER01 Actor.
- Add game-thread-safe remote Actor spawn, transform update, and destruction operations.
- Reuse the existing snapshot interpolation layer and remove remote Actors after a short absence grace period.
- Route remote-player lifecycle out of the native Player experiment; Native PlayerId remains local-only.

## 0.2.62
- Test the remote-player architecture as a normal ACTOR_MPPLAYER01-backed Actor.
- Use a dedicated FrontierRemoteLayout and the normal generic CREATE_ACTOR_IN_LAYOUT return path.
- Validate the resulting Actor and log its enum and IS_ACTOR_PLAYER state without attempting native Player promotion.

## 0.2.61
- Defer the MPPLAYER01 CREATE_ACTOR_IN_LAYOUT and RESPAWN_PLAYER_ACTOR_IN_LAYOUT probe to the game-thread dispatcher instead of calling them reentrantly from GET_PLAYER_ACTOR.
- Log post-creation actor validity and enum, then post-respawn validity, IS_ACTOR_PLAYER, and enum.
- Keep the probe one-shot per client session and retry only when task submission itself fails.

## 0.2.60
- Stop retrying the rejected CREATE_PLAYER_ACTOR_IN_LAYOUT remote-player path.
- Create a proven ACTOR_MPPLAYER01 through CREATE_ACTOR_IN_LAYOUT in the live Player context and test RESPAWN_PLAYER_ACTOR_IN_LAYOUT against that concrete actor.
- Keep the experiment one-shot per client session and log the resulting ActorRef, actor validity, and IS_ACTOR_PLAYER state.

## 0.2.59
- Restore the two-argument STREAMING_IS_ACTOR_LOADED(Model, unk) probe documented by the public RDR1/RDRMP native reference.
- Probe the known second-argument candidates again so MPPLAYER01 load state is measured with the correct native signature.

## 0.2.58
- Reuse the exact PlayerLayout handle captured from the game's local CREATE_PLAYER_ACTOR_IN_LAYOUT call.
- Avoid synthetic FIND_NAMED_LAYOUT/CREATE_LAYOUT resolution in the remote player probe, which previously created a duplicate layout and invalidated the PlayerLayout experiment.

## 0.2.57
- Use the live PlayerLayout for the in-context remote CREATE_PLAYER_ACTOR_IN_LAYOUT experiment.
- Keep the remote actor name/model/position test unchanged so the layout dependency is isolated.

## 0.2.56
- Execute the in-context remote CREATE_PLAYER_ACTOR_IN_LAYOUT probe once the remote layout is valid instead of gating it on STREAMING_IS_ACTOR_LOADED.
- Use the canonical one-argument STREAMING_IS_ACTOR_LOADED probe and keep its result diagnostic-only.

## 0.2.55
- Keep the remote player creation probe in the live Player script context and fix helper declarations so the tracer compiles cleanly.

## 0.2.54
- Remove the invalid out-of-script CREATE_PLAYER_ACTOR_IN_LAYOUT probe from RdrBridge.
- Keep RdrBridge limited to arming the remote layout and requesting MPPLAYER01.
- Move the actual player-actor creation experiment into the live Player script native context captured by StartupNativeTracer.

## 0.2.53
- Fix a regression in invoke_u32 introduced while separating native return storage for mutable invocations.
- Keep returnValue local to invoke_raw_mutable so the existing u32 native path continues to read its return slot correctly.

## 0.2.52
- Ensure the mutable native invocation returns the separate native return value instead of the argument buffer.
- This preserves CREATE_PLAYER_ACTOR_IN_LAYOUT's PlayerId return while also exposing its post-call ActorRef in argument 0.

## 0.2.51
- Keep mutable native arguments and native return storage separate to match the live RDR native call context.
- Validate remote CREATE_PLAYER_ACTOR_IN_LAYOUT by comparing its returned PlayerId with GET_PLAYER_ACTOR(PlayerId) and IS_ACTOR_PLAYER on the created Actor.
- Preserve the streaming diagnostics and request result while probing the player-creation path.

## 0.2.50
- Add a mutable native invocation path so natives that rewrite argument slots can expose those outputs to Frontier.
- Correct the CREATE_PLAYER_ACTOR_IN_LAYOUT experiment to treat its native return as PlayerId and post-call argument 0 as the ActorRef.
- Log MPPLAYER01 installation/streaming state and permit the isolated player-creation probe even when STREAMING_IS_ACTOR_LOADED remains false.

## 0.2.49
- Switch the isolated remote MPPLAYER01 experiment from CREATE_ACTOR_IN_LAYOUT to CREATE_PLAYER_ACTOR_IN_LAYOUT.
- Keep the remote layout and streaming lifecycle unchanged so the test isolates player registration from model loading.
- Record immediate post-create IS_ACTOR_PLAYER and GET_PLAYER_ACTOR checks without broad raw-memory scans.

## 0.2.48
- Retry the remote multiplayer actor spawn request on subsequent state ticks when `STREAMING_IS_ACTOR_LOADED` initially reports not loaded.
- Keep the existing game-thread idempotence guard so repeated retries do not enqueue duplicate work while a spawn attempt is pending.
- Do not change the actor enum, native creation path, or raw-memory diagnostics.

## 0.2.47
- Remove the broad raw-memory `GeneralManagerSlots` scan from the live remote-actor test after confirming it can locate the created actor as `sagGuid=0x0003`.
- Stop using `ACTOR_PLAYER=0` with `CREATE_ACTOR_IN_LAYOUT`; use `ACTOR_MPPLAYER01=837`, matching the game's multiplayer actor family.
- Request and verify the multiplayer actor asset is streamed before attempting creation; defer creation while the model is still loading.
## 0.2.46
- Replace the old ActorHandle-based manager lookup experiment with a non-destructive `sagGuid`-indexed `GeneralManagerSlots` scan.
- After remote actor creation, correlate the new actor by its `sagActor::m_Guid` at `+0x08` and transform position, logging whether the actor was actually registered in the actor manager.
- Keep the remote spawn path itself unchanged; the manager scan is diagnostic only.

## 0.2.45
- Extend passive actor tracing with IS_ACTOR_INITED to distinguish handle creation from completed actor initialization.
- Continue avoiding direct classification probes from Frontier during the remote-actor test.

## 0.2.44
- Add passive tracing for actor/player classification and player-actor lifecycle natives: IS_ACTOR_VALID, IS_ACTOR_PLAYER, IS_ACTOR_LOCAL_PLAYER, IS_LOCAL_PLAYER_VALID, RESPAWN_PLAYER_ACTOR_IN_LAYOUT, SWITCH_PLAYER_TO_ENUM, and INIT_NATIVE_ACTORENUM_PLAYER.
- Keep the remote actor experiment free of additional direct classification calls; use runtime traces to determine whether the engine promotes or uses the created actor as a player.

## 0.2.43

- Replace the speculative remote Actor -> manager-slot memory scan with passive tracing of the engine's Actor/Slot natives.
- Trace GET_ACTOR_SLOT, GET_SLOT_ACTOR, GET_LOCAL_SLOT, and IS_SLOT_VALID without invoking them from Frontier.
- Keep remote actor creation limited to the verified game-thread CREATE_ACTOR_IN_LAYOUT + GET_ACTOR_ENUM path.

## 0.2.42

- Add a read-only diagnostic that resolves the spawned Actor handle through the actor-manager backing storage used by the local-player chain.
- Probe a bounded actor->component->transform candidate chain and accept it only when the resolved position matches the spawn coordinates.
- Keep the probe game-thread-only, protected, and free of additional native calls or actor-memory writes.

## 0.2.41

- Added a game-thread-only controlled remote actor spawn test through CREATE_ACTOR_IN_LAYOUT.
- Added a raw NativeInvoker call path for up to 32 uintptr-sized native arguments.
- Follow the observed Layout lifecycle: FIND_NAMED_LAYOUT, CREATE_LAYOUT fallback, IS_LAYOUTREF_VALID, then CREATE_ACTOR_IN_LAYOUT.
- Use actor enum 0 from the verified local CREATE_PLAYER_ACTOR_IN_LAYOUT trace and place the diagnostic actor two meters from the local player.
- Keep the remote actor test idempotent per client session; no actor memory mutation or Rockstar NET session is introduced.
- Trace GET_ACTOR_ENUM for the returned ActorRef and probe the verified actor-manager slot mapping for internal actor/component/transform resolution.
- Validate the spawned ActorRef through GET_ACTOR_SLOT, GET_SLOT_ACTOR, and GET_POSITION before treating internal actor offsets as authoritative.
- Pass the plain 32-bit Actor handle to actor natives; retain the 0x100000000 tag only on the raw ActorRef returned by CREATE_ACTOR_IN_LAYOUT.
- Removed the experimental scalar actor probes after runtime instability; keep remote actor validation limited to the verified CREATE_ACTOR_IN_LAYOUT and GET_ACTOR_ENUM path.

## 0.2.40

- Capture CREATE_PLAYER_ACTOR_IN_LAYOUT and CREATE_ACTOR_IN_LAYOUT arguments before invoking the original native.
- Record post-call argument slot 0 to detect engine-side handle/output rewriting.
- Preserve decoded position/orientation from the caller-supplied pre-call values.

## 0.2.39

- Added passive tracing for CREATE_LAYOUT, FIND_NAMED_LAYOUT, and IS_LAYOUTREF_VALID.
- Record layout names and returned layout handles to establish a known-good Layout for remote actor creation.
- Keep layout tracing optional; no layout or actor is created by Frontier.

## 0.2.38

- Extend CREATE_ACTOR_IN_LAYOUT tracing with all seven raw arguments.
- Decode layout handle, layout name, actor enum, position, and orientation from the observed native argument layout.
- Preserve passive tracing only; no actor creation or mutation is introduced.

## 0.2.37

- Added passive tracing for player actor creation and lookup.
- Trace CREATE_PLAYER_ACTOR_IN_LAYOUT, GET_PLAYER_ACTOR, and CREATE_ACTOR_IN_LAYOUT when those handlers are available.
- Bound actor-creation tracing to the first 64 calls per process to keep lifecycle logs usable.
- Preserve the existing external Frontier networking architecture; no Rockstar NET session is started or modified.

## 0.2.36

- Added passive tracing for the RDR1 NET session lifecycle.
- Trace NET_ENABLE_MULTIPLAYER, NET_IS_IN_SESSION, NET_IS_SESSION_CLIENT, NET_SESSION_QUICK_JOIN_NATIVE, NET_SESSION_START_GAMEPLAY, NET_SESSION_END_GAMEPLAY, and NET_SESSION_IS_GAMEPLAY_STARTED when those handlers are available.
- NET tracing records arguments/return values plus game-thread context identity without invoking or forcing any network/session transition.
- NET hooks are optional so a missing handler does not prevent the existing startup tracer from attaching.

## 0.2.35

- Normalize `FrontierSession::localPlayerReady` from the current stable-world state instead of retaining a stale ready value after stable world loss.
- When `stableWorldLoaded=0`, `localPlayerReady` is now cleared before the session enters `frontend`.
- Preserve local-player probing while `stableWorldLoaded=1`, including the existing GUID/actor/component/transform lifecycle gating.

## 0.2.34

- Treat `GUID=0` as a pending local-player identity state before actor-slot lookup.
- Preserve the existing actor/component/transform resolution for non-zero GUIDs.
- This removes probing of the placeholder slot 0 while the local-player identity is still uninitialized.

## 0.2.33

- Fixed MSVC C2712 by moving all local-player guarded SEH reads out of the C++ member function and into trivial helper functions.
- Use guarded memcpy for the transform position read.
- Preserved all local-player diagnostics, offsets, and readiness behavior.

- Record the complete local-player pointer chain when state replication first becomes ready.
- Log the GUID, actor-manager slot, actor, actor-component, and transform addresses at successful resolution.
- No local-player offsets, readiness criteria, or runtime-state behavior were changed.

## 0.2.32

- Clarified local-player chain diagnostics so null actor-component and transform pointers are reported as pending lifecycle state rather than unreadable memory.
- Include local-player, actor-manager, GUID, slot, and actor addresses in the pending actor-component diagnostic.
- Include local-player context in non-null actor-component and transform diagnostics.
- No local-player resolution criteria or memory offsets were changed.

## 0.2.6

- Hardened native registration-table discovery: inspect all registration-pattern matches instead of trusting the first hit.
- Validate candidate tables against the G.R.E-Lab initialization sentinel `0xA0AE0C98`.
- Require at least one verified RDR native handler (`GET_POSITION` or `GET_GAME_STATE`) before accepting the table.
- Centralized native-table probing and lookup.
- Keeps the runtime diagnostic-only; no game-state mutation added.

## 0.2.3

- Added per-native diagnostics for the FrontierSession runtime probes.
- `GET_GAME_STATE`, `STREAMING_IS_WORLD_LOADED`, `IS_SIMULATE_START_MULTIPLAYER`, and `IS_STARTPOS_IN_COMMAND_LINE` now report individual failures instead of collapsing into a silent session probe failure.
- No game-state mutation was added in this diagnostic build.

## 0.2.2

- Removed all MSVC SEH blocks from C++ member functions.
- Added low-level guarded copy helper so `__try/__except` is only used in trivial no-unwind helper functions.
- Fixed duplicate preprocessor guard in `native_invoker.cpp`.
- Simplified guarded native-handler invocation.

# Changelog

## 0.2.1
- Fixed MSVC native invoker context layout compilation error caused by inheriting from a `final` context layout.
- Moved guarded native-handler execution into a raw-pointer helper so MSVC SEH does not conflict with C++ object unwinding.
- Removed redundant `WIN32_LEAN_AND_MEAN` definition from `native_invoker.cpp`.


## 0.2.0
- Added an independent Frontier native invoker based on build-gated signature resolution for the RDR PC build currently under verification.
- Added FrontierSession state tracking for frontend, world streaming, local-player readiness, and the game runtime state.
- Added read-only runtime probes for `GET_GAME_STATE`, `STREAMING_IS_WORLD_LOADED`, `IS_SIMULATE_START_MULTIPLAYER`, and `IS_STARTPOS_IN_COMMANDLINE`.
- The session layer is diagnostic-first: it does not mutate Rockstar game state yet. This avoids pretending that hiding the menu is equivalent to entering a dedicated multiplayer session.
- Added `FRONTIER_SESSION_MODE=freeroam` as the client-side session intent passed from the launcher environment.

# Changelog

## 0.1.9
- Fixed authoritative player bootstrap: the first real in-game transform is accepted before movement-speed validation.
- Added finite-value validation for incoming position, rotation, and velocity fields.
- Added server diagnostics when a player receives its first authoritative transform.


## 0.1.8

- Fixed MSVC compilation error in `RdrBridge` caused by comparing `std::uintptr_t` with `nullptr`.
- Removed duplicate `WIN32_LEAN_AND_MEAN` definitions from client sources.
- Moved the client target outside the developer-tools guard so the requested Windows client build is independent of tools.
- Added an explicit launcher dependency on `frontier_client`.
- Added a post-build copy of `FrontierClient.dll` beside `FrontierMP.exe`.
- Hardened the Windows build/run scripts so a missing executable or DLL stops the workflow with a useful error instead of Windows error 9009.

## 0.1.7

- Fixed Windows launcher/client packaging by making `frontier_launcher` depend explicitly on `frontier_client`.
- Added a post-build copy that places `FrontierClient.dll` beside `FrontierMP.exe`.

## [0.1.6] - 2026-09-22

- Fixed Windows `FrontierClient.dll` output placement so the client DLL is emitted beside `FrontierMP.exe` under `bin\<Config>`.
- Added a launcher fallback for older build trees that placed `FrontierClient.dll` at the configuration directory root.

## 0.1.4

- Added the first standalone Windows launcher for FrontierMP.
- Launcher verifies the exact RDR.exe fingerprint before launch.
- Launcher starts RDR1 suspended, loads FrontierClient.dll, then resumes RDR1.
- The launcher does not rewrite the game installation.
- FrontierClient.dll starts its network update loop and reads launcher parameters from the RDR command line.
- Client diagnostics are written to `%LOCALAPPDATA%\FrontierMP\logs\client.log`.
- CMake groups runtime binaries under `build/<preset>/bin/<config>`.
- The exact observed `1.0.42.46611` fingerprint remains identified, but full game-bridge compatibility is not yet verified.

## 0.1.5
- Added the first RDR1 game bridge using build-gated signature resolution.
- Added validated local-player pointer and actor-manager discovery for build `1.0.42.46611`.
- Added guarded reads of the local actor transform position.
- Client now sends real local-player position to the server at 20 Hz when the bridge is ready.
- Networking remains active when the game bridge is unavailable; no synthetic position is sent in that case.
