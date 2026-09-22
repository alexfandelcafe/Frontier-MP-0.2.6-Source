# RDR1 Game Bridge

FrontierMP treats the game executable as a separate ABI boundary.

For build `rdr-pc-1.0.42.46611-fingerprint-a`, the first bridge resolves two data symbols with signature scanning:

- `rage::sagPlayer::sm_LocalPlayer`
- `rage::aGuidGeneral::sm_ManagerSlots`

The bridge then follows:

`localPlayer -> GUID -> actor manager slot -> sagActor -> actor component -> transform -> position`

The signatures and structure layouts are derived from public MIT-licensed G.R.E-Lab research and are used as research input rather than as a runtime dependency.

This build is **identified**, but runtime compatibility is not considered fully verified until the bridge succeeds on the target executable and the returned position is confirmed in-game.
