#include <WiFi.h>
#include <SPI.h>
#include <SPIFFS.h>

/*  Display: "Cheap Yellow Display" ESP32-2432S028R (2.8" 320x240 ILI9341)
    Uses the TFT_eSPI library by Bodmer - the User_Setup.h that ships with the
    library must be configured for the CYD board (see RandomNerdTutorials.com/cyd/) */
#include <TFT_eSPI.h>

/*  SSH client support - install the "LibSSH-ESP32" library by ewpa:
    https://github.com/ewpa/LibSSH-ESP32
    libssh + mbedTLS + WiFi + TFT_eSPI won't fit in the default ~1.2MB OTA app
    slot on a 4MB-flash CYD. In Tools > Partition Scheme pick a scheme with a
    bigger single app partition, e.g. "Minimal SPIFFS (1.9MB APP with OTA/
    190KB SPIFFS)" or "No OTA (2MB APP/2MB SPIFFS)". Only a few KB of SPIFFS
    is actually needed for known_hosts, so the small SPIFFS in those schemes
    is fine. If it still doesn't fit, fall back to "Huge APP (3MB No OTA/1MB
    SPIFFS)", which sacrifices OTA updates for maximum app space. */
#include "libssh_esp32.h"
#include <libssh/libssh.h>

const char* ssid = "TailBoard";
const char* password = "esp32router";

// Static IP configuration for this device
IPAddress staticIP(192, 168, 44, 2);
IPAddress gateway(192, 168, 44, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 44, 1);
IPAddress secondaryDNS(8, 8, 8, 8);

// Initialize the server on standard Telnet port 23
WiFiServer telnetServer(23);
WiFiClient telnetClient;   // The local user connected to us
WiFiClient relayClient;    // Outgoing connection to a remote telnet server

bool relayActive = false;
String remoteHost = "";
int remotePort = 23;

// SSH relay state
bool sshActive = false;
ssh_session sshSession = NULL;
ssh_channel sshChannel = NULL;
String sshHost = "";
String sshUser = "";
int sshPort = 22;

// Buffer that accumulates the characters typed by the local user until Enter
String lineBuf = "";

// ---------------------------------------------------------------------------
// Telnet protocol (IAC) negotiation handling
// ---------------------------------------------------------------------------
// Real telnet servers (like telehack.com) start the session with IAC option
// negotiation (WILL/WONT/DO/DONT ECHO, SUPPRESS-GO-AHEAD, NAWS, etc.). If we
// don't strip these out and answer them, the raw negotiation bytes leak into
// our line buffer / the display as garbage and corrupt whatever gets typed
// or shown, which is why sending lines to a real telnet server could fail.
// This is a minimal NVT-mode client/server: it declines every option offered
// or requested, which is enough for plain text interaction.
struct TelnetNegState {
  int state = 0; // 0 = normal, 1 = saw IAC, 2 = saw IAC+cmd, 3/4 = inside SB...SE
  uint8_t cmd = 0;
};

TelnetNegState localNeg; // Negotiation state for the local user's connection
TelnetNegState relayNeg; // Negotiation state for the outgoing telnet relay

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

// The libssh/mbedtls crypto operations need considerably more stack than the
// default 8KB Arduino loop task, so bump it up (LibSSH-ESP32 example uses 50KB).
size_t getArduinoLoopTaskStackSize(void) {
  return 32768;
}

// ---------------------------------------------------------------------------
// Display / terminal rendering
// ---------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define STATUS_BAR_HEIGHT 16
#define CHAR_W 6
#define CHAR_H 8
#define TERM_COLS (SCREEN_WIDTH / CHAR_W)
#define TERM_ROWS ((SCREEN_HEIGHT - STATUS_BAR_HEIGHT) / CHAR_H)

char termGrid[TERM_ROWS][TERM_COLS];
int curRow = 0;
int curCol = 0;

// VT100/ANSI escape sequence parser state
// 0 = normal text, 1 = saw ESC, 2 = inside a CSI (ESC [ ...) sequence,
// 3 = inside an OSC (ESC ] ...) sequence (e.g. window title, ignored)
int ansiState = 0;
String ansiParams = "";

void updateStatusBar(const String &text) {
  tft.fillRect(0, 0, SCREEN_WIDTH, STATUS_BAR_HEIGHT, TFT_MAGENTA);
  tft.setTextColor(TFT_BLACK, TFT_MAGENTA);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setCursor(2, 4);
  tft.print(text);
}

void termRedraw() {
  tft.fillRect(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH, TERM_ROWS * CHAR_H, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  for (int r = 0; r < TERM_ROWS; r++) {
    tft.setCursor(0, STATUS_BAR_HEIGHT + r * CHAR_H);
    for (int c = 0; c < TERM_COLS; c++) {
      tft.print(termGrid[r][c]);
    }
  }
}

// Redraws just one row (used after a line-erase escape sequence)
void termRedrawRow(int row) {
  tft.fillRect(0, STATUS_BAR_HEIGHT + row * CHAR_H, SCREEN_WIDTH, CHAR_H, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setCursor(0, STATUS_BAR_HEIGHT + row * CHAR_H);
  for (int c = 0; c < TERM_COLS; c++) {
    tft.print(termGrid[row][c]);
  }
}

void termClearGrid() {
  for (int r = 0; r < TERM_ROWS; r++) {
    for (int c = 0; c < TERM_COLS; c++) {
      termGrid[r][c] = ' ';
    }
  }
  curRow = 0;
  curCol = 0;
  tft.fillRect(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH, TERM_ROWS * CHAR_H, TFT_BLACK);
}

void termAdvanceLine() {
  curRow++;
  curCol = 0;
  if (curRow >= TERM_ROWS) {
    // Scroll everything up by one row
    for (int r = 0; r < TERM_ROWS - 1; r++) {
      memcpy(termGrid[r], termGrid[r + 1], TERM_COLS);
    }
    for (int c = 0; c < TERM_COLS; c++) {
      termGrid[TERM_ROWS - 1][c] = ' ';
    }
    curRow = TERM_ROWS - 1;
    termRedraw();
  }
}

// Erases part of the screen for a CSI 'J' (Erase in Display) sequence.
// mode: 0 = cursor to end of screen, 1 = start of screen to cursor, 2/3 = whole screen
void termEraseDisplay(int mode) {
  if (mode == 2 || mode == 3) {
    for (int r = 0; r < TERM_ROWS; r++) {
      for (int c = 0; c < TERM_COLS; c++) termGrid[r][c] = ' ';
    }
  } else if (mode == 1) {
    for (int r = 0; r < curRow; r++) {
      for (int c = 0; c < TERM_COLS; c++) termGrid[r][c] = ' ';
    }
    for (int c = 0; c <= curCol && c < TERM_COLS; c++) termGrid[curRow][c] = ' ';
  } else { // mode == 0 (default)
    for (int c = curCol; c < TERM_COLS; c++) termGrid[curRow][c] = ' ';
    for (int r = curRow + 1; r < TERM_ROWS; r++) {
      for (int c = 0; c < TERM_COLS; c++) termGrid[r][c] = ' ';
    }
  }
  termRedraw();
}

// Erases part of the current line for a CSI 'K' (Erase in Line) sequence.
// mode: 0 = cursor to end of line, 1 = start of line to cursor, 2 = whole line
void termEraseLine(int mode) {
  if (mode == 2) {
    for (int c = 0; c < TERM_COLS; c++) termGrid[curRow][c] = ' ';
  } else if (mode == 1) {
    for (int c = 0; c <= curCol && c < TERM_COLS; c++) termGrid[curRow][c] = ' ';
  } else { // mode == 0 (default)
    for (int c = curCol; c < TERM_COLS; c++) termGrid[curRow][c] = ' ';
  }
  termRedrawRow(curRow);
}

// Handles one completed CSI sequence (ESC [ <params> <cmd>), e.g. cursor
// movement, cursor positioning, and erase display/line. Unsupported commands
// (colors/attributes, cursor show/hide, alternate screen, etc.) are ignored.
void termHandleCSI(char cmd, const String &params) {
  String p = params;
  if (p.length() > 0 && p.charAt(0) == '?') p = p.substring(1); // private-mode prefix

  int nums[4] = { -1, -1, -1, -1 };
  int count = 0;
  int start = 0;
  for (unsigned int i = 0; i <= p.length() && count < 4; i++) {
    if (i == p.length() || p.charAt(i) == ';') {
      String tok = p.substring(start, i);
      nums[count++] = tok.length() > 0 ? tok.toInt() : -1;
      start = i + 1;
    }
  }
  int n1 = (nums[0] > 0) ? nums[0] : 1;

  switch (cmd) {
    case 'A': curRow = max(0, curRow - n1); break;
    case 'B': curRow = min(TERM_ROWS - 1, curRow + n1); break;
    case 'C': curCol = min(TERM_COLS - 1, curCol + n1); break;
    case 'D': curCol = max(0, curCol - n1); break;
    case 'H':
    case 'f': {
      int row = (nums[0] > 0) ? nums[0] : 1;
      int col = (nums[1] > 0) ? nums[1] : 1;
      curRow = constrain(row - 1, 0, TERM_ROWS - 1);
      curCol = constrain(col - 1, 0, TERM_COLS - 1);
      break;
    }
    case 'J': termEraseDisplay(nums[0] > 0 ? nums[0] : 0); break;
    case 'K': termEraseLine(nums[0] > 0 ? nums[0] : 0); break;
    default: break; // e.g. 'm' (SGR colors/attributes) - not emulated
  }
}

// Draws one character of the telnet/SSH session onto the screen, live, as it
// happens. Understands enough VT100/ANSI escape sequences (cursor movement,
// erase line/display) to render real interactive shell sessions reasonably
// instead of dumping raw escape bytes.
void termPutChar(char c) {
  uint8_t uc = (uint8_t)c;

  if (ansiState == 1) { // Just saw ESC
    if (uc == '[') { ansiState = 2; ansiParams = ""; return; }
    if (uc == ']') { ansiState = 3; return; }
    ansiState = 0; // Unsupported escape (e.g. charset select) - swallow it
    return;
  }
  if (ansiState == 2) { // Inside a CSI sequence, collecting parameters
    if ((uc >= '0' && uc <= '9') || uc == ';' || uc == '?') {
      ansiParams += c;
      return;
    }
    termHandleCSI(c, ansiParams); // Final byte terminates the sequence
    ansiState = 0;
    return;
  }
  if (ansiState == 3) { // Inside an OSC sequence, swallow until BEL/ESC
    if (uc == 7 || uc == 27) ansiState = 0;
    return;
  }
  if (uc == 27) { ansiState = 1; return; } // Start of an escape sequence

  if (c == '\r') { curCol = 0; return; }
  if (c == '\n') { termAdvanceLine(); return; }
  if (c == '\b' || c == 127) { if (curCol > 0) curCol--; return; }
  if (c == '\t') {
    curCol = ((curCol / 8) + 1) * 8;
    if (curCol >= TERM_COLS) termAdvanceLine();
    return;
  }
  if (uc < 32) return; // Swallow other unsupported control bytes

  termGrid[curRow][curCol] = c;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setCursor(curCol * CHAR_W, STATUS_BAR_HEIGHT + curRow * CHAR_H);
  tft.print(c);

  curCol++;
  if (curCol >= TERM_COLS) termAdvanceLine();
}

void termPrintLine(const String &s) {
  for (unsigned int i = 0; i < s.length(); i++) termPutChar(s.charAt(i));
  termPutChar('\n');
}

// Redraws the current termGrid contents onto a (re)connecting client so it
// can pick a still-running relay/SSH session back up where it left off,
// instead of starting with a blank terminal. Trailing spaces on each row
// are trimmed for cleanliness.
void resyncClientDisplay(WiFiClient &client, const String &headerMsg) {
  if (!(client && client.connected())) return;
  client.print("\x1b[2J\x1b[H"); // Clear the remote terminal and home its cursor
  if (headerMsg.length() > 0) {
    client.print(headerMsg);
    client.print("\r\n");
  }
  for (int r = 0; r < TERM_ROWS; r++) {
    int lastNonSpace = -1;
    for (int c = 0; c < TERM_COLS; c++) {
      if (termGrid[r][c] != ' ') lastNonSpace = c;
    }
    String rowStr;
    for (int c = 0; c <= lastNonSpace; c++) rowStr += termGrid[r][c];
    client.print(rowStr);
    client.print("\r\n");
  }
}

// ---------------------------------------------------------------------------
// Local input preview (ephemeral, never committed to the scrollback)
// ---------------------------------------------------------------------------
// Typing is drawn as an overlay exactly at the live cursor position (curRow,
// curCol) - i.e. right at the prompt, wherever that currently is - in the
// "writeback" magenta color. It never touches termGrid, so there is nothing
// to de-duplicate: whatever the remote sends (an echo, a chat rebroadcast,
// or anything else) is the only copy that ever becomes permanent history.
// Before any remote data is processed the preview is cleared, and afterwards
// it's redrawn at the (possibly moved) cursor position, so it always tracks
// the live prompt even if background traffic arrives while you're typing.
bool previewShown = false;
int previewRow = -1;
int previewCol = -1;
int previewLen = 0;

void clearPreview() {
  if (!previewShown) return;
  int width = min(previewLen * CHAR_W, SCREEN_WIDTH - previewCol * CHAR_W);
  tft.fillRect(previewCol * CHAR_W, STATUS_BAR_HEIGHT + previewRow * CHAR_H, width, CHAR_H, TFT_BLACK);
  previewShown = false;
}

void showPreview() {
  clearPreview();
  if (lineBuf.length() == 0) return;

  int maxChars = TERM_COLS - curCol;
  if (maxChars <= 0) return; // Cursor is past the edge of the screen - nothing to show
  String toShow = lineBuf;
  if ((int)toShow.length() > maxChars) {
    toShow = toShow.substring(toShow.length() - maxChars); // Show the tail end
  }

  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setCursor(curCol * CHAR_W, STATUS_BAR_HEIGHT + curRow * CHAR_H);
  tft.print(toShow);

  previewShown = true;
  previewRow = curRow;
  previewCol = curCol;
  previewLen = toShow.length();
}

// ---------------------------------------------------------------------------
// Relay handling (//connect and //disconnect)
// ---------------------------------------------------------------------------
#define STATUS_IDLE_TEXT "Not connected - //connect or //ssh"

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
  if (sshActive) stopSSH("");

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

// ---------------------------------------------------------------------------
// SSH relay handling (//ssh and //disconnect)
// ---------------------------------------------------------------------------
// Trust-on-first-use host key check backed by a known_hosts file on SPIFFS.
// Returns false (and refuses the connection) if a previously trusted host key
// has since changed, which is the classic sign of a man-in-the-middle attack.
bool sshVerifyKnownHost(ssh_session session) {
  enum ssh_known_hosts_e state = ssh_session_is_known_server(session);
  switch (state) {
    case SSH_KNOWN_HOSTS_OK:
      return true;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
    case SSH_KNOWN_HOSTS_UNKNOWN:
      if (ssh_session_update_known_hosts(session) != SSH_OK) {
        if (telnetClient && telnetClient.connected()) {
          telnetClient.println("Could not save SSH host key (continuing anyway).");
        }
      } else if (telnetClient && telnetClient.connected()) {
        telnetClient.println("New SSH host key trusted and saved.");
      }
      return true;
    case SSH_KNOWN_HOSTS_CHANGED:
      if (telnetClient && telnetClient.connected()) {
        telnetClient.println("WARNING: SSH host key CHANGED! Possible attack - connection aborted.");
      }
      return false;
    case SSH_KNOWN_HOSTS_OTHER:
      if (telnetClient && telnetClient.connected()) {
        telnetClient.println("WARNING: SSH host key type mismatch - connection aborted.");
      }
      return false;
    default:
      if (telnetClient && telnetClient.connected()) {
        telnetClient.print("SSH host key check error: ");
        telnetClient.println(ssh_get_error(session));
      }
      return false;
  }
}

void stopSSH(const String &reason) {
  if (sshChannel) {
    ssh_channel_close(sshChannel);
    ssh_channel_free(sshChannel);
    sshChannel = NULL;
  }
  if (sshSession) {
    ssh_disconnect(sshSession);
    ssh_free(sshSession);
    sshSession = NULL;
  }
  sshActive = false;
  if (reason.length() > 0 && telnetClient && telnetClient.connected()) telnetClient.println(reason);
  updateStatusBar(STATUS_IDLE_TEXT);
}

void startSSH(String host, int port, String user, String pass) {
  if (sshActive) stopSSH("");
  if (relayActive) stopRelay("");

  if (telnetClient && telnetClient.connected()) {
    telnetClient.print("Connecting via SSH to ");
    telnetClient.print(user);
    telnetClient.print("@");
    telnetClient.print(host);
    telnetClient.print(":");
    telnetClient.println(port);
  }
  updateStatusBar("SSH connecting to " + host + "...");

  sshSession = ssh_new();
  if (sshSession == NULL) {
    if (telnetClient && telnetClient.connected()) telnetClient.println("SSH: out of memory.");
    updateStatusBar(STATUS_IDLE_TEXT);
    return;
  }

  unsigned int portOpt = (unsigned int)port;
  ssh_options_set(sshSession, SSH_OPTIONS_HOST, host.c_str());
  ssh_options_set(sshSession, SSH_OPTIONS_PORT, &portOpt);
  ssh_options_set(sshSession, SSH_OPTIONS_USER, user.c_str());
  ssh_options_set(sshSession, SSH_OPTIONS_KNOWNHOSTS, "/spiffs/known_hosts");

  if (ssh_connect(sshSession) != SSH_OK) {
    if (telnetClient && telnetClient.connected()) {
      telnetClient.print("SSH connection failed: ");
      telnetClient.println(ssh_get_error(sshSession));
    }
    ssh_free(sshSession);
    sshSession = NULL;
    updateStatusBar(STATUS_IDLE_TEXT);
    return;
  }

  if (!sshVerifyKnownHost(sshSession)) {
    ssh_disconnect(sshSession);
    ssh_free(sshSession);
    sshSession = NULL;
    updateStatusBar(STATUS_IDLE_TEXT);
    return;
  }

  if (ssh_userauth_password(sshSession, NULL, pass.c_str()) != SSH_AUTH_SUCCESS) {
    if (telnetClient && telnetClient.connected()) {
      telnetClient.print("SSH authentication failed: ");
      telnetClient.println(ssh_get_error(sshSession));
    }
    ssh_disconnect(sshSession);
    ssh_free(sshSession);
    sshSession = NULL;
    updateStatusBar(STATUS_IDLE_TEXT);
    return;
  }

  sshChannel = ssh_channel_new(sshSession);
  if (sshChannel == NULL ||
      ssh_channel_open_session(sshChannel) != SSH_OK ||
      ssh_channel_request_pty_size(sshChannel, "vt100", TERM_COLS, TERM_ROWS) != SSH_OK ||
      ssh_channel_request_shell(sshChannel) != SSH_OK) {
    if (telnetClient && telnetClient.connected()) {
      telnetClient.print("SSH shell setup failed: ");
      telnetClient.println(ssh_get_error(sshSession));
    }
    if (sshChannel) {
      ssh_channel_close(sshChannel);
      ssh_channel_free(sshChannel);
      sshChannel = NULL;
    }
    ssh_disconnect(sshSession);
    ssh_free(sshSession);
    sshSession = NULL;
    updateStatusBar(STATUS_IDLE_TEXT);
    return;
  }

  sshActive = true;
  sshHost = host;
  sshPort = port;
  sshUser = user;
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println("SSH connected! Type //disconnect to return.");
  }
  termClearGrid();
  updateStatusBar("SSH: " + user + "@" + host + ":" + String(port));
}

// Handles one completed line of input typed by the local user
void handleLocalLine(String line) {
  String trimmed = line;
  trimmed.trim();

  if (trimmed.equalsIgnoreCase("//disconnect")) {
    if (relayActive) {
      stopRelay("Disconnected.");
    } else if (sshActive) {
      stopSSH("Disconnected.");
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

  if (trimmed.startsWith("//ssh")) {
    String rest = trimmed.substring(5);
    rest.trim();

    // Expect: <host> <user> <password> [port]
    String tokens[4];
    int tokenCount = 0;
    String remaining = rest;
    while (tokenCount < 4) {
      remaining.trim();
      if (remaining.length() == 0) break;
      int sp = remaining.indexOf(' ');
      if (sp == -1) {
        tokens[tokenCount++] = remaining;
        break;
      }
      tokens[tokenCount++] = remaining.substring(0, sp);
      remaining = remaining.substring(sp + 1);
    }

    if (tokenCount < 3) {
      if (telnetClient && telnetClient.connected()) {
        telnetClient.println("Usage: //ssh <host> <user> <password> [port]");
      }
      return;
    }

    int port = 22;
    if (tokenCount >= 4) {
      int p = tokens[3].toInt();
      if (p > 0) port = p;
    }
    startSSH(tokens[0], port, tokens[1], tokens[2]);
    return;
  }

  if (relayActive) {
    // Forward the typed line to the remote telnet server
    relayClient.print(line);
    relayClient.print("\r\n");
  } else if (sshActive) {
    // Forward the typed line to the remote SSH shell
    ssh_channel_write(sshChannel, line.c_str(), line.length());
    ssh_channel_write(sshChannel, "\n", 1);
  } else if (telnetClient && telnetClient.connected()) {
    telnetClient.println("No active session. Use //connect <host> [port] or //ssh <host> <user> <password> [port].");
  }
}

void setup() {
  Serial.begin(115200);

  // Start the TFT display
  tft.init();
  tft.setRotation(3); // Landscape, flipped 180 degrees from rotation 1
  tft.fillScreen(TFT_BLACK);
  updateStatusBar("Booting...");
  termClearGrid();

  // Mount SPIFFS (formatting if necessary) so the SSH client can persist its
  // known_hosts file across reboots, and initialize the SSH library.
  SPIFFS.begin(true);
  libssh_begin();

  // Connect to Wi-Fi network with a static IP
  if (!WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Static IP configuration failed.");
  }
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Start listening for Telnet connections
  telnetServer.begin();

  updateStatusBar(STATUS_IDLE_TEXT);
  termPrintLine("ESP32 Telnet/SSH Relay ready.");
  termPrintLine("IP: " + WiFi.localIP().toString());
  termPrintLine("//connect <host> [port] - telnet relay");
  termPrintLine("//ssh <host> <user> <pass> [port] - ssh relay");
  termPrintLine("//disconnect - end the relay session");
}

void loop() {
  // Check if a new client is trying to connect
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop(); // Clear old connection
      // Note: an active telnet relay or SSH session is deliberately left
      // running here so you can disconnect, go do something else, and
      // reconnect later to pick the same session back up.
      lineBuf = "";
      clearPreview();                        // Remove any stale in-progress typing left on screen
      localNeg = TelnetNegState();           // Reset telnet IAC negotiation state
      telnetClient = telnetServer.available();

      if (relayActive) {
        resyncClientDisplay(telnetClient, "Reattached to telnet relay: " + remoteHost + ":" + String(remotePort) + " (type //disconnect to end)");
      } else if (sshActive) {
        resyncClientDisplay(telnetClient, "Reattached to SSH session: " + sshUser + "@" + sshHost + ":" + String(sshPort) + " (type //disconnect to end)");
      } else {
        telnetClient.println("Welcome to the ESP32 Telnet/SSH Relay!");
        telnetClient.println("Use //connect <host> [port] to relay to another telnet server.");
        telnetClient.println("Use //ssh <host> <user> <password> [port] to relay over SSH.");
        telnetClient.println("Use //disconnect to end the relay session.");
      }
      Serial.println("Client connected over Telnet.");
    } else {
      // Reject additional clients (only supports one session at a time)
      telnetServer.available().stop();
    }
  }

  // Handle incoming data from the active local client
  if (telnetClient && telnetClient.connected() && telnetClient.available()) {
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
  }

  // Handle incoming data from the relayed remote telnet server
  if (relayActive) {
    if (!relayClient.connected()) {
      stopRelay("Remote server closed the connection.");
    } else if (relayClient.available()) {
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
    }
  }

  // Handle incoming data from the relayed remote SSH shell
  if (sshActive) {
    if (!ssh_channel_is_open(sshChannel) || ssh_channel_is_eof(sshChannel)) {
      stopSSH("SSH session ended.");
    } else {
      int avail;
      char buf[256];
      bool gotAny = false;
      while ((avail = ssh_channel_poll_timeout(sshChannel, 0, 0)) > 0) {
        int toRead = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
        int nread = ssh_channel_read_nonblocking(sshChannel, buf, toRead, 0);
        if (nread <= 0) break;
        if (!gotAny) { clearPreview(); gotAny = true; } // Let the remote draw on a clean slate
        for (int i = 0; i < nread; i++) {
          if (telnetClient && telnetClient.connected()) telnetClient.write((uint8_t)buf[i]);
          Serial.write((uint8_t)buf[i]);
          termPutChar(buf[i]);
        }
      }
      if (gotAny) showPreview(); // Re-show any in-progress typing at the (possibly new) cursor position
      if (avail < 0) {
        stopSSH("SSH session ended.");
      }
    }
  }
}