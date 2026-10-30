SHELL   := /bin/bash
PKGS    := gtk4 libadwaita-1
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local

jot: jot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Headless UI tests (GTK Broadway backend, nothing appears on screen).
test_jot: test_jot.c jot.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $< $(LDLIBS)

check: test_jot
	@gtk4-broadwayd :17 >/dev/null 2>&1 & pid=$$!; sleep 0.5; \
	tmp=$$(mktemp -d); \
	GDK_BACKEND=broadway BROADWAY_DISPLAY=:17 JOT_DIR=$$tmp/notes XDG_CONFIG_HOME=$$tmp/cfg \
	  JOT_SHOTS=$(JOT_SHOTS) ASAN_OPTIONS=detect_leaks=0 ./test_jot; rc=$$?; \
	kill $$pid; rm -rf $$tmp; exit $$rc

install: jot
	install -Dm755 jot $(PREFIX)/bin/jot
	install -Dm644 /dev/stdin $(PREFIX)/share/applications/dev.jot.Jot.desktop <<< \
	  "$$(printf '[Desktop Entry]\nName=jot\nComment=Quick monospace notes with screenshots\nExec=$(PREFIX)/bin/jot\nIcon=accessories-text-editor\nTerminal=false\nType=Application\nCategories=Utility;TextEditor;Development;\nStartupWMClass=dev.jot.Jot\n')"

uninstall:
	rm -f $(PREFIX)/bin/jot $(PREFIX)/share/applications/dev.jot.Jot.desktop

clean:
	rm -f jot test_jot

.PHONY: check install uninstall clean
