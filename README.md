# ciopfs

ciopfs is a stackable or overlay linux userspace file system (implemented with [fuse](http://fuse.sourceforge.org/)) which mounts a normal directory on a regular file system in case insensitive fashion.

The commands below should illustrate it's function.

```
mkdir -p ~/tmp/ciopfs/{.data,case-insensitive}
ciopfs ~/tmp/ciopfs/.data ~/tmp/ciopfs/case-insensitive
cd ~/tmp/ciopfs
mkdir -p case-insensitive/DeMo/SubFolder
echo demo >> case-insensitive/DEMO/subFolder/MyFile
```

At this point your file system should look like this:

case-insensitive
```
`-- DeMo
    `-- SubFolder
        `-- MyFile
.data
`-- demo
    `-- subfolder
        `-- myfile
```
To avoid any conflicts you should not manipulate the data directory directly, any change should be done over the mount point. Any filenames in the data directory which aren't all lower case are ignored.

If you want to mount the file system automatically at boot time add a line like the one below to your /etc/fstab.

```
/data/projects/ciopfs/data	/data/projects/ciopfs/mnt	ciopfs	allow_other,default_permissions,use_ino,attr_timeout=0	0	0
```

Note that ciopfs is primarely designed for single user mode. It was originally developed to mount the wine programm folder and provide faster case insensitive file access. If you want to give multiple users write access to the same file system, then you have to mount it as root. However in order to avoid security problems ciopfs will force fuse into single threaded mode and thus hurt performance.

## How it works

ciopfs works by translating every path element to lower case before further operations take place. On file or directory creation the original file name is stored in an extended attribute which is later returned upon request.

This can be seen below:

```
getfattr -dR .data
# file: .data/demo
user.filename="DeMo"

# file: .data/demo/subfolder
user.filename="SubFolder"

# file: .data/demo/subfolder/myfile
user.filename="MyFile"
```

## Runtime Requirements

If you want the file system to preserve case information you have to make sure that the underlying file system supports extended attributes (for example for ext{2,3} you need a kernel with CONFIG_EXT{2,3}_FS_XATTR enabled). You probably also want to mount the underlying filesystem with the user_xattr option which allows non root users to create extended attributes.

## Build Requirements

In order to compile ciopfs you will need the fuse development files, [`libattr`](http://acl.bestbits.at/download.html#Attr) and if you plan to use unicode characters within file names you will either need [`glib`](http://www.gtk.org/) which is the default or alternatively [`libicu`](http://www.icu-project.org/).

If you want to use neither of those the file system will fall back to libc's `tolower(3)` function which is only defined for `[a-zA-Z]`

which means it will only work case insensitively for ascii file names.

For ease of use the following 3 Makefile targets are supported:

* unicode-glib (default)
* unicode-icu
* ascii

Running one of those followed by sudo make install should do everything that is needed.

## POSIX Compliance

ciopfs passes all test of a slightly patched POSIX file system test suite when mounted as root user with the following options

```
allow_other,use_ino,attr_timeout=0,entry_timeout=0
```

and $fs set to "ciopfs" in the test suite configuration file. This was last tested with pjd-fstest-20090130-RC.tgz and ext3 as the underlying file system.

## Stability and Speed

ciopfs just passes every requested operation to the underlying file system so in theory it shouldn't have a negative impact on stability. However if you find a bug then send me an email with the instruction to reproduce it.

As far as speed is of concern, i didn't really benchmark or optimize it so far. There is the usual overhead associated with user / kernel space context switches. Furthermore ciopfs in it's current implementation uses libc's malloc/free quite extensively, maybe this could be a bottleneck.

## License

ciopfs is licensed under the GNU GPL v2.

http://brain-dump.org/projects/ciopfs/
