#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+

set -eu

MKFS=${MKFS:-./mkfs/mkfs.erofs}
DUMP=${DUMP:-./dump/dump.erofs}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/erofs-hotfile-layout.XXXXXX")
cleanup() {
	rm -rf "$tmpdir"
}
trap cleanup EXIT

extent_start() {
	"$DUMP" -e --path="$1" "$2" |
		awk '
			/^[[:space:]]+[0-9]+:/ {
				line = $0
				sub(/^.*\|[[:space:]]*[0-9]+[[:space:]]*:[[:space:]]*/, "", line)
				sub(/\.\..*$/, "", line)
				gsub(/[[:space:]]/, "", line)
				print line
				exit
			}
		'
}

inode_links() {
	"$DUMP" --path="$1" "$2" |
		awk '
			/Links:/ {
				for (i = 1; i <= NF; ++i) {
					if ($i == "Links:") {
						print $(i + 1)
						exit
					}
				}
			}
		'
}

assert_hot_file_nonrecursive_hotdir() {
	img="$1"

	hot=$(extent_start /a/hot "$img")
	hotdir_cold=$(extent_start /a/cold "$img")
	outside_cold=$(extent_start /c/cold "$img")

	if [ "$hot" -ge "$hotdir_cold" ]; then
		echo "hot file was not placed before cold sibling: hot=$hot cold=$hotdir_cold" >&2
		exit 1
	fi

	if [ "$outside_cold" -ge "$hotdir_cold" ]; then
		echo "hot directory promoted cold child recursively: outside=$outside_cold hotdir_cold=$hotdir_cold" >&2
		exit 1
	fi
}

assert_explicit_hotdir_nonrecursive() {
	img="$1"

	hotdir_cold=$(extent_start /a/cold "$img")
	outside_cold=$(extent_start /c/cold "$img")

	if [ "$outside_cold" -ge "$hotdir_cold" ]; then
		echo "explicit hot directory promoted cold child recursively: outside=$outside_cold hotdir_cold=$hotdir_cold" >&2
		exit 1
	fi
}

assert_symlink_chain_metadata_hot() {
	img="$1"
	log="$2"

	parent_link=$(extent_start /lib64 "$img")
	parent_cold=$(extent_start /aaa-cold "$img")
	symlink=$(extent_start /usr/lib64/ld-linux-x86-64.so.2 "$img")
	cold=$(extent_start /usr/lib64/aaa-cold "$img")
	real=$(extent_start /usr/lib/x86_64-linux-gnu/ld-real "$img")

	if ! grep -q 'loaded 3 hot files' "$log"; then
		echo "hot symlink chain did not preserve parent link, symlink, and target as hot files" >&2
		cat "$log" >&2
		exit 1
	fi

	if [ "$parent_link" -ge "$parent_cold" ]; then
		echo "hot parent symlink metadata was not placed before cold sibling: parent_link=$parent_link parent_cold=$parent_cold" >&2
		exit 1
	fi

	if [ "$symlink" -ge "$cold" ]; then
		echo "hot symlink metadata was not placed before cold sibling: symlink=$symlink cold=$cold" >&2
		exit 1
	fi

	if [ "$real" -ge "$cold" ]; then
		echo "hot symlink target was not placed before cold sibling: real=$real cold=$cold" >&2
		exit 1
	fi
}

assert_hot_metadata_precedes_regular_data() {
	img="$1"

	hotfile=$(extent_start /a/hot "$img")
	hotdir=$(extent_start /z "$img")

	if [ "$hotdir" -ge "$hotfile" ]; then
		echo "hot metadata was not placed before regular hot file data: hotdir=$hotdir hotfile=$hotfile" >&2
		exit 1
	fi
}

assert_late_hotdirs_precede_regular_hotfile() {
	img="$1"

	sysdir=$(extent_start /sys "$img")
	devdir=$(extent_start /dev "$img")
	liblink=$(extent_start /lib "$img")
	lib64link=$(extent_start /lib64 "$img")
	node=$(extent_start /usr/local/bin/node "$img")

	if [ "$liblink" -ge "$node" ]; then
		echo "late hot /lib symlink metadata was placed after node data: lib=$liblink node=$node" >&2
		exit 1
	fi

	if [ "$lib64link" -ge "$node" ]; then
		echo "late hot /lib64 symlink metadata was placed after node data: lib64=$lib64link node=$node" >&2
		exit 1
	fi

	if [ "$sysdir" -ge "$node" ]; then
		echo "late hot /sys metadata was placed after node data: sys=$sysdir node=$node" >&2
		exit 1
	fi

	if [ "$devdir" -ge "$node" ]; then
		echo "late hot /dev metadata was placed after node data: dev=$devdir node=$node" >&2
		exit 1
	fi
}

assert_late_symlink_alias_inherits_target_rank() {
	img="$1"

	alias=$(extent_start /usr/lib/x86_64-linux-gnu/libfoo.so.1 "$img")
	node=$(extent_start /usr/local/bin/node "$img")

	if [ "$alias" -ge "$node" ]; then
		echo "late hot symlink alias was placed after node data: alias=$alias node=$node" >&2
		exit 1
	fi
}

assert_hot_hardlinks_keep_real_link_count() {
	img="$1"

	links=$(inode_links /a/hot "$img")

	if [ "$links" -ne 2 ]; then
		echo "hot hardlink aliases changed link count: links=$links" >&2
		exit 1
	fi
}

assert_root_dirdata_precedes_hot_file() {
	img="$1"

	rootdir=$(extent_start / "$img")
	hotfile=$(extent_start /zz/hot "$img")

	if [ "$rootdir" -ge "$hotfile" ]; then
		echo "root directory data was placed after hot file data: root=$rootdir hotfile=$hotfile" >&2
		exit 1
	fi
}

root="$tmpdir/root"
mkdir -p "$root/a" "$root/c"
printf hot > "$root/a/hot"
dd if=/dev/zero bs=4096 count=16 of="$root/a/cold" status=none
dd if=/dev/zero bs=4096 count=16 of="$root/c/cold" status=none

printf '/a/hot\n' > "$tmpdir/hotlist.file"
"$MKFS" -zlz4hc,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.file" \
	"$tmpdir/img.file.erofs" "$root" >/dev/null
assert_hot_file_nonrecursive_hotdir "$tmpdir/img.file.erofs"

printf '/a/\n' > "$tmpdir/hotlist.dir"
"$MKFS" -zlz4hc,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.dir" \
	"$tmpdir/img.dir.erofs" "$root" >/dev/null
assert_explicit_hotdir_nonrecursive "$tmpdir/img.dir.erofs"

mkdir -p "$root/usr/lib/x86_64-linux-gnu" "$root/usr/lib64"
printf real > "$root/usr/lib/x86_64-linux-gnu/ld-real"
dd if=/dev/zero bs=4096 count=16 of="$root/aaa-cold" status=none
dd if=/dev/zero bs=4096 count=16 of="$root/usr/lib64/aaa-cold" status=none
ln -s usr/lib64 "$root/lib64"
ln -s /lib/x86_64-linux-gnu/ld-real "$root/usr/lib64/ld-linux-x86-64.so.2"

printf '/lib64/ld-linux-x86-64.so.2\n' > "$tmpdir/hotlist.symlink"
"$MKFS" -d9 -zlz4hc,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.symlink" \
	"$tmpdir/img.symlink.erofs" "$root" >"$tmpdir/mkfs.symlink.log" 2>&1
assert_symlink_chain_metadata_hot "$tmpdir/img.symlink.erofs" "$tmpdir/mkfs.symlink.log"

root_meta="$tmpdir/root-meta-first"
mkdir -p "$root_meta/a" "$root_meta/z"
dd if=/dev/urandom bs=4096 count=256 of="$root_meta/a/hot" status=none
printf '/a/hot\n/z/\n' > "$tmpdir/hotlist.meta-first"
"$MKFS" -d9 -zzstd,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.meta-first" \
	"$tmpdir/img.meta-first.erofs" "$root_meta" >"$tmpdir/mkfs.meta-first.log" 2>&1
assert_hot_metadata_precedes_regular_data "$tmpdir/img.meta-first.erofs"

root_usrmerge="$tmpdir/root-usrmerge"
mkdir -p \
	"$root_usrmerge/dev" \
	"$root_usrmerge/etc/ssl" \
	"$root_usrmerge/proc" \
	"$root_usrmerge/sys" \
	"$root_usrmerge/usr/bin" \
	"$root_usrmerge/usr/lib/x86_64-linux-gnu" \
	"$root_usrmerge/usr/lib64" \
	"$root_usrmerge/usr/local/bin" \
	"$root_usrmerge/usr/local/sbin"
ln -s usr/bin "$root_usrmerge/bin"
ln -s usr/lib "$root_usrmerge/lib"
ln -s usr/lib64 "$root_usrmerge/lib64"
ln -s dash "$root_usrmerge/usr/bin/sh"
ln -s /lib/x86_64-linux-gnu/ld-real "$root_usrmerge/usr/lib64/ld-linux-x86-64.so.2"
for file in \
	etc/passwd \
	etc/ld.so.cache \
	etc/ssl/openssl.cnf \
	usr/bin/dash \
	usr/lib/x86_64-linux-gnu/ld-real \
	usr/lib/x86_64-linux-gnu/libc.so.6 \
	usr/lib/x86_64-linux-gnu/libdl.so.2 \
	usr/lib/x86_64-linux-gnu/libstdc++.so.6 \
	usr/lib/x86_64-linux-gnu/libm.so.6 \
	usr/lib/x86_64-linux-gnu/libgcc_s.so.1 \
	usr/lib/x86_64-linux-gnu/libpthread.so.0 \
	usr/local/bin/docker-entrypoint.sh \
	usr/local/bin/node \
	usr/local/sbin/docker-entrypoint.sh; do
	printf x > "$root_usrmerge/$file"
done
cat > "$tmpdir/hotlist.usrmerge" <<'EOF'
/etc/passwd
/proc/
/usr/local/sbin/docker-entrypoint.sh
/usr/local/bin/docker-entrypoint.sh
/bin/sh
/lib64/ld-linux-x86-64.so.2
/etc/ld.so.preload
/etc/ld.so.cache
/lib/x86_64-linux-gnu/libc.so.6
/usr/local/sbin/node
/usr/local/bin/node
/lib/x86_64-linux-gnu/libdl.so.2
/lib/x86_64-linux-gnu/libstdc++.so.6
/lib/x86_64-linux-gnu/libm.so.6
/lib/x86_64-linux-gnu/libgcc_s.so.1
/lib/x86_64-linux-gnu/libpthread.so.0
/etc/ssl/openssl.cnf
/sys/
/dev/
EOF
"$MKFS" -d9 -zzstd,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.usrmerge" \
	"$tmpdir/img.usrmerge.erofs" "$root_usrmerge" >"$tmpdir/mkfs.usrmerge.log" 2>&1
assert_late_hotdirs_precede_regular_hotfile "$tmpdir/img.usrmerge.erofs"

root_alias="$tmpdir/root-alias-rank"
mkdir -p "$root_alias/usr/lib/x86_64-linux-gnu" "$root_alias/usr/local/bin"
dd if=/dev/zero bs=4096 count=64 of="$root_alias/usr/local/bin/node" status=none
dd if=/dev/zero bs=4096 count=64 of="$root_alias/usr/lib/x86_64-linux-gnu/libfoo.so.1.2.3" status=none
ln -s libfoo.so.1.2.3 "$root_alias/usr/lib/x86_64-linux-gnu/libfoo.so.1"
cat > "$tmpdir/hotlist.alias-rank" <<'EOF'
/usr/lib/x86_64-linux-gnu/libfoo.so.1.2.3
/usr/local/bin/node
/usr/lib/x86_64-linux-gnu/libfoo.so.1
EOF
"$MKFS" -d9 -zzstd,level=9 --workers=1 \
	--hot-file-list="$tmpdir/hotlist.alias-rank" \
	"$tmpdir/img.alias-rank.erofs" "$root_alias" >"$tmpdir/mkfs.alias-rank.log" 2>&1
assert_late_symlink_alias_inherits_target_rank "$tmpdir/img.alias-rank.erofs"

root_hardlink="$tmpdir/root-hardlink"
mkdir -p "$root_hardlink/a" "$root_hardlink/b"
printf data > "$root_hardlink/a/hot"
ln "$root_hardlink/a/hot" "$root_hardlink/b/alias"
cat > "$tmpdir/hotlist.hardlink" <<'EOF'
/a/hot
/b/alias
EOF
"$MKFS" -d9 --hot-file-list="$tmpdir/hotlist.hardlink" \
	"$tmpdir/img.hardlink.erofs" "$root_hardlink" >"$tmpdir/mkfs.hardlink.log" 2>&1
assert_hot_hardlinks_keep_real_link_count "$tmpdir/img.hardlink.erofs"

root_wide="$tmpdir/root-wide"
mkdir -p "$root_wide/zz"
printf hot > "$root_wide/zz/hot"
i=0
while [ "$i" -lt 420 ]; do
	dir=$(printf 'cold%04d' "$i")
	mkdir -p "$root_wide/$dir"
	printf x > "$root_wide/$dir/file"
	i=$((i + 1))
done
printf '/zz/hot\n' > "$tmpdir/hotlist.wide-root"
"$MKFS" -d9 --hot-file-list="$tmpdir/hotlist.wide-root" \
	"$tmpdir/img.wide-root.erofs" "$root_wide" >"$tmpdir/mkfs.wide-root.log" 2>&1
assert_root_dirdata_precedes_hot_file "$tmpdir/img.wide-root.erofs"
