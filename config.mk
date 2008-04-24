# ciopfs version
VERSION = 0.1

# Customize below to fit your system

CC = cc
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

CFLAGS_FUSE  = $(shell pkg-config --cflags fuse) -DFUSE_USE_VERSION=26
LDFLAGS_FUSE = $(shell pkg-config --libs fuse) -lulockmgr

LDFLAGS_XATTR = -lattr

# optional for unicode supprt
CFLAGS_ICU  = -DHAVE_LIBICUUC
LDFLAGS_ICU = -licuuc

CFLAGS  += ${CFLAGS_FUSE}  ${CFLAGS_ICU} -DVERSION=\"${VERSION}\" -DNDEBUG -Os
LDFLAGS += ${LDFLAGS_FUSE} ${LDFLAGS_ICU} ${LDFLAGS_XATTR}

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall
