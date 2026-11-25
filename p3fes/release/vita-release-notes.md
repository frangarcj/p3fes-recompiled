## Persona 3 FES for PS Vita @VERSION@ (beta)

**This is an early beta.** Expect crashes, missing effects and slow scenes. It is a static recompilation of the
USA PS2 executable (SLUS-21621), so it contains no game data: you need your own copy of the disc.

Build: `@COMMIT@`, Vita app version `@APP_VER@`, @DATE@.

### Gameplay controls

Open **System → Config** and press **R** for the gameplay settings. Select **Confirm** to save changes.

- **Dialogue history** is on by default. During a conversation, press **SELECT** to read recent lines over the dimmed scene. Use **Up/Down** to browse and press **SELECT** again to return to the conversation.
- **Direct party control** lets you choose commands for each active companion starting with the next battle. Rush and forced actions still follow the game's AI.
- **Scene fast forward** speeds up scripted scenes while you hold **Triangle**, with a VHS tracking effect. Release it to resume normal speed. The game waits for your dialogue choices.

### Requirements

- A PS Vita or PS TV with HENkaku/Ensō and taiHEN installed (the app imports taiHEN).
- About 6 GB free on `ux0:` for the game data, plus the VPK itself.
- Your own SLUS-21621 (USA) ISO.

### Install

1. Install `@VPK_NAME@` with VitaShell (copy it to the Vita, press Cross on it and confirm the install).
2. Open the data tool at <@TOOL_URL@> in a browser, pick your ISO and download the zip.
   Nothing is uploaded: the zip is built on your computer.
3. Extract the zip into the root of `ux0:` (it creates `ux0:data/p3fes/`).
4. Launch "Persona 3 FES" from the LiveArea.

### Updating

Install the new VPK over the old one. Saves and the data in `ux0:data/p3fes/` are kept; you do not need to
generate the data again unless the release notes say so.

### Reporting bugs

Attach `ux0:data/p3fes/runtime.log` (it is rewritten on every launch, so copy it before starting the game again) and
say what you were doing, which Vita model and which firmware you use.

### Known limits of the beta

- Performance varies by scene and can fall below 30 fps.
- Only the USA release is supported; other regions and other executables are rejected.
- Features that are not finished may be switched off or look different from the PS2.

### Files

`@VPK_NAME@` SHA-256: `@VPK_SHA256@` (also in `SHA256SUMS`).
