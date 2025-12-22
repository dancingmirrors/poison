VERSION=	1.6

VERSION!=	[ -d .git ] && \
		echo "git-`git describe --always --abbrev=0`" || \
		echo "${VERSION}"

CC?=		cc
PREFIX?=	/usr/local
PKGLIBS=	x11 xft xrandr xtst xres xext freetype2
CFLAGS+=	-O2 -Wall -Wextra -Wno-unused-parameter \
		-Wunused -Wmissing-prototypes -Wstrict-prototypes \
		`pkg-config --cflags ${PKGLIBS}` \
		-DVERSION=\"${VERSION}\"
LDFLAGS+=	`pkg-config --libs ${PKGLIBS}`

# uncomment to enable debugging
#CFLAGS+=	-g -DDEBUG=1
# and this for subsystem-specific debugging
#CFLAGS+=	-DINPUT_DEBUG=1
#CFLAGS+=	-DSENDCMD_DEBUG=1

BINDIR=		${DESTDIR}$(PREFIX)/bin

SRC!=		ls *.c
OBJ=		${SRC:.c=.o}

BIN=		poison

all: poison

poison: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

install: all
	mkdir -p $(BINDIR)
	install -s $(BIN) $(BINDIR)

regress:
	scan-build $(MAKE)

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all install clean
