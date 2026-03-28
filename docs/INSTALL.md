This document describes how to configure and build erofs-utils from
source.

See the [README](../README) file in the top level directory about
the brief overview of erofs-utils.

## Quick Start

For those who want a quick build, ensure that the following prerequisites are
installed (on Debian/Ubuntu):

``` sh
$ sudo apt-get install autoconf automake libtool pkg-config uuid-dev \
                       liblz4-dev liblzma-dev libfuse-dev zlib1g-dev \
                       libselinux1-dev libzstd-dev
```

Then, run the following commands to build and install:

``` sh
$ ./autogen.sh
$ ./configure
$ make
# make install
```

## Dependencies & build

LZ4 1.9.3+ for LZ4(HC) enabled [^1].

[XZ Utils 5.3.2alpha+](https://tukaani.org/xz/xz-5.3.2alpha.tar.gz) for
LZMA enabled, [XZ Utils 5.4+](https://tukaani.org/xz/xz-5.4.1.tar.gz)
highly recommended.

libfuse 2.6+ for erofsfuse enabled.

[^1]: It's not recommended to use LZ4 versions under 1.9.3 since
unexpected crashes could make trouble to end users due to broken
LZ4_compress_destSize() (fixed in v1.9.2),
[LZ4_compress_HC_destSize()](https://github.com/lz4/lz4/commit/660d21272e4c8a0f49db5fc1e6853f08713dff82) or
[LZ4_decompress_safe_partial()](https://github.com/lz4/lz4/issues/783).

## How to build with LZ4

To build, the following commands can be used in order:

``` sh
$ ./autogen.sh
$ ./configure
$ make
```

`mkfs.erofs`, `dump.erofs` and `fsck.erofs` binaries will be
generated under the corresponding folders.

## How to build with liblzma

In order to enable LZMA support, build with the following commands:

``` sh
$ ./configure --enable-lzma
$ make
```

Additionally, you could specify liblzma target paths with
`--with-liblzma-incdir` and `--with-liblzma-libdir` manually.

## How to build with multithreading

To enable multithreading support for mkfs.erofs, use the following:

``` sh
$ ./configure --enable-multithreading
$ make
```

Note that multithreading is enabled by default if the compiler supports it.
To disable it explicitly, use `--disable-multithreading`.

## How to build erofsfuse

It's disabled by default as an experimental feature for now due
to the extra libfuse dependency, to enable and build it manually:

``` sh
$ ./configure --enable-fuse
$ make
```

`erofsfuse` binary will be generated under `fuse` folder.

## How to install erofs-utils manually

Use the following command to install erofs-utils binaries:

``` sh
# make install
```

By default, `make install` will install all the files in
`/usr/local/bin`, `/usr/local/lib` etc.  You can specify an
installation prefix other than `/usr/local` using `--prefix`,
for instance `--prefix=$HOME`.
