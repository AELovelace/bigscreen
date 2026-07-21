# Bigscreen Architecture

## Overview

This sketch turns an ESP32 CYD-style display into a single-user terminal bridge with two simultaneous surfaces:

- A local Telnet server on port `23` that acts as the user-facing control plane.
- An outbound Telnet relay that can connect the user to a remote Telnet host.

The board also renders the active session on the TFT display, including enough ANSI and Telnet protocol handling to keep interactive sessions readable instead of showing raw control bytes.

## Runtime Model

The application is a cooperative event loop. `loop()` does three things every pass:

1. Accept or reject local Telnet clients.
2. Read pending bytes from the local client and translate them into commands or input lines.
3. Pump bytes from a relayed Telnet server to the user/display.

There are no background tasks beyond Arduino/Wi-Fi internals. Shared state lives in global variables, and each service function mutates that state directly.

## Module Layout

### [`bigscreen.ino`](./bigscreen.ino)

The composition root. It owns:

- Includes and board/library notes
- Network configuration
- Shared global state
- `setup()`
- `loop()`

`loop()` now delegates to small runtime service functions so high-level control flow is visible at a glance.

### [`runtime_services.ino`](./runtime_services.ino)

The orchestration layer for live traffic:

- `acceptTelnetClient()`
- `serviceLocalClientInput()`
- `serviceRelayClient()`

This file is the best place to start if behavior changes around reconnects, typing, or byte forwarding.

### [`telnet_protocol.ino`](./telnet_protocol.ino)

Contains the Telnet IAC negotiation filter. It strips and answers Telnet control traffic so the rest of the app can mostly work with plain session bytes.

### [`display_terminal.ino`](./display_terminal.ino)

Owns the TFT-backed terminal model:

- Small sprite framebuffers for the status bar and terminal rows
- Status bar rendering
- Scrollback grid maintenance
- ANSI CSI parsing
- Character placement
- Client re-sync after reconnect

The key architectural idea here is that `termGrid` is the durable terminal history, while sprite-backed framebuffers project dirty rows onto the physical display and reattached Telnet clients.

### [`preview.ino`](./preview.ino)

Handles the temporary magenta input preview. This is intentionally separate from terminal history:

- Local typing is shown immediately.
- The preview never writes into `termGrid`.
- Only remote echoes or server output become permanent scrollback.

That separation avoids duplicate text when a remote endpoint echoes input back.

### [`local_input.ino`](./local_input.ino)

Parses control commands typed by the local user:

- `//connect <host> [port]`
- `//disconnect`

Any non-command line is forwarded to the currently active remote session.

### [`relay_telnet.ino`](./relay_telnet.ino)

Owns outbound Telnet relay lifecycle:

- Start relay
- Stop relay
- Update user-visible status

## State Boundaries

The most important state groups are:

- Network/listener state: `telnetServer`, `telnetClient`, `relayClient`
- Relay state: `relayActive`, `remoteHost`, `remotePort`
- Terminal state: `termGrid`, `curRow`, `curCol`, `ansiState`, `ansiParams`
- Display buffer state: `statusBarSprite`, `rowSprite`, `statusBarText`, `termDirtyRows`
- Local preview state: `lineBuf`, `previewShown`, `previewRow`, `previewCol`, `previewLen`
- Telnet parser state: `localNeg`, `relayNeg`

These are still global because Arduino sketches compile most naturally that way, but they are now grouped around clear module responsibilities.

## Data Flow

### Local input path

1. A Telnet client connects to the ESP32.
2. Bytes are filtered through `telnetFilterByte(...)`.
3. Printable input is echoed back to the client and mirrored to the serial console.
4. The current line is shown via `showPreview()`.
5. On newline, `handleLocalLine(...)` decides whether to run a control command or forward the line to a remote peer.

### Remote Telnet path

1. `startRelay(...)` opens an outbound client connection.
2. `serviceRelayClient()` reads remote bytes.
3. Telnet negotiation bytes are consumed by `telnetFilterByte(...)`.
4. Normal data is forwarded to the local client, serial console, and TFT terminal.

## Framebuffer Strategy

The sketch now uses two compact sprite-backed framebuffers:

- A status bar sprite sized to `320x16`
- A reusable terminal row sprite sized to `320x8`

Remote output and local preview changes mark terminal rows dirty instead of drawing directly to the TFT for every character. At the end of an input or relay batch, `flushDisplay()` re-renders only the changed rows and pushes them to the panel. This keeps the logical terminal model in `termGrid` while reducing the number of slow display transactions.

## Reconnect Behavior

The local Telnet user is intentionally detachable from the active remote session:

- If the local client disconnects, the relay session can continue running.
- When a user reconnects, `resyncClientDisplay(...)` replays terminal history from `termGrid`.
- This makes the local Telnet connection a view/controller, not the owner of the remote session.

## Storage and Security Notes

- The current sketch does not persist any local application data.
- All remote access is plain Telnet, so the relay should only be used on networks you trust.

## Extending the System

For common changes:

- Add a new local command in [`local_input.ino`](./local_input.ino).
- Change reconnect or stream behavior in [`runtime_services.ino`](./runtime_services.ino).
- Improve rendering or ANSI support in [`display_terminal.ino`](./display_terminal.ino).
- Adjust transport behavior in [`relay_telnet.ino`](./relay_telnet.ino).

If the sketch grows much further, the next natural step would be moving shared state into a small set of `.h/.cpp` modules or lightweight classes, but this `.ino` split is a good midpoint that stays friendly to the Arduino IDE.
