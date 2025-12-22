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
LDFLAGS+=	`pkg-config --libs ${PKGLIBS}` -lm

COMMONER_PKGLIBS=	x11 xcomposite xdamage xfixes xext xpresent egl gl xshmfence
COMMONER_CFLAGS=	-O3 -Wall -Wextra `pkg-config --cflags ${COMMONER_PKGLIBS}` \
			-DVERSION=\"${VERSION}\"
COMMONER_LDFLAGS=	`pkg-config --libs ${COMMONER_PKGLIBS}` -lm

CFLAGS+=	-g -DDEBUG=1
#CFLAGS+=	-DINPUT_DEBUG=1
#CFLAGS+=	-DSENDCMD_DEBUG=1

BINDIR=		${DESTDIR}$(PREFIX)/bin

SRC!=		ls *.c | grep -v commoner.c
OBJ=		${SRC:.c=.o}

BIN=		poison commoner

all: poison commoner

poison: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

commoner.o: commoner.c commoner.h
	$(CC) $(COMMONER_CFLAGS) -c commoner.c

commoner: commoner.o
	$(CC) -o $@ commoner.o $(COMMONER_LDFLAGS)

install: all
	mkdir -p $(BINDIR)
	install -s poison $(BINDIR)
	install -s commoner $(BINDIR)

regress:
	scan-build $(MAKE)

clean:
	rm -f poison commoner $(OBJ) commoner.o

.PHONY: all install clean
