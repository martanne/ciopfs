#!/bin/sh

[ -z "$CIOPFS" ] && CIOPFS="ciopfs"

FSTEST=http://tuxera.com/sw/qa/pjd-fstest-20090130-RC.tgz
CIOPFS_ARGS="-f -o direct_io,allow_other,use_ino,noauto_cache,ac_attr_timeout=0,attr_timeout=0,entry_timeout=0"
CIOPFS_XATTR_NAME="user.filename"

die() {
	[ $# -ne 0 ] && echo "$0: $*"
	exit 1
}

# $1 => fs type, $2 => image size, $3 => mkfs options
mkfs_image() {
	local loopdev size
	dd if=/dev/zero of="$1.img" bs=1M count=$2
	loopdev=`losetup -f`
	losetup "$loopdev" "$1.img"
	mkfs.$1 $3 "$loopdev"
	losetup -d "$loopdev"
}

# $1 => dir in which to perform the tests, $2 => fs type, $3 => log file
run_fstest() {
	tar -C "$1" -xzf fstest.tgz
	cd "$1"/pjd-fstest*
	echo fs="$2" >> tests/conf
	[[ $2 == ntfs ]] && echo fs="ntfs-3g" >> tests/conf
	make
	prove -r tests 2>&1 | tee "$3"
	cd -
}

# $1 => fs type, $2 => mount options
test_image() {
	echo mount -t $1 -o "loop,$2" "$1.img" mnt
	mount -t $1 -o "loop,$2" "$1.img" mnt || die "could not mount $1 image"
	mkdir -p "mnt/$1" "mnt/ciopfs-$1"
	run_fstest "mnt/$1" $1 "../../../$1.result"
	"$CIOPFS" $CIOPFS_ARGS "mnt/ciopfs-$1" ciopfs-mnt &> "ciopfs-$1.log" &
	ps -p $! &> /dev/null || die "ciopfs not running, aborting..."
	touch ciopfs-mnt/CiOpFs &&
	getfattr -n $CIOPFS_XATTR_NAME mnt/ciopfs-$1/ciopfs | grep CiOpFs &> /dev/null ||
	die "ciopfs not working correctly"
	run_fstest "ciopfs-mnt" $1 "../../ciopfs-$1.result"
	umount -f ciopfs-mnt || kill -9 $!
	umount -f mnt || die "couldn't umount $1 image"
}

[ $# -eq 0 ] && die "specify a directory in which to run the testsuite in"

[ ! -d "$1" ] && mkdir "$1"

cd "$1" && rm -rf mnt ciopfs-mnt *.img *.result *.log && mkdir -p mnt ciopfs-mnt || die

[ ! -f fstest.tgz ] && wget "$FSTEST" -O fstest.tgz

mkfs_image ext3 20 -F
test_image ext3 user_xattr

mkfs_image ntfs 20
test_image ntfs

mkfs_image xfs 20
test_image xfs

# btrfs needs at least 256M
mkfs_image btrfs 256
test_image btrfs
