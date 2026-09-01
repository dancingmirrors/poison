VERSION=0.84

GIT_SHA!=[ -d .git ] && \
git rev-parse HEAD 2>/dev/null || \
echo "unknown"

CC?=cc
PREFIX?=/usr/local
PKGLIBS=pixman-1 wlroots-0.21 wayland-server xkbcommon
CFLAGS+=-O3 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
`pkg-config --cflags ${PKGLIBS}` \
-DVERSION=\"${VERSION}\" \
-DGIT_SHA=\"${GIT_SHA}\" \
-DWLR_USE_UNSTABLE -I. -I/usr/local/include
LDFLAGS+=`pkg-config --libs ${PKGLIBS}`

LAUNCHER_PKGLIBS=wayland-client xkbcommon cairo pangocairo
LAUNCHER_CFLAGS=-O3 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
`pkg-config --cflags ${LAUNCHER_PKGLIBS}` -I.
LAUNCHER_LDFLAGS=`pkg-config --libs ${LAUNCHER_PKGLIBS}`

NOTIFY_PKGLIBS=wayland-client cairo pangocairo dbus-1
NOTIFY_CFLAGS=-O3 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
`pkg-config --cflags ${NOTIFY_PKGLIBS}` -I.
NOTIFY_LDFLAGS=`pkg-config --libs ${NOTIFY_PKGLIBS}`

WAYLAND_PROTOCOLS!=pkg-config --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER!=pkg-config --variable=wayland_scanner wayland-scanner

BINDIR=${DESTDIR}$(PREFIX)/bin
LIBDIR=${DESTDIR}$(PREFIX)/lib
FONTDIR=${DESTDIR}${PREFIX}/share/fonts
MANDIR=${DESTDIR}${PREFIX}/share/man/man1

PROTOCOLS=	xdg-shell-protocol.h \
		wlr-layer-shell-unstable-v1-protocol.h \
		wlr-layer-shell-unstable-v1-client-protocol.h \
		xdg-shell-client-protocol.h

all: $(PROTOCOLS) poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-window-selector

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

wlr-layer-shell-unstable-v1-protocol.h: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) server-header \
		wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-client-protocol.h: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header \
		wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-client-protocol.c: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code \
		wlr-layer-shell-unstable-v1.xml $@

poison: poison.c poison-rc.c poison-render.c poison-split.c poison-debug.c poison.h $(PROTOCOLS)
	$(CC) $(CFLAGS) -o poison poison.c poison-split.c poison-rc.c poison-render.c poison-debug.c $(LDFLAGS) -lm

poison-launcher: poison-launcher.c wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
	$(CC) $(LAUNCHER_CFLAGS) -o poison-launcher poison-launcher.c \
		wlr-layer-shell-unstable-v1-client-protocol.c \
		xdg-shell-client-protocol.c $(LAUNCHER_LDFLAGS)

poison-window-selector: poison-window-selector.c wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
	$(CC) $(LAUNCHER_CFLAGS) -o poison-window-selector poison-window-selector.c \
		wlr-layer-shell-unstable-v1-client-protocol.c \
		xdg-shell-client-protocol.c $(LAUNCHER_LDFLAGS)

poison-indicator: poison-indicator.c wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
	$(CC) $(LAUNCHER_CFLAGS) -o poison-indicator poison-indicator.c \
		wlr-layer-shell-unstable-v1-client-protocol.c \
		xdg-shell-client-protocol.c $(LAUNCHER_LDFLAGS) -lm

poison-console: poison-console.c wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
	$(CC) $(LAUNCHER_CFLAGS) -o poison-console poison-console.c \
		wlr-layer-shell-unstable-v1-client-protocol.c \
		xdg-shell-client-protocol.c $(LAUNCHER_LDFLAGS) -lm

poison-notify: poison-notify.c wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c
	$(CC) $(NOTIFY_CFLAGS) -o poison-notify poison-notify.c \
		wlr-layer-shell-unstable-v1-client-protocol.c \
		xdg-shell-client-protocol.c $(NOTIFY_LDFLAGS) -lm

poison-filter.so: poison-filter.c
	$(CC) -O3 -Wall -Wextra -fPIC -ldl -shared poison-filter.c -o poison-filter.so

poison-terminal: poison-terminal.c poison-terminal.h
	$(MAKE) -f Makefile.terminal all

install: all strip
	mkdir -p $(BINDIR)
	mkdir -p $(LIBDIR)
	mkdir -p $(FONTDIR)
	mkdir -p $(MANDIR)
	cp poison-console $(BINDIR)/poison-console
	cp poison-indicator $(BINDIR)/poison-indicator
	cp poison-launcher $(BINDIR)/poison-launcher
	cp poison-notify $(BINDIR)/poison-notify
	cp poison-terminal $(BINDIR)/poison-terminal
	cp poison-window-selector $(BINDIR)/poison-window-selector
	cp poison $(BINDIR)/poison
	cp poison-filter.so $(LIBDIR)/poison-filter.so
	cp poison.ttf $(FONTDIR)/poison.ttf
	cp poison.1 $(MANDIR)/poison.1
	fc-cache -f

strip: all
	strip poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-window-selector

clean:
	rm -f poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-window-selector $(PROTOCOLS) wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.c

uninstall:
	rm -f ${BINDIR}/poison
	rm -f $(LIBDIR)/poison-filter.so
	rm -f ${BINDIR}/poison-console
	rm -f ${BINDIR}/poison-indicator
	rm -f ${BINDIR}/poison-launcher
	rm -f ${BINDIR}/poison-notify
	rm -f ${BINDIR}/poison-terminal
	rm -f ${BINDIR}/poison-window-selector
	rm -f $(FONTDIR)/poison.ttf
	rm -f $(MANDIR)/poison.1

.PHONY: all install clean uninstall
