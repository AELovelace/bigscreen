void acceptTelnetClient() {
  // Check if a new client is trying to connect
  if (!telnetServer.hasClient()) return;

  if (!telnetClient || !telnetClient.connected()) {
    if (telnetClient) telnetClient.stop(); // Clear old connection
    // Note: an active telnet relay is deliberately left running here so you
    // can disconnect, go do something else, and reconnect later to pick the
    // same session back up.
    lineBuf = "";
    clearPreview();              // Remove any stale in-progress typing left on screen
    localNeg = TelnetNegState(); // Reset telnet IAC negotiation state
    telnetClient = telnetServer.available();

    if (relayActive) {
      resyncClientDisplay(telnetClient, "Reattached to telnet relay: " + remoteHost + ":" + String(remotePort) + " (type //disconnect to end)");
    } else {
      telnetClient.println("Welcome to the ESP32 Telnet Relay!");
      telnetClient.println("Use //connect <host> [port] to relay to another telnet server.");
      telnetClient.println("Use //disconnect to end the relay session.");
    }
    Serial.println("Client connected over Telnet.");
    return;
  }

  // Reject additional clients (only supports one session at a time)
  telnetServer.available().stop();
}

void serviceLocalClientInput() {
  if (!(telnetClient && telnetClient.connected() && telnetClient.available())) return;

  while (telnetClient.available()) {
    char c = telnetClient.read();

    if (telnetFilterByte(localNeg, telnetClient, (uint8_t)c)) {
      continue; // Consumed as telnet IAC negotiation, not session data
    }

    if (c == '\r') {
      continue; // wait for the paired \n
    }

    if (c == '\n') {
      telnetClient.print("\r\n");
      Serial.println();
      String completedLine = lineBuf;
      lineBuf = "";
      clearPreview(); // Cursor position is unchanged - the remote's own reply will land right here
      handleLocalLine(completedLine);
      continue;
    }

    if (c == 8 || c == 127) { // Backspace / Delete
      if (lineBuf.length() > 0) {
        lineBuf.remove(lineBuf.length() - 1);
        telnetClient.write((uint8_t)8);
        telnetClient.write((uint8_t)' ');
        telnetClient.write((uint8_t)8);
        showPreview();
      }
      continue;
    }

    // Echo the character back to the client terminal
    telnetClient.write(c);
    // Also mirror it to the physical Serial Monitor
    Serial.write(c);
    // Draw it live, right at the prompt, as an ephemeral overlay (never
    // committed to termGrid) - see showPreview() for why this avoids
    // duplication entirely instead of trying to detect/suppress an echo.
    lineBuf += c;
    showPreview();
  }

  flushDisplay();
}

void serviceRelayClient() {
  if (!relayActive) return;

  if (!relayClient.connected()) {
    stopRelay("Remote server closed the connection.");
    return;
  }
  if (!relayClient.available()) return;

  clearPreview(); // Let the remote draw on a clean slate at the cursor
  while (relayClient.available()) {
    char c = relayClient.read();
    if (telnetFilterByte(relayNeg, relayClient, (uint8_t)c)) {
      continue; // Consumed as telnet IAC negotiation, not session data
    }
    if (telnetClient && telnetClient.connected()) telnetClient.write(c);
    Serial.write(c);
    termPutChar(c);
  }
  showPreview(); // Re-show any in-progress typing at the (possibly new) cursor position
  flushDisplay();
}
