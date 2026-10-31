#!/bin/bash
# test_win.sh - jot.exe under Wine, with no display: keys go through the real window procedure.
# Run with `make check-windows`.
set -u
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
export WINEPREFIX=${JOT_WINEPREFIX:-$HOME/.cache/jot-wine} WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
unset DISPLAY WAYLAND_DISPLAY
exe=$PWD/jot.exe; fails=0; n=0
win() { echo "Z:${1//\//\\}"; }
run() { (cd "$tmp" && JOT_DIR="$(win "$tmp/page")" timeout 60 wine "$exe" --selftest "$(win "$tmp/o.bmp")" "$(win "$tmp/o.txt")" "$1"); }
page() { rm -rf "$tmp/page"; mkdir -p "$tmp/page/assets"; printf "$1" > "$tmp/page/jot.md"; }
check() { # name, page (printf format), keys, what jot.md must then contain
    page "$2"; run "$3"; n=$((n + 1))
    if [ "$(cat "$tmp/page/jot.md")" == "$(printf "$4")" ]; then echo "ok    $1"
    else echo "FAIL  $1: $(cut -c1-80 "$tmp/page/jot.md")"; fails=$((fails + 1)); fi
}
E=$'\033'; CR=$'\r'
check "substitute" 'foo foo\nbar\n' ":%s/foo/x/g$CR" 'x x\nbar\n'
check "undo restores" 'hello world\n' ":%s/o/0/g${CR}u" 'hello world\n'
check "search then delete" 'hello world\nfoo bar\nbaz\n' "/bar${CR}dw" 'hello world\nfoo \nbaz\n'
check "yank and put use the clipboard" 'one\ntwo\n' 'ggyyGp' 'one\ntwo\none\n'
check "visual line delete" 'one\ntwo\nthree\n' 'ggVjd' 'three\n'
check "Enter keeps indent, Backspace erases" '  in\n' "A!${CR}xy"$'\x08'"$E" '  in!\n  x\n'
check "accents and emoji" 'x\n' "A é😀$E" 'x é😀\n'
check "Ctrl+V pastes text" 'hi\n' "yyA\\cv$E" 'hihi\n\n'
check "invalid UTF-8 isn't overwritten" 'caf\351\n' "iNEW$E" 'caf\351\n'

page 'a\nb\n'; run '\dp'; n=$((n + 1))
if grep -q '^!\[\](assets/.*\.png)$' "$tmp/page/jot.md" && [ -s "$(ls "$tmp"/page/assets/*.png)" ]; then echo "ok    screenshot paste saves a PNG"
else echo "FAIL  screenshot paste"; fails=$((fails + 1)); fi

for k in '\Pp' "A\\P\\cv$E"; do
    page 'a\nb\n'; run "$k"; n=$((n + 1))
    img=$(ls "$tmp"/page/assets/*.png 2>/dev/null | head -1)
    if grep -q '^!\[\](assets/.*\.png)$' "$tmp/page/jot.md" && [ -s "$img" ]; then echo "ok    a PNG on the clipboard pastes"
    else echo "FAIL  PNG paste with keys $k: $(cut -c1-60 "$tmp/page/jot.md" | tr '\n' '|')"; fails=$((fails + 1)); fi
done
page 'a\n'; run "A\\E\\cv"; n=$((n + 1))
if grep -q 'Nothing to paste' "$tmp/o.txt"; then echo "ok    an empty paste says why"; else echo "FAIL  silent empty paste"; fails=$((fails + 1)); fi

page 'caf\351\n'; run ''; n=$((n + 1))
if grep -q "isn't UTF-8" "$tmp/o.txt"; then echo "ok    an unreadable page says so"; else echo "FAIL  no warning"; fails=$((fails + 1)); fi
page 'a long enough line to measure\n'; run ''; before=$(grep -o 'cols=[0-9]*' "$tmp/o.txt")
run '\D'; after=$(grep -o 'cols=[0-9]*' "$tmp/o.txt"); n=$((n + 1))
if [ "${after#cols=}" -lt "${before#cols=}" ]; then echo "ok    moving to a higher-scale monitor rewraps ($before -> $after)"
else echo "FAIL  rewrap on DPI change: $before -> $after"; fails=$((fails + 1)); fi
[ $fails -eq 0 ] && echo "windows: $n checks passed" || { echo "windows: $fails failed"; exit 1; }
