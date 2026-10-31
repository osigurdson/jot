SHELL   := /bin/bash
PKGS    := gtk4 libadwaita-1
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local
SRCS    := jot.c md.c vim.c

jot: $(SRCS:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c jot.h compile_flags.txt
	$(CC) $(CFLAGS) -c -o $@ $<

# For clangd (editor diagnostics / go-to-definition): the same flags the build uses.
compile_flags.txt: Makefile
	@printf '%s\n' -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) > $@

# Headless UI tests (GTK Broadway backend, nothing appears on screen). test_jot.c #includes the
# three .c files, so the tests can reach their internals (vim's mode, Markdown's tags...).
test_jot: test_jot.c $(SRCS) jot.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $< $(LDLIBS)

check: test_jot
	@gtk4-broadwayd :17 >/dev/null 2>&1 & pid=$$!; sleep 0.5; \
	tmp=$$(mktemp -d); \
	GDK_BACKEND=broadway BROADWAY_DISPLAY=:17 JOT_DIR=$$tmp/notes XDG_CONFIG_HOME=$$tmp/cfg \
	  JOT_SHOTS=$(JOT_SHOTS) ASAN_OPTIONS=detect_leaks=0 ./test_jot; rc=$$?; \
	kill $$pid; rm -rf $$tmp; exit $$rc

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
	rm -f jot test_jot *.o compile_flags.txt

.PHONY: check install uninstall clean
