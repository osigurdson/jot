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
- Line numbers in a gutter (logical lines, so they match `jot.md` in vim/grep); `:set nonu` hides them.
- OCR (if `tesseract` is installed): text in screenshots is read in the background and
  cached next to each image as `assets/x.png.txt` (word positions in `.tsv`). `/` search finds
  text inside images and highlights the matching words in the screenshot, and
  right-click an image → **Copy Text from Image** (or select it and press Ctrl+Shift+C).
  Without tesseract these features just don't appear.

## Build

    make            # needs gtk4 + libadwaita headers (stock on Arch)
                    # optional, for OCR: sudo pacman -S tesseract tesseract-data-eng
    ./jot
    make install    # ~/.local/bin/jot + app-grid launcher (PREFIX=... to change)
    make check      # headless UI tests (Broadway backend, ASan/UBSan)

## Keys: it's vim

jot is modal, like vim. It opens in normal mode with the cursor at the end of the page.

- **Modes:** normal, insert (`i a I A o O`, `s S`, `c…`), visual `v` / visual line `V`.
  The bottom line shows the mode, keys typed so far, and line,col.
- **Motions:** `h j k l`, `w b e W B E`, `0 ^ $ g_`, `gg G {n}G`, `f F t T ; ,`, `{ }`, `%`, `H M L`,
  `Ctrl+D/U/F/B`, `Ctrl+E/Y`, `zz zt zb`, arrows/Home/End/PgUp/PgDn. Counts work (`3w`, `5j`).
- **Operators:** `d y c < >` with any motion or text object (`dw`, `d3j`, `ci"`, `da(`, `yip`),
  doubled for lines (`dd`, `3yy`, `cc`, `>>`), plus `x X D C Y J r ~ p P`.
- **Text objects:** `iw aw iW aW`, `i" a" i' a' i\` a\``, `i( a( ib`, `i[ a[`, `i{ a{ iB`, `i< a<`, `ip ap`.
- **Undo/redo:** `u`, `Ctrl+R` (an insert session is one undo step).
- **Search:** `/` `?` (incremental, smartcase) then `n N`; `* #` for the word under the cursor.
  Search also finds text inside screenshots (see OCR).
- **Command line:** `:w` `:q` `:wq` `:x`, `:{n}` `:$`, `:[range]s/pat/rep/[gi]` (vim-style
  `\( \) \| \+ \< \>`, `&`, `\1`), `:[range]d|y|>|<|j`, `:set nu|nonu`, `:e!`, `:noh`, `:u` `:redo`.
  Up/Down for history. Ranges: `%`, `.`, `$`, `n`, `'<,'>`, `+n/-n`.
- **Clipboard:** yank/delete/put use the system clipboard (like `clipboard=unnamedplus`), so `p`
  puts a screenshot you just took — on its own line below. `Ctrl+V` in insert mode pastes too.
- **Images:** an image is selected (outlined) when the cursor is on it: move onto its line, or
  click it. Then `yy` copies the image itself, `dd` deletes it, `gx` (or double-click) opens it,
  and `Ctrl+Shift+C` (or right-click → Copy Text from Image) copies its OCR text.
- Insert mode: `Esc`/`Ctrl+[` to leave, `Ctrl+W` deletes a word, `Ctrl+U` to the line start,
  `Tab` indents, Enter keeps indentation.
- Not (yet): `.` repeat, macros, marks, named registers, block visual (`Ctrl+V`).
- Window: `Ctrl+S` save (it autosaves anyway), `Ctrl+Q` quit, `Ctrl +/-/0` zoom.
