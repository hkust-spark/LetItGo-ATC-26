# 解码器依赖

[English](README.md) | 简体中文 | [返回实验说明](../README.zh-CN.md)

本独立离线 motivation 实验需要使用本目录补丁构建的 FFmpeg/AOM，未经修改的 FFmpeg 不能替代。原生实验程序需要包含 `AV_CODEC_ID_VVC` 的 FFmpeg 头文件，以及本目录补丁提供的定制重建路径。

补丁只包含源代码修改，不包含原开发过程的提交历史或提交信息。对应的公开上游版本如下：

| 依赖 | 公开上游 | 基准版本 | 补丁 |
| --- | --- | --- | --- |
| FFmpeg | https://github.com/FFmpeg/FFmpeg | `239f2c733de417201d7ad3b3b8b0d9b63285b2b1` | `ffmpeg-ec.patch` |
| AOM | https://aomedia.googlesource.com/aom | `44121a2955e80dd72acf18f75b95b886afa23da6` | `aom-ec.patch` |

修改保留原有解码实现，包括错误隐藏和实验信息记录代码。运行 SSIM 实验不需要另一套 DC/MV 实验脚本。重建后的源码仍适用各上游项目的许可证和版权声明。

H.264 仅在未启用帧级多线程时写入附加的 `packet_loss.*` metadata，避免视频准备或探测期间，一个解码线程复制图像信息时，另一个线程同时修改其 metadata。原生实验程序使用单个解码线程，因此其 metadata 和恢复行为保持不变。

## 准备条件

需要安装 C/C++ 编译器、make、CMake、Git、pkg-config、NASM，以及 x264、x265、libvpx 和 VVenC（`libvvenc`）开发库。在 Ubuntu 24.04 上先安装系统依赖：

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git ca-certificates pkg-config nasm \
  libx264-dev libx265-dev libvpx-dev
```

然后按下一节安装 VVenC。默认 FFmpeg 构建会启用上述全部编码库，因此即使先只运行 H.264，也需要先完成这些开发库的安装。

SVT-AV1 为可选依赖。不启用时，准备阶段使用打过补丁的 AOM 编码器。启用 SVT-AV1 后，输入准备脚本优先使用 SVT 编码，但实验仍明确优先使用 **libaom AV1 解码器**。

## 安装 VVenC

以下命令从 `motivation` 运行，将官方 [VVenC v1.14.0](https://github.com/fraunhoferhhi/vvenc/releases/tag/v1.14.0) 安装到与后续 AOM/FFmpeg 相同的项目内前缀，不需要给 VVenC 打补丁。此目录由当前用户写入，不需要 `sudo`。如需自定义安装位置，将 `FFMPEG_PREFIX` 设为可写的绝对路径，并在后续构建和运行中保持一致。

```bash
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p dependencies/src
git clone --depth 1 --branch v1.14.0 \
  https://github.com/fraunhoferhhi/vvenc.git dependencies/src/vvenc
git -C dependencies/src/vvenc rev-parse HEAD
```

最后一条命令应输出 `9428ea8636ae7f443ecde89999d16b2dfc421524`。随后构建并安装共享库、开发头文件和 `libvvenc.pc`；FFmpeg 使用 VVenC 编码库，不需要单独构建 `vvencapp`：

```bash
cmake -S dependencies/src/vvenc -B dependencies/build/vvenc \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$FFMPEG_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON \
  -DVVENC_LIBRARY_ONLY=ON
cmake --build dependencies/build/vvenc --parallel 4
cmake --install dependencies/build/vvenc

pkg-config --modversion libvvenc
pkg-config --variable=prefix libvvenc
pkg-config --cflags --libs libvvenc
```

检查版本为 `1.14.0`，安装前缀为所选的 `FFMPEG_PREFIX`，且头文件与链接参数指向该前缀。内存较小时可调低 `--parallel`。VVenC 负责生成 H.266/VVC 输入；实验中的 VVC 解码器来自后续构建的补丁 FFmpeg。

安装结果应包含 `include/vvenc/vvenc.h`、`lib/libvvenc.so` 和 `lib/pkgconfig/libvvenc.pc`。若 pkg-config 指向其他安装位置，先修正 `PKG_CONFIG_PATH` 再构建 FFmpeg。以上选项见上游 [CMake 配置](https://github.com/fraunhoferhhi/vvenc/blob/v1.14.0/CMakeLists.txt)；`VVENC_LIBRARY_ONLY=ON` 只构建库，省略独立应用。

安装完成后继续下面的脚本构建步骤。新开终端时，先回到 `motivation` 并重新导出本节的三个环境变量，再进行构建或运行。

## 脚本构建

以下命令从 `motivation` 运行：

```bash
bash dependencies/prepare_sources.sh
bash dependencies/build_dependencies.sh
```

`prepare_sources.sh` 获取固定版本，应用补丁，并导出不含 `.git` 目录的源码到 `dependencies/src/ffmpeg` 和 `dependencies/src/aom`。已有同名源码目录时它会退出，避免覆盖。已有成功准备的源码时，直接执行构建步骤即可；如需重新准备，应先保留或移走已有源码目录。

`build_dependencies.sh` 先构建 AOM，再构建 FFmpeg，使 FFmpeg 能访问定制 AOM 的错误隐藏控制接口，最后构建原生实验程序。这两个脚本只负责 AOM、FFmpeg 和原生实验程序；运行前须完成系统依赖和上述 VVenC 安装。

构建脚本可从任意目录调用。使用 `--help` 查看帮助，或使用 `--dry-run` 只打印命令；后者不要求已准备源码或安装编译依赖，也不创建目录或写入文件：

```bash
bash dependencies/build_dependencies.sh --dry-run
```

| 环境变量 | 默认值 | 作用 |
| --- | --- | --- |
| `FFMPEG_PREFIX` | 本模块的 `dependencies/install` | AOM/FFmpeg 安装前缀；构建及运行时保持一致；相对值按调用时的当前目录解析 |
| `JOBS` | `nproc` | 构建并发数；内存较小时调低 |
| `ENABLE_SVTAV1` | `0` | 设为 `1` 时为 FFmpeg 启用已安装的 SVT-AV1 开发库 |

例如，自定义安装位置并限制编译并发：

```bash
FFMPEG_PREFIX="$PWD/dependencies/install" JOBS=4 \
  bash dependencies/build_dependencies.sh
```

已安装 SVT-AV1 开发库，并希望优先用它生成 AV1 输入时：

```bash
ENABLE_SVTAV1=1 JOBS=4 bash dependencies/build_dependencies.sh
```

构建脚本对子进程环境的修改不会持续到调用它的 shell。后续通过 `scripts/run_motivation.sh` 运行时，它会为所选前缀设置环境；如果直接运行底层入口，则在当前 shell 中设置：

```bash
export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

## 手动构建

需要逐步检查编译过程时，在准备源码和设置上述环境变量后执行：

```bash
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

若已安装 SVT-AV1 开发库，可在 FFmpeg 的 configure 命令中加入 `--enable-libsvtav1`。实际使用的编码器会记录在生成的编码报告中。这不改变实验对 libaom AV1 解码器的优先选择。

## 检查安装结果

```bash
"$FFMPEG_PREFIX/bin/ffmpeg" -hide_banner -encoders
"$FFMPEG_PREFIX/bin/ffmpeg" -hide_banner -decoders
pkg-config --modversion libavformat libavcodec libavutil libswscale aom libvvenc
```

| 用途 | 必需组件 |
| --- | --- |
| 编码 | `libx264`、`libx265`、`libvpx`、`libvpx-vp9`、`libaom-av1`（编码也可用 `libsvtav1`）、`libvvenc` |
| 解码 | `h264`、`hevc`、`vp8`、`vp9`、`libaom-av1`、`vvc` |

原生程序的 AV1 解码器选择存在按 codec ID 查找的回退路径，因此即使命令能运行，也应确认 `libaom-av1` 解码器实际存在，才能复现配置中的错误隐藏行为。

命令行工具、开发头文件和共享库必须来自同一 `FFMPEG_PREFIX`。底层运行脚本支持 `FFMPEG_BIN` 和 `FFPROBE_BIN` 覆盖。使用尚未安装的 FFmpeg 构建时，可通过 `PACKET_LOSS_FFMPEG_LIBRARY_PATH` 显式指定原生程序所需共享库的目录。切换链接的 FFmpeg 版本后，应清理并重建 `code/native`。

## 定制解码器改变了什么

补丁不仅导出 metadata，也会改变重建图像。原生程序通过 `PACKET_LOSS_CORRUPT_PACKET_POS`、`PACKET_LOSS_CORRUPT_REL_START` 和 `PACKET_LOSS_CORRUPT_REL_END` 向定制解码器提供已知的合成损坏字节范围。

- VP9 可以据此停止熵解码，并对该 tile 剩余的 superblock 进行错误隐藏。
- AOM 提供定制错误隐藏控制，检测长连续零字节，并将字节位置映射到受损 superblock 或后缀区域；同时处理受损的 hidden frame。
- VVC 增加受损 CTU/access unit 的错误隐藏，并允许在可恢复错误后输出图像。

测得的 packet 位置与 importance 的关系反映了上述编码设置和定制解码恢复规则。

## 常见构建问题

| 现象 | 处理 |
| --- | --- |
| `Source directory already exists` | 已有源码时直接构建；要重新准备则先保留或移走已有目录 |
| pkg-config 找不到 `libvvenc` | 按[安装 VVenC](#安装-vvenc)完成安装并重新导出环境变量，检查 `pkg-config --variable=prefix libvvenc` 指向所选前缀 |
| FFmpeg 找不到 AOM 定制接口 | 先完成补丁 AOM 的安装，确认 pkg-config 使用同一前缀，再配置 FFmpeg |
| 缺少 `AV_CODEC_ID_VVC` | 检查是否误用了系统旧 FFmpeg 头文件，确认使用按本文构建的 FFmpeg 安装前缀 |
| 运行时缺共享库或出现符号不匹配 | 核对 `LD_LIBRARY_PATH`、pkg-config 和 `FFMPEG_PREFIX`，清理并重建原生程序 |
| 构建因内存不足被终止 | 降低 `JOBS`，例如 `JOBS=2` |
