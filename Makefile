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

clean:
	@echo cleaning
	@rm -f ciopfs ${OBJ} ciopfs-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p ciopfs-${VERSION}
	@cp -R Makefile config.mk ciopfs.c ciopfs-${VERSION}
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
	@echo creating symlink ${DESTDIR}${PREFIX}/bin/mount.ciopfs
	@ln -s ${DESTDIR}${PREFIX}/bin/ciopfs /sbin/mount.ciopfs
#	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
#	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
#	@sed "s/VERSION/${VERSION}/g" < ciopfs.1 > ${DESTDIR}${MANPREFIX}/man1/ciopfs.1
#	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/ciopfs.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/ciopfs
	@echo removing symlink from /sbin/mount.ciopfs
	@rm -f /sbin/mount.ciopfs
#	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
#	@rm -f ${DESTDIR}${MANPREFIX}/man1/ciopfs.1

.PHONY: all options clean dist install uninstall debug