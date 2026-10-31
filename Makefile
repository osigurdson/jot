SHELL   := /bin/bash
PKGS    := gtk4 libadwaita-1
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local
SRCS    := jot.c core.c md.c vim.c ui_gtk.c

jot: $(SRCS:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c jot.h ui.h buf_gtk.h compile_flags.txt
	$(CC) $(CFLAGS) -c -o $@ $<

# For clangd (editor diagnostics / go-to-definition): the same flags the build uses.
compile_flags.txt: Makefile
	@printf '%s\n' -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) > $@

# Headless UI tests (GTK Broadway backend, nothing appears on screen). test_jot.c #includes the
# .c files, so the tests can reach their internals (vim's mode, Markdown's tags...).
test_jot: test_jot.c $(SRCS) jot.h ui.h buf_gtk.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $< $(LDLIBS)

check: test_jot
	@gtk4-broadwayd :17 >/dev/null 2>&1 & pid=$$!; sleep 0.5; \
	tmp=$$(mktemp -d); \
	GDK_BACKEND=broadway BROADWAY_DISPLAY=:17 JOT_DIR=$$tmp/notes XDG_CONFIG_HOME=$$tmp/cfg \
	  JOT_SHOTS=$(JOT_SHOTS) ASAN_OPTIONS=detect_leaks=0 ./test_jot; rc=$$?; \
	kill $$pid; rm -rf $$tmp; exit $$rc

# The engine with no window: the same key scripts against real GTK and against lite.c (the
# platforms without GTK use it), which must leave identical results.
ENGINE_SRCS := test_engine.c core.c md.c vim.c jot.h ui.h buf_gtk.h engine_scripts.inc
PCRE    := $(shell pkg-config --cflags --libs libpcre2-8)
test_engine_gtk: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -DENGINE_HARVEST -fsanitize=address,undefined -o $@ $< $(LDLIBS)

test_engine_lite: $(ENGINE_SRCS) buf.c buf.h lite.c lite.h
	$(CC) -std=gnu11 -g -O1 -Wall -Wextra -DJOT_LITE -DENGINE_HARVEST -fsanitize=address,undefined -o $@ $< buf.c lite.c $(PCRE)

# Without undo the two must match exactly. GTK merges adjacent deletes in its undo history (so `xxu`
# undoes both, unlike vim); lite.c doesn't, and only imitates it for this comparison, not perfectly,
# so scripts with undo may differ slightly.
FUZZ ?= 3000
check-engine: test_engine_gtk test_engine_lite
	@export ASAN_OPTIONS=detect_leaks=0; \
	./test_engine_lite undo-steps; \
	./test_engine_gtk $(FUZZ) no-undo > engine_gtk.txt && ./test_engine_lite $(FUZZ) no-undo > engine_lite.txt || exit 1; \
	diff engine_gtk.txt engine_lite.txt > engine.diff || { echo "engine: GTK and lite DIFFER, see engine.diff"; exit 1; }; \
	echo "engine: $$(grep -cE '^(script|harvest|style|fuzz) [0-9]+$$' engine_gtk.txt) scripts match exactly"; \
	./test_engine_gtk $(FUZZ) > engine_gtk.txt && ./test_engine_lite $(FUZZ) > engine_lite.txt || exit 1; \
	n=$$(diff engine_gtk.txt engine_lite.txt | grep -c '^<   text'); \
	echo "engine: with undo, $$n of $(FUZZ) differ (allowed: GTK's delete merging)"; \
	[ $$((n * 200)) -le $(FUZZ) ] || { diff engine_gtk.txt engine_lite.txt > engine.diff; exit 1; }; \
	rm -f engine_gtk.txt engine_lite.txt engine.diff

install: jot
	install -Dm755 jot $(PREFIX)/bin/jot
	install -Dm644 data/dev.jot.Jot.svg $(PREFIX)/share/icons/hicolor/scalable/apps/dev.jot.Jot.svg
	-gtk-update-icon-cache -q -t $(PREFIX)/share/icons/hicolor
	install -Dm644 /dev/stdin $(PREFIX)/share/applications/dev.jot.Jot.desktop <<< \
	  "$$(printf '[Desktop Entry]\nName=jot\nComment=Quick monospace notes with screenshots\nExec=$(PREFIX)/bin/jot\nIcon=dev.jot.Jot\nTerminal=false\nType=Application\nCategories=Utility;TextEditor;Development;\nStartupWMClass=dev.jot.Jot\n')"

uninstall:
	rm -f $(PREFIX)/bin/jot $(PREFIX)/share/applications/dev.jot.Jot.desktop
	rm -f $(PREFIX)/share/icons/hicolor/scalable/apps/dev.jot.Jot.svg

clean:
	rm -f jot test_jot test_engine_gtk test_engine_lite engine_*.txt engine.diff *.o compile_flags.txt

.PHONY: check check-engine install uninstall clean
