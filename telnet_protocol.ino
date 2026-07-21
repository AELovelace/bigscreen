// Feeds one raw byte through the telnet IAC state machine for the given
// connection. Returns true if the byte was consumed as protocol negotiation
// (and should NOT be treated as session data), sending any required reply
// directly back on `conn`. Returns false if `c` is a normal data byte.
bool telnetFilterByte(TelnetNegState &st, WiFiClient &conn, uint8_t c) {
  const uint8_t IAC = 255, WILL = 251, WONT = 252, DO = 253, DONT = 254, SB = 250, SE = 240;

  if (st.state == 0) {
    if (c == IAC) { st.state = 1; return true; }
    return false;
  }
  if (st.state == 1) {
    if (c == IAC) { st.state = 0; return false; } // Escaped 0xFF -> literal data byte
    if (c == WILL || c == WONT || c == DO || c == DONT) { st.cmd = c; st.state = 2; return true; }
    if (c == SB) { st.state = 3; return true; }
    st.state = 0; // Other 2-byte command (NOP, GA, etc.) - consumed, no reply needed
    return true;
  }
  if (st.state == 2) {
    if (st.cmd == WILL) {
      uint8_t resp[3] = { IAC, DONT, c };
      conn.write(resp, 3);
    } else if (st.cmd == DO) {
      uint8_t resp[3] = { IAC, WONT, c };
      conn.write(resp, 3);
    }
    st.state = 0;
    return true;
  }
  if (st.state == 3) { // Inside SB...SE subnegotiation, discard until IAC SE
    if (c == IAC) st.state = 4;
    return true;
  }
  // st.state == 4: saw IAC while inside subnegotiation
  if (c == SE) st.state = 0;
  else if (c != IAC) st.state = 3;
  return true;
}
