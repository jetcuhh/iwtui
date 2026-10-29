CC ?= cc
CSTD = -std=c11
OPTFLAGS ?= -Os -ffunction-sections -fdata-sections
CFLAGS ?= -Wall -Wextra
LDFLAGS ?=

TINY = -flto -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
TINY_LINK = -Wl,--gc-sections -Wl,--build-id=none -s

iwtui: iwtui.c termbox2.h
	$(CC) $(CSTD) $(OPTFLAGS) $(TINY) $(CFLAGS) $(LDFLAGS) $(TINY_LINK) -o $@ iwtui.c

# The default build is already the release build.
release: iwtui

# System-wide when run as root, per-user otherwise.
ifeq ($(shell id -u),0)
PREFIX ?= /usr/local
else
PREFIX ?= $(HOME)/.local
endif

install: iwtui
	install -Dm755 iwtui $(DESTDIR)$(PREFIX)/bin/iwtui

clean:
	rm -f iwtui
.PHONY: release install clean
