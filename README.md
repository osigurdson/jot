# jot

A tiny native (C, GTK4 / libadwaita) scratch page for programmers: one page of
notes, no files to manage. No server, no network, no dependencies beyond what
GNOME already ships.

- Everything is monospace (your system monospace font). No bold/italic/fonts.
- Paste is always plain text.
- Paste screenshots straight in (or drag image files in). HiDPI screenshots are
  shown at their true size and shrink to fit the window.
- Autosaves to one plain file, `~/notes/jot.md` (override the folder with `$JOT_DIR`).
  Images live in `~/notes/assets/` and appear in the file as `![](assets/x.png)` lines,
  so you can grep, vim, or git it. External edits are picked up on window focus.
- Opens with the cursor at the end, ready for the next note.
- Line numbers in a gutter (logical lines, so they match `jot.md` in vim/grep).
- OCR (if `tesseract` is installed): text in screenshots is read in the background and
  cached next to each image as `assets/x.png.txt`. Ctrl+F finds text inside images, and
  right-click an image → **Copy Text from Image** (or select it and press Ctrl+Shift+C).
  Without tesseract these features just don't appear.

## Build

    make            # needs gtk4 + libadwaita headers (stock on Arch)
                    # optional, for OCR: sudo pacman -S tesseract tesseract-data-eng
    ./jot
    make install    # ~/.local/bin/jot + app-grid launcher (PREFIX=... to change)
    make check      # headless UI tests (Broadway backend, ASan/UBSan)

## Keys

| key                  | action                                    |
|----------------------|-------------------------------------------|
| Ctrl+F               | find, including text inside images (Enter next, Shift+Enter previous, Esc closes) |
| Ctrl+Shift+C         | copy the text of the selected image (OCR) |
| Tab / Shift+Tab      | indent / dedent (works on selected lines) |
| Enter                | new line, keeps indentation               |
| Ctrl +/-/0           | zoom                                      |
| double-click image   | open in your image viewer                 |
| Ctrl+C on an image   | copies the actual image to the clipboard  |
| Ctrl+W / Ctrl+Q      | close / quit                              |
