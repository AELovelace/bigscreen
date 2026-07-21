void initDisplayFramebuffers() {
  statusBarSprite.setColorDepth(8);
  statusBarSprite.createSprite(SCREEN_WIDTH, STATUS_BAR_HEIGHT);
  rowSprite.setColorDepth(8);
  rowSprite.createSprite(SCREEN_WIDTH, CHAR_H);

  for (int r = 0; r < TERM_ROWS; r++) {
    termDirtyRows[r] = false;
  }
}

void markTermRowDirty(int row) {
  if (row >= 0 && row < TERM_ROWS) {
    termDirtyRows[row] = true;
  }
}

void markAllTermRowsDirty() {
  for (int r = 0; r < TERM_ROWS; r++) {
    termDirtyRows[r] = true;
  }
}

void flushStatusBar() {
  if (!statusBarDirty) return;

  statusBarSprite.fillSprite(TFT_MAGENTA);
  statusBarSprite.setTextColor(TFT_BLACK, TFT_MAGENTA);
  statusBarSprite.setTextFont(1);
  statusBarSprite.setTextSize(1);
  statusBarSprite.setCursor(2, 4);
  statusBarSprite.print(statusBarText);
  statusBarSprite.pushSprite(0, 0);
  statusBarDirty = false;
}

void flushTermRow(int row) {
  if (row < 0 || row >= TERM_ROWS || !termDirtyRows[row]) return;

  char rowChars[TERM_COLS + 1];
  memcpy(rowChars, termGrid[row], TERM_COLS);
  rowChars[TERM_COLS] = '\0';

  rowSprite.fillSprite(TFT_BLACK);
  rowSprite.setTextColor(TFT_CYAN, TFT_BLACK);
  rowSprite.setTextFont(1);
  rowSprite.setTextSize(1);
  rowSprite.setCursor(0, 0);
  rowSprite.print(rowChars);

  if (previewShown && previewRow == row && previewLen > 0) {
    String previewText = lineBuf;
    if ((int)previewText.length() > previewLen) {
      previewText = previewText.substring(previewText.length() - previewLen);
    }
    rowSprite.setTextColor(TFT_MAGENTA, TFT_BLACK);
    rowSprite.setCursor(previewCol * CHAR_W, 0);
    rowSprite.print(previewText);
  }

  rowSprite.pushSprite(0, STATUS_BAR_HEIGHT + row * CHAR_H);
  termDirtyRows[row] = false;
}

void flushDisplay() {
  flushStatusBar();
  for (int r = 0; r < TERM_ROWS; r++) {
    flushTermRow(r);
  }
}

void updateStatusBar(const String &text) {
  statusBarText = text;
  statusBarDirty = true;
  flushStatusBar();
}

void termRedraw() {
  markAllTermRowsDirty();
  flushDisplay();
}

// Redraws just one row (used after a line-erase escape sequence)
void termRedrawRow(int row) {
  markTermRowDirty(row);
  flushTermRow(row);
}

void termClearGrid() {
  for (int r = 0; r < TERM_ROWS; r++) {
    for (int c = 0; c < TERM_COLS; c++) {
      termGrid[r][c] = ' ';
    }
  }
  curRow = 0;
  curCol = 0;
  markAllTermRowsDirty();
  flushDisplay();
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
    markAllTermRowsDirty();
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

// Draws one character of the relayed telnet session onto the screen, live, as it
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
  markTermRowDirty(curRow);

  curCol++;
  if (curCol >= TERM_COLS) termAdvanceLine();
}

void termPrintLine(const String &s) {
  for (unsigned int i = 0; i < s.length(); i++) termPutChar(s.charAt(i));
  termPutChar('\n');
  flushDisplay();
}

// Redraws the current termGrid contents onto a (re)connecting client so it
// can pick a still-running relay session back up where it left off,
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
