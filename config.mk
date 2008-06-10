# ciopfs version
VERSION = 0.1

# Customize below to fit your system

CC = cc
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

CFLAGS_FUSE  = $(shell pkg-config --cflags fuse) -DFUSE_USE_VERSION=26
LDFLAGS_FUSE = $(shell pkg-config --libs fuse) -lulockmgr

LDFLAGS_XATTR = -lattr

# unicode support form glib
CFLAGS_GLIB  = $(shell pkg-config --cflags glib-2.0) -DHAVE_GLIB
LDFLAGS_GLIB = $(shell pkg-config --libs glib-2.0)

# unicode support from libicu from icu-project.org
CFLAGS_ICU  = -DHAVE_LIBICUUC
LDFLAGS_ICU = -licuuc

# unicode flags set this to either {C,LD}FLAGS_GLIB or {C,LD}FLAGS_ICU
CFLAGS_UNICODE  = ${CFLAGS_GLIB}
LDFLAGS_UNICODE = ${LDFLAGS_GLIB}

CFLAGS  += ${CFLAGS_FUSE}  ${CFLAGS_UNICODE} -DVERSION=\"${VERSION}\" -DNDEBUG -Os
LDFLAGS += ${LDFLAGS_FUSE} ${LDFLAGS_UNICODE} ${LDFLAGS_XATTR}

DEBUG_CFLAGS = ${CFLAGS} -UNDEBUG -O0 -g -ggdb -Wall
