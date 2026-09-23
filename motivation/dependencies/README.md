# Decoder dependencies

[English](README.md) | [简体中文](README.zh-CN.md)

This standalone offline motivation experiment requires FFmpeg/AOM built with
the patches in this directory. Stock FFmpeg is not a substitute. The native
helper needs FFmpeg headers with `AV_CODEC_ID_VVC` and the custom reconstruction
paths in these patches.

The patches contain source changes only, without development commit metadata.
Their public upstream bases are:

| Dependency | Public upstream | Base revision | Patch |
| --- | --- | --- | --- |
| FFmpeg | https://github.com/FFmpeg/FFmpeg | `239f2c733de417201d7ad3b3b8b0d9b63285b2b1` | `ffmpeg-ec.patch` |
| AOM | https://aomedia.googlesource.com/aom | `44121a2955e80dd72acf18f75b95b886afa23da6` | `aom-ec.patch` |

The source changes retain the existing decoder implementation, including its
error-concealment and instrumentation code. SSIM execution does not require the
separate DC/MV experiment scripts. Original upstream licenses and copyright
notices remain applicable to the reconstructed source trees.

H.264 attaches the supplementary `packet_loss.*` metadata only when frame
threading is inactive. This avoids modifying a picture's metadata while another
decoder thread copies it during video preparation or probing. The native
experiment helper uses a single decoder thread, so its metadata and recovery
behavior are preserved.

## Build prerequisites

Install a C/C++ compiler, make, CMake, Git, pkg-config, NASM, and development
libraries for x264, x265, libvpx and VVenC (`libvvenc`). On Ubuntu 24.04, install
the system packages below, then build VVenC using the next section:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git ca-certificates pkg-config nasm \
  libx264-dev libx265-dev libvpx-dev
```

The supplied FFmpeg build enables all these codec libraries, including
`--enable-libvvenc`. They are required even when the first experiment uses
only H.264. Experiments still run one codec at a time.

SVT-AV1 is optional. Without it, input preparation uses the patched AOM
encoder. Enable it only if its development library is installed; the
experiment explicitly prefers the **libaom AV1 decoder** either way.

## Install VVenC

Use the official [VVenC v1.14.0 release](https://github.com/fraunhoferhhi/vvenc/releases/tag/v1.14.0),
commit `9428ea8636ae7f443ecde89999d16b2dfc421524`. VVenC supplies the H.266
encoder; the VVC decoder comes from the patched FFmpeg below. No VVenC patch
is needed.

Run the following commands from `motivation`. Use the same
`FFMPEG_PREFIX` for VVenC, AOM and FFmpeg. The default installs inside this
module, so installation does not require `sudo`. To use another location,
set an absolute, writable prefix before configuring any of the libraries.

```bash
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p dependencies/src
git clone --depth 1 --branch v1.14.0 \
  https://github.com/fraunhoferhhi/vvenc.git dependencies/src/vvenc
git -C dependencies/src/vvenc rev-parse HEAD
```

The last command should print the commit above. Build and install its shared
library, headers and pkg-config metadata:

```bash
cmake -S dependencies/src/vvenc -B dependencies/build/vvenc \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$FFMPEG_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON -DVVENC_LIBRARY_ONLY=ON
cmake --build dependencies/build/vvenc --parallel 4
cmake --install dependencies/build/vvenc

pkg-config --modversion libvvenc
pkg-config --variable=prefix libvvenc
pkg-config --cflags --libs libvvenc
```

Expect version `1.14.0`, the selected prefix, and include/library flags for
that prefix. The installation contains `include/vvenc/vvenc.h`,
`lib/libvvenc.so` and `lib/pkgconfig/libvvenc.pc`. If pkg-config reports another
installation, correct `PKG_CONFIG_PATH` before building FFmpeg.
`VVENC_LIBRARY_ONLY=ON` omits the standalone applications; FFmpeg uses the
library directly. Reduce `--parallel 4` on machines with limited memory.
These are upstream [CMake build options](https://github.com/fraunhoferhhi/vvenc/blob/v1.14.0/CMakeLists.txt).

In each new shell, export the same three environment variables before
building or running the experiment. `PKG_CONFIG_PATH` selects the development
files; `LD_LIBRARY_PATH` selects the installed shared libraries at runtime.

## Prepare and build AOM/FFmpeg

After installing the prerequisites and VVenC, continue from
`motivation` in the same shell:

```bash
bash dependencies/prepare_sources.sh
```

The preparation script exports patched AOM/FFmpeg source trees without `.git`
directories. It refuses to replace an existing source directory. The scripts
do not download or build VVenC; complete the installation above first.

The build script runs the commands below in order and then rebuilds the native
helper. It can be invoked from any directory and preserves externally supplied
pkg-config/library search paths:

```bash
bash dependencies/build_dependencies.sh --dry-run
JOBS=8 bash dependencies/build_dependencies.sh

# Optional alternative AV1 encoder; requires its development library.
ENABLE_SVTAV1=1 JOBS=8 bash dependencies/build_dependencies.sh
```

| Setting | Default | Meaning |
| --- | --- | --- |
| `FFMPEG_PREFIX` | This module's `dependencies/install` | Installation prefix; explicitly relative values use the caller's working directory |
| `JOBS` | `nproc` | Build concurrency, a positive integer |
| `ENABLE_SVTAV1` | `0` | Set `1` to add FFmpeg's `--enable-libsvtav1` |
| `--dry-run` | Off | Print commands; no source/build prerequisites or file writes |

The script does not fetch sources or install system packages. Export
`FFMPEG_PREFIX` in the calling shell if subsequent lower-level commands should
use that prefix; variables set inside the build script do not change the
calling shell's environment.

### Manual equivalent

Build AOM before FFmpeg so that FFmpeg sees its custom concealment control:

```bash
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cmake -S dependencies/src/aom -B dependencies/build/aom \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$FFMPEG_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
cmake --build dependencies/build/aom -j "$(nproc)"
cmake --install dependencies/build/aom

(
  cd dependencies/src/ffmpeg
  ./configure --prefix="$FFMPEG_PREFIX" \
    --enable-shared --disable-static --enable-gpl \
    --enable-libaom --enable-libx264 --enable-libx265 \
    --enable-libvpx --enable-libvvenc \
    --extra-cflags="-I$FFMPEG_PREFIX/include" \
    --extra-ldflags="-L$FFMPEG_PREFIX/lib -Wl,-rpath,$FFMPEG_PREFIX/lib"
  make -j "$(nproc)"
  make install
)

make -C code/native clean
make -C code/native
```

Add `--enable-libsvtav1` to FFmpeg's configure command if that development
library is installed and you want the preparation script's preferred SVT
encoder. Encoder choice is recorded in the generated encode report. It does
not change the experiment's explicit preference for the **libaom AV1 decoder**.

Check the resulting installation:

```bash
"$FFMPEG_PREFIX/bin/ffmpeg" -hide_banner -encoders
"$FFMPEG_PREFIX/bin/ffmpeg" -hide_banner -decoders
pkg-config --modversion libavformat libavcodec libavutil libswscale aom libvvenc
```

Required encoders are `libx264`, `libx265`, `libvpx`, `libvpx-vp9`,
`libaom-av1` (or `libsvtav1` for encoding), and `libvvenc`.
Required decoders are `h264`, `hevc`, `vp8`, `vp9`, `libaom-av1`, and `vvc`.
The native AV1 selection has a codec-ID fallback, so verify that `libaom-av1`
is present when reproducing the configured concealment behavior.

Use the same `FFMPEG_PREFIX` for tools, development headers, and shared
libraries. The runtime scripts honor `FFMPEG_BIN` and `FFPROBE_BIN` overrides.
For an uninstalled FFmpeg build, `PACKET_LOSS_FFMPEG_LIBRARY_PATH` explicitly
selects native helper shared-library directories. Rebuild `code/native` after
changing the linked FFmpeg installation.

If FFmpeg configuration cannot find `libvvenc`, check that
`$FFMPEG_PREFIX/lib/pkgconfig/libvvenc.pc` exists and that
`pkg-config --variable=prefix libvvenc` selects the installation above. If
`libvvenc.so` cannot be loaded at runtime, export `LD_LIBRARY_PATH` as shown
in the VVenC installation section.

## What the custom decoders change

These changes affect reconstructed pixels, in addition to exposing metadata.
The native helper supplies the known synthetic corruption byte range through
`PACKET_LOSS_CORRUPT_PACKET_POS`, `PACKET_LOSS_CORRUPT_REL_START`, and
`PACKET_LOSS_CORRUPT_REL_END`.

- VP9 can use that range to stop entropy decoding and conceal the remaining
  superblocks in the tile.
- AOM supports a custom error-concealment control. Its implementation detects
  long zero runs and maps byte position to a damaged superblock/suffix region;
  it also handles damaged hidden frames.
- VVC adds concealment for damaged CTUs/access units and permits output after
  recoverable errors.

The measured packet position/importance relation reflects these encoder
settings and custom decoder recovery rules.
