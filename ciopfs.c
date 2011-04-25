/*
 * CIOPFS - The Case Insensitive On Purpose Filesystem for FUSE
 *
 * (c) 2008 Marc Andre Tanner <mat at brain-dump dot org>
 * (c) 2001-2007 Miklos Szeredi <miklos@szeredi.hu>
 *
 * This program can be distributed under the terms of the GNU GPLv2.
 *
 * How it works:
 * 	Before any operation takes place all filenames are
 * 	converted to lower case. The original filenames are stored
 * 	in extended attributes named user.filename. This value
 * 	is returned upon request.
 *
 * 	Files or directories which aren't all lowercase in the
 * 	underlying file system are ignored. You should probably
 * 	start with an empty data directory and copy your contents
 * 	over.
 *
 * Requirements:
 * 	In order to compile ciopfs, you will need both
 * 	libfuse and libattr. Furthermore if you want a case
 * 	preserving file system you have to make sure that the
 * 	underlying file system supports extended attributes
 * 	(for example for ext{2,3} you need a kernel with
 * 	CONFIG_EXT{2,3}_FS_XATTR enabled). You probably also
 * 	want to mount the underlying filesystem with the
 * 	user_xattr option which allows non root users to create
 * 	extended attributes.
 *
 * 	If you want to work with unicode characters within file
 * 	names, you will need libicu from www.icu-project.org.
 * 	Otherwise disable it in config.mk, the file system will
 * 	only work with ascii [a-zA-Z] file names.
 *
 * Compile & Install:
 * 	$EDITOR config.mk
 * 	make
 * 	sudo make install
 *
 * Mount:
 * 	ciopfs directory mountpoint [options]
 *
 */

#ifdef __linux__
#define _XOPEN_SOURCE 500 /* For pread()/pwrite() */
#endif

#define _BSD_SOURCE /* for vsyslog() */

#include <fuse.h>
#include <ulockmgr.h>
#include <sys/xattr.h>
#include <sys/time.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <syslog.h>
#include <grp.h>

#if __GNUC__ >= 3
# define likely(x)       __builtin_expect(!!(x), 1)
# define unlikely(x)     __builtin_expect(!!(x), 0)
#else
# define likely(x)       (x)
# define unlikely(x)     (x)
#endif

/* each *.c file implements the following two functions:
 *
 * static inline bool str_contains_upper(const char *s);
 * static inline char *str_fold(const char *s);
 */
#ifdef HAVE_GLIB
# include "unicode-glib.c"
#elif defined HAVE_LIBICUUC
# include "unicode-icu.c"
#else
# include "ascii.c"
#endif

#define log_print(format, args...) (*dolog)(format, ## args)

#ifdef NDEBUG
# define debug(format, args...)
#else
# define debug log_print
#endif

#define CIOPFS_ATTR_NAME "user.filename"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef FILENAME_MAX
#define FILENAME_MAX 4096
#endif

static const char *dirname;
/* whether we have forced fuse in single threaded mode (`-s' option). This happens
 * because we can't store uid/gids per thread and the file system is accessible for
 * multiple users via the `-o allow_other' option.
 */
static bool single_threaded = false;

void stderr_print(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("ciopfs: ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void syslog_print(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsyslog(LOG_NOTICE, fmt, ap);
	va_end(ap);
}

static void (*dolog)(const char *fmt, ...) = syslog_print;

static char *map_path(const char *path)
{
	char *p;
	/* XXX: memory fragmentation? */
	if (path[0] == '/') {
		if (path[1] == '\0')
			return strdup(".");
		path++;
	}

	p = str_fold(path);
	debug("%s => %s\n", path, p);
	return p;
}

/* Returns the supplementary group IDs of a calling process which
 * isued the file system operation.
 *
 * As indicated by Miklos Szeredi the group list is available in
 *
 *   /proc/$PID/task/$TID/status
 *
 * and fuse supplies TID in get_fuse_context()->pid.
 *
 * Jean-Pierre Andre found out that the same information is also
 * available in
 *
 *   /proc/$TID/task/$TID/status
 *
 * which is used in this implementation.
 */

static size_t get_groups(pid_t pid, gid_t **groups)
{
	static char key[] = "\nGroups:\t";
	char filename[64], buf[2048], *s, *t, c = '\0';
	int fd, num_read, matched = 0;
	size_t n = 0;
	gid_t *gids, grp = 0;

	sprintf(filename, "/proc/%u/task/%u/status", pid, pid);
	fd = open(filename, O_RDONLY);
	if (fd == -1)
		return 0;

	for (;;) {
		if (!c) {
			num_read = read(fd, buf, sizeof(buf) - 1);
			if (num_read <= 0) {
				close(fd);
				return 0;
			}
			buf[num_read] = '\0';
			s = buf;
		}

		c = *s++;

		if (key[matched] == c) {
			if (!key[++matched])
				break;

		} else
			matched = (key[0] == c);
	}

	close(fd);
	t = s;
	n = 0;

	while (*t != '\n') {
		if (*t++ == ' ')
			n++;
	}

	if (n == 0)
		return 0;

	*groups = gids = malloc(n * sizeof(gid_t));
	if (!gids)
		return 0;
	n = 0;

	while ((c = *s++) != '\n') {
		if (c >= '0' && c <= '9')
			grp = grp*10 + c - '0';
		else if (c == ' ') {
			gids[n++] = grp;
			grp = 0;
		}
	}

	return n;
}

/* This only works when the filesystem is mounted by root and fuse
 * operates in single threaded mode. Because the euid/egid are stored
 * per process this would otherwise cause all sorts of race condidtions
 * and security issues when multiple users access the file system
 * simultaneously.
 */

static inline void enter_user_context_effective()
{
	gid_t *groups;
	size_t ngroups;
	struct fuse_context *c = fuse_get_context();

	if (!single_threaded || getuid())
		return;
	if ((ngroups = get_groups(c->pid, &groups))) {
		setgroups(ngroups, groups);
		free(groups);
	}

	setegid(c->gid);
	seteuid(c->uid);
}

static inline void leave_user_context_effective()
{
	if (!single_threaded || getuid())
		return;

	seteuid(getuid());
	setegid(getgid());
}

/* access(2) checks the real uid/gid not the effective one
 * we therefore switch them if run as root and in single
 * threaded mode.
 *
 * The real uid/gid are stored per process which is why we
 * can't change them in multithreaded mode. This would lead
 * to all sorts of race conditions and security issues when
 * multiple users access the file system simultaneously.
 *
 */

static inline void enter_user_context_real()
{
	gid_t *groups;
	size_t ngroups;
	struct fuse_context *c = fuse_get_context();

	if (!single_threaded || geteuid())
		return;
	if ((ngroups = get_groups(c->pid, &groups))) {
		setgroups(ngroups, groups);
		free(groups);
	}
	setregid(c->gid, -1);
	setreuid(c->uid, -1);
}

static inline void leave_user_context_real()
{
	if (!single_threaded || geteuid())
		return;

	setuid(geteuid());
	setgid(getegid());
}

static ssize_t ciopfs_get_orig_name(const char *path, char *value, size_t size)
{
	ssize_t attrlen;
	debug("looking up original file name of %s ", path);
	attrlen = lgetxattr(path, CIOPFS_ATTR_NAME, value, size);
	if (attrlen > 0) {
		value[attrlen] = '\0';
		debug("found %s\n", value);
	} else {
		debug("nothing found\n");
	}
	return attrlen;
}

static int ciopfs_set_orig_name_fd(int fd, const char *origpath)
{
	char *filename = strrchr(origpath, '/');
	if (!filename)
		filename = (char *)origpath;
	else
		filename++;
#ifndef NDEBUG
	char *path = map_path(origpath);
	if (likely(path != NULL)) {
		log_print("storing original name '%s' in '%s'\n", filename, path);
		free(path);
	}
#endif
	if (fsetxattr(fd, CIOPFS_ATTR_NAME, filename, strlen(filename), 0)) {
		int ret = -errno;
		debug("%s\n", strerror(errno));
		return ret;
	}
	return 0;
}

static int ciopfs_set_orig_name_path(const char *path, const char *origpath)
{
	char *filename = strrchr(origpath, '/');
	if (!filename)
		filename = (char *)origpath;
	else
		filename++;
	debug("storing original name '%s' in '%s'\n", filename, path);
	/* XXX: setting an extended attribute on a symlink doesn't seem to work (EPERM) */
	if (lsetxattr(path, CIOPFS_ATTR_NAME, filename, strlen(filename), 0)) {
		int ret = -errno;
		debug("%s\n", strerror(errno));
		return ret;
	}
	return 0;
}

static int ciopfs_remove_orig_name(const char *path)
{
	debug("removing original file name of %s\n", path);
	return lremovexattr(path, CIOPFS_ATTR_NAME);
}

static int ciopfs_getattr(const char *path, struct stat *st_data)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = lstat(p, st_data);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_fgetattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
	int ret = 0;
	enter_user_context_effective();
	int res = fstat(fi->fh, stbuf);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	return ret;
}

static int ciopfs_readlink(const char *path, char *buf, size_t size)
{
	int ret;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = readlink(p, buf, size - 1);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	if (res == -1)
		return ret;
	buf[res] = '\0';
	return 0;
}

static int ciopfs_opendir(const char *path, struct fuse_file_info *fi)
{
	int ret;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	DIR *dp = opendir(p);
	if (dp == NULL)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	if (dp == NULL)
		return ret;
	fi->fh = (uint64_t)(uintptr_t)dp;
	return 0;
}

static int ciopfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi)
{
	int ret = 0;
	DIR *dp = (DIR *)(uintptr_t)fi->fh;
	struct dirent *de;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	size_t pathlen = strlen(p);
	char dnamebuf[PATH_MAX];
	char attrbuf[FILENAME_MAX];

	if (pathlen > PATH_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	if (!dp) {
		ret = -EBADF;
		goto out;
	}

	seekdir(dp, offset);

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		char *dname;
		char *attrlower;

		/* skip any entry which is not all lower case for now */
		if (str_contains_upper(de->d_name))
			continue;

		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;

		if (!strcmp(".", de->d_name) || !strcmp("..", de->d_name))
			dname = de->d_name;
		else {
			/* check whether there is an original name associated with
			 * this path and if so return it instead of the all lower
			 * case one
			 */
			snprintf(dnamebuf, sizeof dnamebuf, "%s/%s", p, de->d_name);
			debug("dnamebuf: %s de->d_name: %s\n", dnamebuf, de->d_name);
			if (ciopfs_get_orig_name(dnamebuf, attrbuf, sizeof attrbuf) > 0) {
				/* we found an original name now check whether it is
				 * still accurate and if not remove it
				 */
				attrlower = str_fold(attrbuf);
				if (attrlower && !strcmp(attrlower, de->d_name))
					dname = attrbuf;
				else {
					dname = de->d_name;
					ciopfs_remove_orig_name(dnamebuf);
				}
				free(attrlower);
			} else
				dname = de->d_name;
		}
		debug("dname: %s\n", dname);
		if (filler(buf, dname, &st, telldir(dp)))
			break;
	}

out:
	free(p);
	return ret;
}

static int ciopfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	if (fi->fh)
		closedir((DIR *)(uintptr_t)fi->fh);
	return 0;
}

static int ciopfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int ret = 0;
	int res;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	/* On Linux this could just be 'mknod(p, mode, rdev)' but this
	   is more portable */
	if (S_ISREG(mode)) {
		res = open(p, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0) {
			ciopfs_set_orig_name_fd(res, path);
			close(res);
		}
	} else if (S_ISFIFO(mode)) {
		res = mkfifo(p, mode);
	} else {
		res = mknod(p, mode, rdev);
	}
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_mkdir(const char *path, mode_t mode)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = mkdir(p, mode);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	if (ret == 0)
		ciopfs_set_orig_name_path(p, path);
	free(p);
	return ret;
}

static int ciopfs_unlink(const char *path)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = unlink(p);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_rmdir(const char *path)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = rmdir(p);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_symlink(const char *from, const char *to)
{
	int ret = 0;
	char *t = map_path(to);
	if (unlikely(t == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = symlink(from, t);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	if (ret == 0)
		ciopfs_set_orig_name_path(t, to);
	free(t);
	return ret;
}

static int ciopfs_rename(const char *from, const char *to)
{
	int ret = 0;
	char *f = map_path(from);
	char *t = map_path(to);
	if (unlikely(f == NULL || t == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = rename(f, t);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	if (ret == 0)
		ciopfs_set_orig_name_path(t, to);
	free(f);
	free(t);
	return ret;
}

static int ciopfs_link(const char *from, const char *to)
{
	int ret = 0;
	char *f = map_path(from);
	char *t = map_path(to);
	if (unlikely(f == NULL || t == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = link(f, t);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	if (ret == 0)
		ciopfs_set_orig_name_path(t, to);
	free(f);
	free(t);
	return ret;
}

static int ciopfs_chmod(const char *path, mode_t mode)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = chmod(p, mode);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_chown(const char *path, uid_t uid, gid_t gid)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = lchown(p, uid, gid);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_truncate(const char *path, off_t size)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = truncate(p, size);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	int ret = 0;
	enter_user_context_effective();
	int res = ftruncate(fi->fh, size);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	return ret;
}

static int ciopfs_utimens(const char *path, const struct timespec ts[2])
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;

	struct timeval tv[2];
	tv[0].tv_sec = ts[0].tv_sec;
	tv[0].tv_usec = ts[0].tv_nsec / 1000;
	tv[1].tv_sec = ts[1].tv_sec;
	tv[1].tv_usec = ts[1].tv_nsec / 1000;

	enter_user_context_effective();
	int res = utimes(p, tv);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int ret;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int fd = open(p, fi->flags, mode);
	if (fd == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	if (fd == -1)
		return ret;
	ciopfs_set_orig_name_fd(fd, path);
	fi->fh = fd;
	return 0;
}

static int ciopfs_open(const char *path, struct fuse_file_info *fi)
{
	int ret;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int fd = open(p, fi->flags);
	if (fd == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	if (fd == -1)
		return ret;
	if (fi->flags & O_CREAT)
		ciopfs_set_orig_name_fd(fd, path);
	fi->fh = fd;
	return 0;
}

static int ciopfs_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
	int res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;
	return res;
}

static int ciopfs_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
	int res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;
	return res;
}

static int ciopfs_statfs(const char *path, struct statvfs *stbuf)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = statvfs(p, stbuf);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_flush(const char *path, struct fuse_file_info *fi)
{
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	int res = close(dup(fi->fh));
	if (res == -1)
		return -errno;

	return 0;
}

static int ciopfs_release(const char *path, struct fuse_file_info *fi)
{
	close(fi->fh);
	return 0;
}

static int ciopfs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
	int res;
#ifdef HAVE_FDATASYNC
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
#endif
		res = fsync(fi->fh);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_access(const char *path, int mode)
{
	int ret = 0;
  	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_real();
  	int res = access(p, mode);
	if (res == -1)
		ret = -errno;
	leave_user_context_real();
	free(p);
	return ret;
}

static int ciopfs_setxattr(const char *path, const char *name, const char *value,
                           size_t size, int flags)
{
	if (!strcmp(name, CIOPFS_ATTR_NAME)) {
		debug("denying setting value of extended attribute '%s'\n", CIOPFS_ATTR_NAME);
		return -EPERM;
	}
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = lsetxattr(p, name, value, size, flags);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = lgetxattr(p, name, value, size);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_listxattr(const char *path, char *list, size_t size)
{
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = llistxattr(p, list, size);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_removexattr(const char *path, const char *name)
{
	if (!strcmp(name, CIOPFS_ATTR_NAME)) {
		debug("denying removal of extended attribute '%s'\n", CIOPFS_ATTR_NAME);
		return -EPERM;
	}
	int ret = 0;
	char *p = map_path(path);
	if (unlikely(p == NULL))
		return -ENOMEM;
	enter_user_context_effective();
	int res = lremovexattr(p, name);
	if (res == -1)
		ret = -errno;
	leave_user_context_effective();
	free(p);
	return ret;
}

static int ciopfs_lock(const char *path, struct fuse_file_info *fi, int cmd,
                       struct flock *lock)
{
	return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
			   sizeof(fi->lock_owner));
}

static void *ciopfs_init(struct fuse_conn_info *conn)
{
	if (chdir(dirname) == -1) {
		log_print("init: %s\n", strerror(errno));
		exit(1);
	}
	return NULL;
}

struct fuse_operations ciopfs_operations = {
	.getattr	= ciopfs_getattr,
	.fgetattr	= ciopfs_fgetattr,
	.readlink	= ciopfs_readlink,
	.opendir	= ciopfs_opendir,
	.readdir	= ciopfs_readdir,
	.releasedir	= ciopfs_releasedir,
	.mknod		= ciopfs_mknod,
	.mkdir		= ciopfs_mkdir,
	.symlink	= ciopfs_symlink,
	.unlink		= ciopfs_unlink,
	.rmdir		= ciopfs_rmdir,
	.rename		= ciopfs_rename,
	.link		= ciopfs_link,
	.chmod		= ciopfs_chmod,
	.chown		= ciopfs_chown,
	.truncate	= ciopfs_truncate,
	.ftruncate	= ciopfs_ftruncate,
	.utimens	= ciopfs_utimens,
	.create		= ciopfs_create,
	.open		= ciopfs_open,
	.read		= ciopfs_read,
	.write		= ciopfs_write,
	.statfs		= ciopfs_statfs,
	.flush		= ciopfs_flush,
	.release	= ciopfs_release,
	.fsync		= ciopfs_fsync,
	.access		= ciopfs_access,
	.setxattr	= ciopfs_setxattr,
	.getxattr	= ciopfs_getxattr,
	.listxattr	= ciopfs_listxattr,
	.removexattr	= ciopfs_removexattr,
	.lock		= ciopfs_lock,
	.init		= ciopfs_init
};

static void usage(const char *name)
{
	fprintf(stderr, "usage: %s directory mountpoint [options]\n"
	                "\n"
			"Mounts the content of directory at mountpoint in case insensitiv fashion.\n"
			"\n"
			"general options:\n"
			"    -o opt,[opt...]        mount options\n"
			"    -h|--help              print help\n"
			"       --version           print version\n"
			"\n", name);

}

enum {
	CIOPFS_OPT_HELP,
	CIOPFS_OPT_VERSION
};

static int ciopfs_opt_parse(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	switch (key) {
		case FUSE_OPT_KEY_NONOPT:
			if (!dirname) {
				/* realpath(char *s, NULL) is a POSIX.1-2008 extension, originally from GLIBC,
				   and might be unavailible on older non-glibc systems. */
				if (!(dirname = realpath(arg, NULL))) {
					perror(outargs->argv[0]);
					exit(1);
				}
				return 0;
			}
			return 1;
		case FUSE_OPT_KEY_OPT:
			if (arg[0] == '-') {
				switch (arg[1]) {
					case 'd':
					case 'f':
						dolog = stderr_print;
				}
			} else if (!strcmp("allow_other", arg)) {
				/* disable multithreaded mode if the file system
				 * is accessible to multiple users simultanousely
				 * because we can't store uid/gid per thread and
				 * this leads to all sorts of race conditions and
				 * security issues.
				 */
				single_threaded = (getuid() == 0);
			}
			return 1;
		case CIOPFS_OPT_HELP:
			usage(outargs->argv[0]);
			fuse_opt_add_arg(outargs, "-ho");
			fuse_main(outargs->argc, outargs->argv, &ciopfs_operations, NULL);
			exit(0);
		case CIOPFS_OPT_VERSION:
			fprintf(stderr, "%s: "VERSION" fuse: %d\n", outargs->argv[0], fuse_version());
			exit(0);
		default:
			fprintf(stderr, "see `%s -h' for usage\n", outargs->argv[0]);
			exit(1);
	}
	return 1;
}

static struct fuse_opt ciopfs_opts[] = {
	FUSE_OPT_KEY("-h",		CIOPFS_OPT_HELP),
	FUSE_OPT_KEY("--help",		CIOPFS_OPT_HELP),
	FUSE_OPT_KEY("--version",	CIOPFS_OPT_VERSION),
	FUSE_OPT_END
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	fuse_opt_parse(&args, &dirname, ciopfs_opts, ciopfs_opt_parse);

	if (single_threaded) {
		fuse_opt_add_arg(&args, "-s");
		log_print("disabling multithreaded mode for root mounted "
		          "filesystem that is accessible for other users "
		          "via the `-o allow_other' option\n");
	}

	umask(0);
	return fuse_main(args.argc, args.argv, &ciopfs_operations, NULL);
}
