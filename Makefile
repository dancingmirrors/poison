VERSION=0.94

GIT_SHA!=[ -d .git ] && \
git rev-parse HEAD 2>/dev/null || \
echo "unknown"

CC?=cc
PREFIX?=/usr/local
PKGLIBS=pixman-1 wlroots-0.21 wayland-server xkbcommon libdrm cairo pangocairo xcb
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

TRAY_PKGLIBS=xcb xcb-composite xcb-damage xcb-randr xcb-shape xcb-xfixes cairo-xcb pangocairo
TRAY_CFLAGS=-O3 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
`pkg-config --cflags ${TRAY_PKGLIBS}` -I.
TRAY_LDFLAGS=`pkg-config --libs ${TRAY_PKGLIBS}`

WAYLAND_PROTOCOLS!=pkg-config --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER!=pkg-config --variable=wayland_scanner wayland-scanner

CHECK_SANITIZE?=-fsanitize=address,undefined
CHECK_CFLAGS=-O1 -g -fno-omit-frame-pointer $(CHECK_SANITIZE)
CHECK_ARGS?=
TEST_PKGLIBS=wayland-client xcb
TEST_CFLAGS=-O1 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
`pkg-config --cflags ${TEST_PKGLIBS}` -Itests
TEST_LDFLAGS=`pkg-config --libs ${TEST_PKGLIBS}`
TEST_SOURCES=	tests/runner.c tests/client.c tests/x11.c tests/image.c \
		tests/test-xdg.c tests/test-xwayland.c tests/test-tray.c
TEST_PROTOCOLS=	tests/xdg-shell-client-protocol.h \
		tests/xdg-shell-protocol.c \
		tests/xdg-decoration-unstable-v1-client-protocol.h \
		tests/xdg-decoration-unstable-v1-protocol.c \
		tests/ext-image-capture-source-v1-client-protocol.h \
		tests/ext-image-capture-source-v1-protocol.c \
		tests/ext-image-copy-capture-v1-client-protocol.h \
		tests/ext-image-copy-capture-v1-protocol.c \
		tests/ext-foreign-toplevel-list-v1-protocol.c

BINDIR=${DESTDIR}$(PREFIX)/bin
LIBDIR=${DESTDIR}$(PREFIX)/lib
FONTDIR=${DESTDIR}${PREFIX}/share/fonts
MANDIR=${DESTDIR}${PREFIX}/share/man/man1

PROTOCOLS=	xdg-shell-protocol.h \
		wlr-layer-shell-unstable-v1-protocol.h \
		wlr-layer-shell-unstable-v1-client-protocol.h \
		xdg-shell-client-protocol.h

all: $(PROTOCOLS) poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-tray poison-window-selector

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

poison: poison.c poison-rc.c poison-render.c poison-split.c poison-debug.c poison-device.c poison-decoration.c poison.h $(PROTOCOLS)
	$(CC) $(CFLAGS) -o poison poison.c poison-split.c poison-rc.c poison-render.c poison-debug.c poison-device.c poison-decoration.c $(LDFLAGS) -lm

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

poison-tray: poison-tray.c
	$(CC) $(TRAY_CFLAGS) -o poison-tray poison-tray.c $(TRAY_LDFLAGS)

poison-check: poison.c poison-rc.c poison-render.c poison-split.c poison-debug.c poison-device.c poison-decoration.c poison.h $(PROTOCOLS)
	$(CC) $(CFLAGS) $(CHECK_CFLAGS) -o poison-check poison.c poison-split.c poison-rc.c poison-render.c poison-debug.c poison-device.c poison-decoration.c $(LDFLAGS) -lm $(CHECK_SANITIZE)

tests/xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

tests/xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

tests/xdg-decoration-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

tests/xdg-decoration-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

tests/ext-image-capture-source-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml $@

tests/ext-image-capture-source-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml $@

tests/ext-image-copy-capture-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

tests/ext-image-copy-capture-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

tests/ext-foreign-toplevel-list-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml $@

tests/poison-test: $(TEST_SOURCES) tests/poison-test.h $(TEST_PROTOCOLS)
	$(CC) $(TEST_CFLAGS) -o tests/poison-test $(TEST_SOURCES) \
		tests/xdg-shell-protocol.c \
		tests/xdg-decoration-unstable-v1-protocol.c \
		tests/ext-image-capture-source-v1-protocol.c \
		tests/ext-image-copy-capture-v1-protocol.c \
		tests/ext-foreign-toplevel-list-v1-protocol.c \
		$(TEST_LDFLAGS)

check: poison-check poison-tray tests/poison-test
	./tests/poison-test --compositor ./poison-check $(CHECK_ARGS)

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
	cp poison-tray $(BINDIR)/poison-tray
	cp poison-window-selector $(BINDIR)/poison-window-selector
	cp poison $(BINDIR)/poison
	cp poison-filter.so $(LIBDIR)/poison-filter.so
	cp poison.ttf $(FONTDIR)/poison.ttf
	cp poison.1 $(MANDIR)/poison.1
	fc-cache -f

strip: all
	strip poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-tray poison-window-selector

clean:
	rm -f poison poison-filter.so poison-console poison-indicator poison-launcher poison-notify poison-terminal poison-tray poison-window-selector $(PROTOCOLS) wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.c
	rm -f poison-check tests/poison-test $(TEST_PROTOCOLS)

uninstall:
	rm -f ${BINDIR}/poison
	rm -f $(LIBDIR)/poison-filter.so
	rm -f ${BINDIR}/poison-console
	rm -f ${BINDIR}/poison-indicator
	rm -f ${BINDIR}/poison-launcher
	rm -f ${BINDIR}/poison-notify
	rm -f ${BINDIR}/poison-terminal
	rm -f ${BINDIR}/poison-tray
	rm -f ${BINDIR}/poison-window-selector
	rm -f $(FONTDIR)/poison.ttf
	rm -f $(MANDIR)/poison.1

.PHONY: all check install clean uninstall
