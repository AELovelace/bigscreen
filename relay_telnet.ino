void stopRelay(const String &reason) {
  relayClient.stop();
  relayActive = false;
  if (reason.length() > 0 && telnetClient && telnetClient.connected()) telnetClient.println(reason);
  updateStatusBar(STATUS_IDLE_TEXT);
}

void startRelay(String host, int port) {
  if (relayActive) {
    relayClient.stop();
    relayActive = false;
  }

  if (telnetClient && telnetClient.connected()) {
    telnetClient.print("Connecting to ");
    telnetClient.print(host);
    telnetClient.print(":");
    telnetClient.println(port);
  }
  updateStatusBar("Connecting to " + host + ":" + String(port) + "...");
  relayNeg = TelnetNegState(); // Reset telnet IAC negotiation state

  if (relayClient.connect(host.c_str(), port)) {
    relayActive = true;
    remoteHost = host;
    remotePort = port;
    if (telnetClient && telnetClient.connected()) {
      telnetClient.println("Connected! Type //disconnect to return.");
    }
    termClearGrid();
    updateStatusBar("Relay: " + host + ":" + String(port));
  } else {
    if (telnetClient && telnetClient.connected()) {
      telnetClient.println("Connection failed.");
    }
    updateStatusBar(STATUS_IDLE_TEXT);
  }
}
