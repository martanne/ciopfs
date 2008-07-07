include config.mk

SRC += ciopfs.c
OBJ = ${SRC:.c=.o}

all: clean options ciopfs

options:
	@echo ciopfs build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.mk

ciopfs: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

debug: clean
	@make CFLAGS='${DEBUG_CFLAGS}'

ascii: clean
	@make CFLAGS_UNICODE='' LDFLAGS_UNICODE=''

unicode-glib: clean
	@make CFLAGS_UNICODE='${CFLAGS_GLIB}' LDFLAGS_UNICODE='${LDFLAGS_GLIB}'

unicode-icu: clean
	@make CFLAGS_UNICODE='${CFLAGS_ICU}' LDFLAGS_UNICODE='${LDFLAGS_ICU}'

clean:
	@echo cleaning
	@rm -f ciopfs ${OBJ} ciopfs-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p ciopfs-${VERSION}
	@cp -R Makefile config.mk ciopfs.c ascii.c unicode-icu.c unicode-glib.c ciopfs-${VERSION}
	@tar -cf ciopfs-${VERSION}.tar ciopfs-${VERSION}
	@gzip ciopfs-${VERSION}.tar
	@rm -rf ciopfs-${VERSION}

install: ciopfs
	@echo stripping executable
	@strip -s ciopfs
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f ciopfs ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/ciopfs
	@echo creating symlink ${DESTDIR}/sbin/mount.ciopfs
	@mkdir -p ${DESTDIR}/sbin
	@ln -sf ${PREFIX}/bin/ciopfs ${DESTDIR}/sbin/mount.ciopfs
#	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
#	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
#	@sed "s/VERSION/${VERSION}/g" < ciopfs.1 > ${DESTDIR}${MANPREFIX}/man1/ciopfs.1
#	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/ciopfs.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/ciopfs
	@echo removing symlink from ${DESTDIR}/sbin/mount.ciopfs
	@rm -f ${DESTDIR}/sbin/mount.ciopfs
#	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
#	@rm -f ${DESTDIR}${MANPREFIX}/man1/ciopfs.1

.PHONY: all options clean dist install uninstall debug ascii unicode-glib unicode-icu
