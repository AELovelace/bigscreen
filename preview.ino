void clearPreview() {
  if (!previewShown) return;
  markTermRowDirty(previewRow);
  previewShown = false;
  previewRow = -1;
  previewCol = -1;
  previewLen = 0;
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

  previewShown = true;
  previewRow = curRow;
  previewCol = curCol;
  previewLen = toShow.length();
  markTermRowDirty(previewRow);
}
