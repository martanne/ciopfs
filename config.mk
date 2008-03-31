# fs version
VERSION = $(shell date +%d.%m.%Y)

# Customize below to fit your system

PREFIX = /usr/local
#PREFIX = /usr
MANPREFIX = ${PREFIX}/share/man

CFLAGS += $(shell pkg-config --cflags fuse) -DFUSE_USE_VERSION=26 -DHAVE_LIBICUUC -DVERSION=\"${VERSION}\" -DNDEBUG -Os
LDFLAGS += $(shell pkg-config --libs fuse) -lattr -lulockmgr -licuuc

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall

CC = cc
