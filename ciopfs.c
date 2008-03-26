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
 * Requirements:
 * 	In order to compile ciopfs, you will need both
 * 	libfuse and libattr. Furthermore if you want a case
 * 	preserving filesystem you have to make sure that the
 * 	underlaying filesystem supports extended attributes
 * 	(for example for ext{2,3} you need a kernel with
 * 	CONFIG_EXT{2,3}_FS_XATTR enabled. You probably also
 * 	want to mount the underlaying filesystem with the
 * 	user_xattr option which allows non root users to create
 * 	extended attributes.
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
#include <assert.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <syslog.h>

#define logp(format, args...) (*dolog)(format, ## args)

#ifdef NDEBUG
# define debug(format, args...)
#else
# define debug logp
#endif

#define CIOPFS_ATTR_NAME "user.filename"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef FILENAME_MAX
#define FILENAME_MAX 4096
#endif


static const char *dirname;

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

static inline char *strtolower(char *s)
{
	char *t = s;
	while(*t) {
		*t = tolower(*t);
		t++;
	}
	return s;
}

static char* map_path(const char *path)
{
	// XXX: malloc failure, memory fragmentation?
	if (path[0] == '/') {
		if (path[1] == '\0')
			return strdup(".");
		path++;
	}
	char *p = strdup(path);
	strtolower(p);
	debug("%s => %s\n", path, p);
	return p;
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
	//XXX: map_path memory leak
	debug("storing original name '%s' in '%s'\n", filename, map_path(origpath));
	if (fsetxattr(fd, CIOPFS_ATTR_NAME, filename, strlen(filename), 0)) {
		debug("%s\n", strerror(errno));
		return -errno;
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
	// XXX: setting an extended attribute on a symlink doesn't seem to work (EPERM)
	if (lsetxattr(path, CIOPFS_ATTR_NAME, filename, strlen(filename), 0)) {
		debug("%s\n", strerror(errno));
		return -errno;
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
	char *p = map_path(path);
	int res = lstat(p, st_data);
	free(p);
	return (res == -1) ? -errno : 0;
}

static int ciopfs_fgetattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
	int res = fstat(fi->fh, stbuf);
	if (res == -1)
		return -errno;
	return 0;
}


static int ciopfs_readlink(const char *path, char *buf, size_t size)
{
	char *p = map_path(path);
	int res = readlink(p, buf, size - 1);
	free(p);
	if (res == -1)
		return -errno;
	buf[res] = '\0';
	return 0;
}

static int ciopfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi)
{
	int ret = 0;
	DIR *dp;
	struct dirent *de;
	char *p = map_path(path);
	size_t pathlen = strlen(p);
	char dnamebuf[PATH_MAX];
	char attrbuf[FILENAME_MAX];

	if (pathlen > PATH_MAX) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	strcpy(dnamebuf, p);

	(void) offset;
	(void) fi;

	dp = opendir(p);
	if (dp == NULL) {
		ret = -errno;
		goto out;
	}

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		char *dname;
		char *attrlower;

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
				attrlower = strtolower(strdup(attrbuf));
				if(!strcmp(attrlower, de->d_name))
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
		if (filler(buf, dname, &st, 0))
			break;
	}

	closedir(dp);
out:
	free(p);
	return ret;
}

static int ciopfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;
	char *p = map_path(path);
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
	} else
		res = mknod(p, mode, rdev);
	free(p);
	if (res == -1)
		return -errno;

	return 0;
}

static int ciopfs_mkdir(const char *path, mode_t mode)
{
	int ret = 0;
	char *p = map_path(path);
	int res = mkdir(p, mode);

	if (res == -1) {
		ret =  -errno;
		goto out;
	}

	ciopfs_set_orig_name_path(p, path);
out:
	free(p);
	return ret;
}

static int ciopfs_unlink(const char *path)
{
	char *p = map_path(path);
	int res = unlink(p);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_rmdir(const char *path)
{
	char *p = map_path(path);
	int res = rmdir(p);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_symlink(const char *from, const char *to)
{
	int ret = 0;
	char *f = map_path(from);
	char *t = map_path(to);
	int res = symlink(f, t);
	debug("symlink from %s to %s\n", from, to);
	if (res == -1) {
		ret = -errno;
		goto out;
	}
	ciopfs_set_orig_name_path(t, to);
out:
	free(f);
	free(t);
	return ret;
}

static int ciopfs_rename(const char *from, const char *to)
{
	int ret = 0;
	char *f = map_path(from);
	char *t = map_path(to);
	int res = rename(f, t);
	if (res == -1) {
		ret = -errno;
		goto out;
	}
	ciopfs_set_orig_name_path(t, to);
out:
	free(f);
	free(t);
	return ret;
}

static int ciopfs_link(const char *from, const char *to)
{
	int ret = 0;
	char *f = map_path(from);
	char *t = map_path(to);
	int res = link(from, to);
	if (res == -1) {
		ret = -errno;
		goto out;
	}
	ciopfs_set_orig_name_path(t, to);
out:
	free(f);
	free(t);
	return ret;
}

static int ciopfs_chmod(const char *path, mode_t mode)
{
	char *p = map_path(path);
	int res = chmod(p, mode);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_chown(const char *path, uid_t uid, gid_t gid)
{
	char *p = map_path(path);
	int res = lchown(p, uid, gid);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_truncate(const char *path, off_t size)
{
	char *p = map_path(path);
	int res = truncate(p, size);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	int res = ftruncate(fi->fh, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int ciopfs_utime(const char *path, struct utimbuf *buf)
{
	char *p = map_path(path);
	int res = utime(p, buf);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char *p = map_path(path);
	int fd = open(p, fi->flags, mode);
	free(p);
	if(fd == -1)
		return -errno;
	ciopfs_set_orig_name_fd(fd, path);
	fi->fh = fd;
	return 0;
}

static int ciopfs_open(const char *path, struct fuse_file_info *fi)
{
	char *p = map_path(path);
	int fd = open(p, fi->flags);
	free(p);
	if (fd == -1)
		return -errno;
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
	char *p = map_path(path);
	int res = statvfs(p, stbuf);
	free(p);
	if (res == -1)
		return -errno;

	return 0;
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
  	char *p = map_path(path);
  	int res = access(p, mode);
	free(p);
  	if (res == -1)
    		return -errno;
  	return 0;
}

static int ciopfs_setxattr(const char *path, const char *name, const char *value,
                           size_t size, int flags)
{
	if (!strcmp(name, CIOPFS_ATTR_NAME)) {
		debug("denying setting value of extended attribute '%s'\n", CIOPFS_ATTR_NAME);
		return -EPERM;
	}
	char *p = map_path(path);
	int res = lsetxattr(p, name, value, size, flags);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
	char *p = map_path(path);
	int res = lgetxattr(p, name, value, size);
	free(p);
	if (res == -1)
		return -errno;
	return res;
}

static int ciopfs_listxattr(const char *path, char *list, size_t size)
{
	char *p = map_path(path);
	int res = llistxattr(p, list, size);
	free(p);
	if (res == -1)
		return -errno;
	return res;
}

static int ciopfs_removexattr(const char *path, const char *name)
{
	if (!strcmp(name, CIOPFS_ATTR_NAME)) {
		debug("denying removal of extended attribute '%s'\n", CIOPFS_ATTR_NAME);
		return -EPERM;
	}
	char *p = map_path(path);
	int res = lremovexattr(p, name);
	free(p);
	if (res == -1)
		return -errno;
	return 0;
}

static int ciopfs_lock(const char *path, struct fuse_file_info *fi, int cmd,
                       struct flock *lock)
{
	return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
			   sizeof(fi->lock_owner));
}


struct fuse_operations ciopfs_operations = {
	.getattr	= ciopfs_getattr,
	.fgetattr	= ciopfs_fgetattr,
	.readlink	= ciopfs_readlink,
	.readdir	= ciopfs_readdir,
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
	.utime		= ciopfs_utime,
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
	.lock		= ciopfs_lock
/*
 *	what about:
 *
 *	opendir
 *	releasedir
 */
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
	switch(key) {
		case FUSE_OPT_KEY_NONOPT:
			if (!dirname) {
				// XXX: realpath(char *s, NULL) is a glibc extension
				if (!(dirname = realpath(arg, NULL)) || chdir(arg)) {
					perror(outargs->argv[0]);
					exit(1);
				}
				return 0;
			}
			return 1;
		case FUSE_OPT_KEY_OPT:
			if (arg[0] == '-') {
				switch(arg[1]) {
					case 'd':
					case 'f':
						dolog = stderr_print;
				}
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
	if (fuse_opt_parse(&args, &dirname, ciopfs_opts, ciopfs_opt_parse)) {
		fprintf(stderr, "Invalid arguments, see `%s -h' for usage\n", argv[0]);
		exit(1);
	}
	debug("dir: %s\n", dirname);
	fuse_main(args.argc, args.argv, &ciopfs_operations, NULL);
	return 0;
}
