// Handles one completed line of input typed by the local user
void handleLocalLine(String line) {
  String trimmed = line;
  trimmed.trim();

  if (trimmed.equalsIgnoreCase("//disconnect")) {
    if (relayActive) {
      stopRelay("Disconnected.");
    } else if (telnetClient && telnetClient.connected()) {
      telnetClient.println("Not connected to any remote server.");
    }
    return;
  }

  if (trimmed.startsWith("//connect")) {
    String rest = trimmed.substring(9);
    rest.trim();
    if (rest.length() == 0) {
      if (telnetClient && telnetClient.connected()) {
        telnetClient.println("Usage: //connect <host> [port]");
      }
      return;
    }
    int spaceIdx = rest.indexOf(' ');
    String host;
    int port = 23;
    if (spaceIdx == -1) {
      host = rest;
    } else {
      host = rest.substring(0, spaceIdx);
      String portStr = rest.substring(spaceIdx + 1);
      portStr.trim();
      if (portStr.length() > 0) {
        int p = portStr.toInt();
        if (p > 0) port = p;
      }
    }
    startRelay(host, port);
    return;
  }

  if (relayActive) {
    // Forward the typed line to the remote telnet server
    relayClient.print(line);
    relayClient.print("\r\n");
  } else if (telnetClient && telnetClient.connected()) {
    telnetClient.println("No active session. Use //connect <host> [port].");
  }
}
