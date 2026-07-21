# Bigscreen Engineering Onboarding

## What This Project Is

`bigscreen` is an ESP32 terminal bridge for the CYD display board. A user connects to the board over Telnet, then uses local commands to open a remote Telnet relay session.

The remote session is rendered on the TFT screen and mirrored back to the connected Telnet user.

## Hardware and Library Expectations

### Board assumptions

- ESP32-2432S028R / CYD-style board
- ILI9341 display
- Standard ESP32 app space for Wi-Fi and `TFT_eSPI`

### Required libraries

- `TFT_eSPI`
- Core ESP32 Arduino libraries for Wi-Fi and SPI

`TFT_eSPI` also has to be configured for the CYD board in its `User_Setup.h`.

## File Tour

- [`bigscreen.ino`](./bigscreen.ino): shared configuration, globals, `setup()`, `loop()`
- [`runtime_services.ino`](./runtime_services.ino): client acceptance and byte-pumping services
- [`display_terminal.ino`](./display_terminal.ino): terminal grid, sprite framebuffers, ANSI handling, redraw logic
- [`preview.ino`](./preview.ino): local input preview overlay
- [`telnet_protocol.ino`](./telnet_protocol.ino): Telnet option filtering
- [`local_input.ino`](./local_input.ino): local command parsing and forwarding decisions
- [`relay_telnet.ino`](./relay_telnet.ino): outbound Telnet session lifecycle
- [`architecture.md`](./architecture.md): deeper architecture and data-flow reference

## How To Reason About The App

Think of the app as three layers:

1. Local control endpoint
   The ESP32 hosts a Telnet server that the user connects to.

2. Transport adapters
   The board speaks to a remote Telnet server.

3. Shared terminal presentation
   All remote output is normalized into a single on-device terminal model and mirrored to the local client.

This means the local Telnet client is not the session itself. It is a controller/view over a longer-lived relay state.

## Common Developer Workflows

### Add a new slash command

1. Edit [`local_input.ino`](./local_input.ino).
2. Handle the new command before the generic forwarding branch.
3. Decide whether the command should mutate relay state, display state, or both.
4. Add user-facing guidance to the startup banner in [`bigscreen.ino`](./bigscreen.ino) if needed.

### Change TFT rendering behavior

1. Start in [`display_terminal.ino`](./display_terminal.ino).
2. Decide whether the change belongs to permanent history (`termGrid`) or temporary input preview (`preview.ino`).
3. Keep the dirty-row framebuffer flow intact so we still batch TFT writes.
4. Test with real interactive shells because ANSI edge cases only show up with real traffic.

### Change reconnect behavior

1. Start in [`runtime_services.ino`](./runtime_services.ino).
2. Review `acceptTelnetClient()` and `resyncClientDisplay(...)`.
3. Preserve the distinction between:
   - remote session lifetime
   - local viewer connection lifetime

## Debugging Checklist

### Device boots but display is wrong

- Verify `TFT_eSPI` board configuration.
- Check `tft.setRotation(3)` assumptions.
- Confirm CYD wiring matches the configured driver/setup.

### Sketch does not fit

- Check partition scheme first.
- Avoid adding large static buffers casually.

### Telnet session shows garbage characters

- Inspect [`telnet_protocol.ino`](./telnet_protocol.ino).
- The issue is often Telnet negotiation bytes leaking through.

### Typing appears duplicated

- Inspect the preview model in [`preview.ino`](./preview.ino).
- Permanent text should come from remote echo/output, not from local preview rendering.

### Screen drawing still feels slow

- Inspect dirty-row marking and `flushDisplay()` in [`display_terminal.ino`](./display_terminal.ino).
- Confirm new code is not forcing full-screen redraws when a single row would do.

### Reconnect works but terminal history is odd

- Inspect `termGrid` mutation paths in [`display_terminal.ino`](./display_terminal.ino).
- Inspect replay logic in `resyncClientDisplay(...)`.

## Current Technical Risks

- Global mutable state is simple but makes cross-module coupling easy to introduce.
- ANSI emulation is intentionally partial; unusual terminal apps may still render imperfectly.
- The app is single-user by design and rejects concurrent local clients.
- All remote access is plain Telnet, so the system should stay on trusted networks.

## Suggested First Tasks For A New Engineer

1. Read [`architecture.md`](./architecture.md).
2. Read [`bigscreen.ino`](./bigscreen.ino) and [`runtime_services.ino`](./runtime_services.ino) together.
3. Trace one full Telnet session through the code.
4. Identify one safe improvement:
   - better command help text
   - an additional local command
   - richer status bar messaging
   - broader ANSI support

## Definition Of Done For Future Changes

Before considering a change complete, verify:

- The sketch still builds with the required libraries and partition scheme.
- Local Telnet command handling still works.
- Telnet relay still connects and forwards traffic.
- The TFT display still shows scrollback correctly.
- Reconnect still restores the visible session instead of presenting a blank screen.
