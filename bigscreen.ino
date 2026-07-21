#include <WiFi.h>
#include <SPI.h>
/*  Display: "Cheap Yellow Display" ESP32-2432S028R (2.8" 320x240 ILI9341)
    Uses the TFT_eSPI library by Bodmer - the User_Setup.h that ships with the
    library must be configured for the CYD board (see RandomNerdTutorials.com/cyd/) */
#include <TFT_eSPI.h>

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

// ---------------------------------------------------------------------------
// Display / terminal rendering
// ---------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite statusBarSprite = TFT_eSprite(&tft);
TFT_eSprite rowSprite = TFT_eSprite(&tft);

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define STATUS_BAR_HEIGHT 16
#define CHAR_W 6
#define CHAR_H 8
#define TERM_COLS (SCREEN_WIDTH / CHAR_W)
#define TERM_ROWS ((SCREEN_HEIGHT - STATUS_BAR_HEIGHT) / CHAR_H)

char termGrid[TERM_ROWS][TERM_COLS];
bool termDirtyRows[TERM_ROWS];
int curRow = 0;
int curCol = 0;
bool statusBarDirty = false;
String statusBarText = "";

// VT100/ANSI escape sequence parser state
// 0 = normal text, 1 = saw ESC, 2 = inside a CSI (ESC [ ...) sequence,
// 3 = inside an OSC (ESC ] ...) sequence (e.g. window title, ignored)
int ansiState = 0;
String ansiParams = "";

// Local input preview is an overlay exactly at the current remote cursor.
bool previewShown = false;
int previewRow = -1;
int previewCol = -1;
int previewLen = 0;

#define STATUS_IDLE_TEXT "Not connected - use //connect"

void setup() {
  Serial.begin(115200);

  // Start the TFT display
  tft.init();
  tft.setRotation(3); // Landscape, flipped 180 degrees from rotation 1
  tft.fillScreen(TFT_BLACK);
  initDisplayFramebuffers();
  updateStatusBar("Booting...");
  termClearGrid();

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
  termPrintLine("ESP32 Telnet Relay ready.");
  termPrintLine("IP: " + WiFi.localIP().toString());
  termPrintLine("//connect <host> [port] - telnet relay");
  termPrintLine("//disconnect - end the relay session");
}

void loop() {
  acceptTelnetClient();
  serviceLocalClientInput();
  serviceRelayClient();
}
