# Motivation：六种 codec 中 packet importance 与 position 的关系

[English](README.md) | 简体中文

本模块提供离线实验，考察**编码帧内 packet 的位置与单个 packet 损坏造成的画质损失之间的关系**。覆盖 **H.264、H.265/HEVC、VP8、VP9、AV1、H.266/VVC** 六种 codec。

模块输出逐次试验结果和数值汇总，不包含绘图脚本，也不自动生成论文图。它作为独立的离线实验运行，不需要网络 trace。

## 实验视频：YouTube-UGC

Motivation 实验使用 **YouTube-UGC** 数据集中的视频，覆盖其 **15 个类别**，包含 HDR 和 VR。
数据集及类别定义见[数据集论文](https://arxiv.org/abs/1904.06457)。

| 类别 | 内容说明 | 文件名前缀 |
| --- | --- | --- |
| Animation | 动画 | `Animation` |
| Cover Song | 歌曲翻唱 | `CoverSong` |
| Gaming | 游戏 | `Gaming` |
| HDR | 高动态范围视频 | `HDR` |
| How To | 操作教程 | `HowTo` |
| Lecture | 讲座 | `Lecture` |
| Live Music | 现场音乐 | `LiveMusic` |
| Lyric Video | 歌词视频 | `LyricVideo` |
| Music Video | 音乐视频 | `MusicVideo` |
| News Clip | 新闻片段 | `NewsClip` |
| Sports | 体育 | `Sports` |
| Television Clip | 电视节目片段 | `TelevisionClip` |
| Vertical Video | 竖屏视频 | `VerticalVideo` |
| Vlog | 视频日志 | `Vlog` |
| VR | 虚拟现实视频 | `VR` |

从[数据集官网](https://media.withyoutube.com/)选择并下载视频；类别标签和文件名也可在官方
[视频索引](https://storage.googleapis.com/ugc-dataset/website/ugc_dataset.json)中查询。
官网提供 raw 与 H.264 两种版本，其中[原始 H.264 下载目录](https://console.cloud.google.com/storage/browser/ugc-dataset/original_videos_h264)
提供可用于下方准备命令的 MP4 输入。记录实际使用的下载版本；网页预览文件与原始下载文件不同。

每个源视频先准备一个 H.264 基准输入，再从同一基准生成其余五种 codec。
基准文件名应保留源视频标识，例如 `<source_stem>_h264.mp4`，下文用 `$BASELINE` 指向该文件，
避免不同视频的结果混在一起。对每个视频仍按 codec 逐一准备、运行和检查。

## 目录与入口

| 路径 | 用途 |
| --- | --- |
| `scripts/run_motivation.sh` | 可选批量工具：准备输入、运行六种 codec、生成数值汇总 |
| `dependencies/prepare_sources.sh` | 获取固定上游版本并应用定制解码补丁 |
| `dependencies/build_dependencies.sh` | 构建并安装 AOM、FFmpeg 和原生实验程序 |
| `code/ssim_loss_experiment/remove_b_frames.sh` | 按需准备不含 B 帧的 H.264 基准输入 |
| `code/ssim_loss_experiment/prepare_matched_codecs.sh` | 从 H.264 输入生成指定 codec 的视频 |
| `run_six_codecs.sh` | 可选批量工具：检查六个已准备好的输入并依次启动实验 |
| `code/ssim_loss_experiment/run_experiment.py` | 推荐入口：对一个 codec 输入运行单帧或连续帧范围实验 |
| `code/packet_loss/` | 视频探测、调度、SSIM 计算与原生程序调用 |
| `code/native/` | 基于 C/libavcodec 的解码与独立 packet 损坏试验 |
| `summarize_results.py` | 跨帧、跨 codec 的数值 CSV 汇总 |

## 按 codec 逐一运行

推荐每次只准备和运行一种 codec，检查它的日志和数值结果后，再手动运行下一种。

### 1. 准备环境与 H.264 输入

请自行安装并初始化 Conda，然后创建和激活实验所用的 Python 环境。已验证的 Python 版本为 3.12.6；`requirements.txt` 固定 Python 直接依赖为 `numpy==2.1.1`、`Pillow==10.4.0` 和 `scikit-image==0.24.0`。编译工具及 x264、x265、libvpx、VVenC 开发库仍需根据[依赖说明](dependencies/README.zh-CN.md)安装。

执行下方 AOM/FFmpeg 构建命令前，先完成[系统依赖](dependencies/README.zh-CN.md#准备条件)和[VVenC 安装](dependencies/README.zh-CN.md#安装-vvenc)。即使先只运行 H.264，当前构建也需要这些依赖。

```bash
# 创建环境只需执行一次。
conda create -n letitgo-motivation python=3.12 pip
conda activate letitgo-motivation
export PYTHON_BIN=python3

cd motivation
python3 -m pip install -r requirements.txt

# 可选：检查 Python 依赖能否正常导入。
python3 -c "import numpy; from PIL import Image; from skimage.metrics import structural_similarity"

export FFMPEG_PREFIX="$PWD/dependencies/install"
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$FFMPEG_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib:$FFMPEG_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 完成 VVenC 安装后，再获取并构建补丁 AOM/FFmpeg。
bash dependencies/prepare_sources.sh
bash dependencies/build_dependencies.sh
```

每次打开新终端，都先激活 Conda 环境，进入本模块目录，并重新设置上述环境变量。脚本通过当前 `PATH` 中的 `python3` 使用已激活的环境；环境创建和依赖安装由使用者管理。`requirements.txt` 只包含 Python 依赖，FFmpeg、AOM、原生 helper 及系统开发库仍按依赖说明准备。如使用其他安装位置，构建和运行时均应设置同一个 `FFMPEG_PREFIX`。

#### 视频放在哪里

以下路径均相对于 `motivation/`，准备和运行命令也从该目录执行。建议按下面的布局存放：

```text
motivation/
├── data/
│   ├── original/                     # 下载的视频，保留原文件名和内容
│   │   └── <source_stem>.mp4
│   ├── h264/                         # 不含 B 帧的 H.264 基准
│   │   └── <source_stem>_h264.mp4
│   └── generated_codecs/
│       └── <baseline_stem>/           # 按 codec 逐一生成视频和编码报告
└── result/
    ├── logs/                         # 准备与实验日志
    ├── smoke/                        # 单帧检查结果
    └── ssim_loss/                    # 帧范围实验及 numeric_summary/
```

从上方官方 H.264 下载目录取得所选 MP4，放进 `data/original/`，保留下载文件名。
这些输入文件由使用者提供，脚本不会自动下载。`data/` 与 `result/` 均已被 Git 忽略；
准备脚本会自动创建生成视频目录，实验和汇总程序会自动创建相应结果目录。
视频也可保存在仓库外，将下方 `SOURCE_VIDEO` 设为其绝对路径即可，无需复制一份。

#### 生成 H.264 基准输入

完成依赖安装后，创建本地目录，并选择**一个**视频。把 `<downloaded_filename>`
替换成实际下载文件名（不含最后的 `.mp4`）；不要将占位符原样运行：

```bash
mkdir -p data/original data/h264 result/logs

SOURCE_VIDEO="data/original/<downloaded_filename>.mp4"
SOURCE_STEM="$(basename "${SOURCE_VIDEO%.*}")"
BASELINE="data/h264/${SOURCE_STEM}_h264.mp4"
BASELINE_STEM="$(basename "${BASELINE%.*}")"
GENERATED_DIR="data/generated_codecs/$BASELINE_STEM"
```

在同一终端执行转换，保留原始下载文件：

```bash
set -o pipefail
bash code/ssim_loss_experiment/remove_b_frames.sh \
  --input "$SOURCE_VIDEO" --output "$BASELINE" \
  2>&1 | tee "result/logs/${BASELINE_STEM}_prepare.log"
```

脚本重新编码第一条视频流，默认使用 libx264、`medium` preset、CRF 11、
8-bit `yuv420p`，关闭 B 帧并移除音频、字幕和数据流。原 B 帧对应的显示画面仍保留；
脚本不设置缩放、强制帧率转换或时间裁剪。HDR 类输入同样会转换为上述 8-bit 基准格式。

检查日志末尾的 `Output frame types`，应看到 `B=0`，并核对总帧数。
还可以查看基准文件的编码、尺寸和帧率：

```bash
"${FFPROBE_BIN:-$FFMPEG_PREFIX/bin/ffprobe}" -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,pix_fmt,avg_frame_rate,has_b_frames,nb_frames \
  -of default=noprint_wrappers=1 "$BASELINE"
```

应看到 `codec_name=h264`、`pix_fmt=yuv420p` 和 `has_b_frames=0`，并确认尺寸、帧率、帧数符合预期。
若输出文件已存在，转换脚本会直接跳过，不重新检查；可以复用已验证的基准文件，
需要重新生成时使用新路径，或明确加 `--force` 覆盖。

如果已有合适的不含 B 帧的 H.264 基准，可以跳过转换：将 `BASELINE` 指向该文件，
然后重新计算 `BASELINE_STEM` 和 `GENERATED_DIR`。打开新终端或更换源视频时，
重新设置上面的变量，并恢复前述 Conda 和原生依赖环境变量。
后续六种 codec 都使用同一基准视频；需要裁剪时，应先准备统一片段的 H.264 基准。

### 2. 先运行并检查 H.264

先运行单帧，确认本机依赖和输入可用。单帧检查使用独立输出目录：

```bash
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "$BASELINE" \
  --frame-index 0 --jobs 1 --output-dir result/smoke
```

若第 0 帧没有可损坏的 packet，它会被跳过；此时选择另一个帧索引检查。确认正常后，运行 H.264 的第 0–599 帧（包括两端），并保存日志：

```bash
set -o pipefail
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "$BASELINE" \
  --start-frame-index 0 --stop-frame-index 599 \
  --frame-jobs 4 --jobs 6 --output-dir result/ssim_loss \
  2>&1 | tee "result/logs/${BASELINE_STEM}_run.log"

python3 summarize_results.py --result-root result/ssim_loss
```

检查 `result/ssim_loss/numeric_summary/codecs.csv` 中当前 codec 的 `frames_completed` 和 `packet_trials`，并对照日志中的跳过信息。完整参考帧解码失败需要排查；没有可损坏 packet 属于正常跳过。尚未运行的 codec 显示零计数和空指标是正常的，不需要为了汇总先运行全部六种。完全没有逐帧结果时，汇总会报错。

### 3. 再准备和运行下一种 codec

例如，H.264 检查完成后，只生成 H.265 输入：

```bash
bash code/ssim_loss_experiment/prepare_matched_codecs.sh \
  --input "$BASELINE" --codec h265
```

准备命令未指定 `--output-dir` 时，会自动写入上面计算的 `$GENERATED_DIR`，
生成 `${BASELINE_STEM}_h265.mp4` 和 `${BASELINE_STEM}_h265_encode_report.txt`。
先查看编码报告中的 codec、尺寸、帧率和实际码率，再运行该输入：

```bash
set -o pipefail
python3 code/ssim_loss_experiment/run_experiment.py \
  --input "${GENERATED_DIR}/${BASELINE_STEM}_h265.mp4" \
  --start-frame-index 0 --stop-frame-index 599 \
  --frame-jobs 4 --jobs 6 --output-dir result/ssim_loss \
  2>&1 | tee "result/logs/${BASELINE_STEM}_h265_run.log"

python3 summarize_results.py --result-root result/ssim_loss
```

每种新 codec 也可以先按上方单帧示例检查：替换输入路径，保留 `--frame-index 0 --jobs 1 --output-dir result/smoke`，确认可用后再运行完整帧范围。

检查当前 codec 的编码报告、运行日志和汇总结果后，再选择下一种。下表列出以 `$BASELINE` 为基准时，每种 codec 对应的准备参数与实验输入。每次手动选择一行，替换上述命令中的 `--codec`、实验 `--input` 和日志文件名；H.264 直接使用基准输入。准备下一种 codec 时继续使用同一个 `$BASELINE`，不要改用前一种 codec 的生成视频。

| Codec | 准备参数 | 实验输入 |
| --- | --- | --- |
| H.264 | 无需转换 | `$BASELINE` |
| H.265 | `--codec h265` | `${GENERATED_DIR}/${BASELINE_STEM}_h265.mp4` |
| VP8 | `--codec vp8` | `${GENERATED_DIR}/${BASELINE_STEM}_vp8.webm` |
| VP9 | `--codec vp9` | `${GENERATED_DIR}/${BASELINE_STEM}_vp9.webm` |
| AV1 | `--codec av1` | `${GENERATED_DIR}/${BASELINE_STEM}_av1.mkv` |
| H.266 | `--codec h266` | `${GENERATED_DIR}/${BASELINE_STEM}_h266.mp4` |

已有目标 codec 视频时，准备脚本会拒绝覆盖；可直接复用该输入，或明确加 `--force` 重新生成。

各 codec 可以使用同一结果根目录，程序按输入文件主名分别存储结果；每次汇总都会包含该根目录内已完成的 codec。不同源视频应使用不同输入主名；不同 packet 大小或编码配置应使用不同 `--output-dir`，避免混合设置。短视频的帧范围会截断到可用范围。指定 `--codec` 时，准备脚本将报告分别保存为 `<input_stem>_<codec>_encode_report.txt`，便于逐一检查与保留。

**本流程不支持中断续跑。** 若某个 codec 的实验被打断，请对该 codec 从头重新运行：使用新的 `--output-dir`，或在 Python 实验命令中加 `--overwrite`，重新执行整个请求帧范围。不要将默认跳过已有帧目录视为可靠的恢复机制。

原生程序会自动构建。切换 FFmpeg 安装后，先执行 `make -C code/native clean`，确保下一次构建链接到所选版本。

## 可选的六 codec 批量工具

完成逐 codec 检查后，如需批量运行，可使用 `scripts/run_motivation.sh`。它的 `all` 阶段依次生成五种编码版本、运行全部六种 codec、汇总 CSV，默认不覆盖已有编码文件或帧输出。`--force` 同时允许编码覆盖和同名帧实验输出覆盖。

```bash
# 只打印批量准备、运行和汇总命令，不执行实验。
bash scripts/run_motivation.sh --input "$BASELINE" --dry-run

# 仅生成五种编码版本；H.264 基准输入保持原样。
bash scripts/run_motivation.sh --input "$BASELINE" --stage prepare

# 使用已有六个输入运行实验。
bash scripts/run_motivation.sh --input "$BASELINE" --stage run

# 仅汇总已有输出，不需要 --input，也不重新解码。
bash scripts/run_motivation.sh --stage summarize
```

`--stage run` 不自动汇总；之后运行 `--stage summarize` 即可。汇总会重新生成数值 CSV，不改动已有逐帧结果。

底层批量入口 `run_six_codecs.sh` 直接调用 Python 实验程序，同样使用已激活的 Conda 环境和 `PYTHON_BIN=python3`。其环境变量默认 `OVERWRITE=1`；要保留已有输出，应设置 `OVERWRITE=0`。它的 `DRY_RUN=1` 会先执行 ffprobe 检查六个输入，再打印实验命令，与统一入口只打印命令的 `--dry-run` 不同。

### 批量入口参数

显式传入的相对路径以**调用命令时的当前目录**为基准。下表所列默认目录位于 `motivation` 内。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--input PATH` | 无 | H.264 基准输入；除 `summarize` 外均必需 |
| `--stage NAME` | `all` | `all`、`prepare`、`run`、`summarize` |
| `--generated-dir DIR` | `data/generated_codecs/<input_stem>` | 五种编码版本的目录 |
| `--output-dir DIR` | `result/ssim_loss` | 逐帧实验输出根目录；汇总写入其 `numeric_summary` 子目录 |
| `--start-frame N` | `0` | 开始帧索引，包括该帧 |
| `--stop-frame N` | `599` | 结束帧索引，包括该帧 |
| `--frame-jobs N` | `4` | 并行处理的帧数 |
| `--packet-jobs N` | `6` | 每帧 packet 试验的并发数 |
| `--packet-size N` | `1500` | 合成 packet 的字节数 |
| `--bitrate RATE` | 从输入推断 | 五个生成版本的目标码率，例如 `4M`；不重编码 H.264 输入 |
| `--force` | 关闭 | 允许覆盖已有编码文件和对应帧输出 |
| `--dry-run` | 关闭 | 只打印拟执行命令，不探测输入、不写文件；输入可以尚不存在 |
| `--help` | — | 显示命令帮助 |

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `FFMPEG_PREFIX` | 本模块的 `dependencies/install` | 定制 FFmpeg/AOM 安装前缀 |
| `FFMPEG_BIN`、`FFPROBE_BIN` | 所选前缀的 `bin/ffmpeg`、`bin/ffprobe` | 可选的命令行工具覆盖；应与实验所链接的库保持一致 |
| `PYTHON_BIN` | `python3` | 激活 Conda 环境后设为 `python3`，使用当前 `PATH` 中的解释器 |

批量入口会为选定安装前缀配置 pkg-config 和动态库路径。直接运行 Python 入口时，使用上方逐 codec 流程中显式设置的环境。

## 实验定义

对每个目标帧，原生程序从容器中取得该帧的编码 payload，按默认 `P=1500` 字节切分为合成 packet。payload 长度为 `L` 时，packet 数为 `N=ceil(L/P)`。

1. 完整解码输入，取得该 codec 的参考帧。
2. 保护第一个 packet。对每个 `j=2..N`，独立进行一次试验，将第 `j` 个 packet 的字节置零，保持 payload 长度不变；末尾 packet 可以不足 `P` 字节。
3. 每次试验创建独立的解码器上下文，并从缓存的编码 packet 重放完整参考历史，避免不同 packet 损坏试验之间累积误差或残留错误隐藏状态。
4. 解码损坏的目标帧，与完整解码参考帧计算 RGB SSIM。packet importance 定义为 `ssim_drop = 1 - SSIM`。

这是合成 payload 损坏模型，不是删除真实 RTP packet。参考图像是**同一 codec 的完整解码结果**，不是未压缩原视频；指标是原始 SSIM 损失，不是 dB-SSIM。原生实现使用 7×7 均匀 SSIM 窗口，并对 RGB 三个通道取平均；Python 回退路径使用对应的 scikit-image SSIM 计算。

`trials.csv` 保留两种位置定义：

| 字段 | 定义 |
| --- | --- |
| `relative_position_lossable` | `(j-2)/(N-2)`；只有一个可损坏 packet 时为 0 |
| `relative_position_all_packets` | `(j-1)/(N-1)` |

逐帧 Pearson 相关系数使用 `relative_position_lossable`。负相关表示该帧中位置越后的 packet 损坏通常造成更小的损失。

明确上报损坏帧解码或 SSIM 计算失败的试验仍保留，记为 `decode_success=False`、`SSIM=0`、`ssim_drop=1`。没有可损坏 packet 的帧会跳过，跳过信息写入运行日志，不生成已完成的逐帧 JSON。

帧范围按 ffprobe 报告了 payload 位置与大小的帧索引。应保存运行日志，以核对请求帧数与实际完成帧数。

提取完整参考帧失败属于实验异常，单帧及帧范围运行（包括并行运行）均适用。原生进程无法启动、非零退出，或退出码为零但未生成非空输出文件时，程序保留该帧目录和诊断报告，当前 codec 命令以非零状态退出。

原生 packet 批处理进程非零退出，或退出码为零但缺少请求的 packet 结果时，也视为实验异常。程序保存下方说明的诊断报告，当前 codec 命令以非零状态退出。这两类异常帧都不生成已完成的 `trials.csv` 或 `summary.json`，也不会把缺失测量记作 SSIM 损失 1；其他正在并发处理的帧仍可能完成。

## 编码配置与适用范围

| Codec | 编码器或输入 | 主要准备设置 |
| --- | --- | --- |
| H.264 | 提供的 H.264 基准输入 | 可选的无 B 帧辅助脚本使用 libx264，默认 CRF 11 |
| H.265 | `libx265` | 两遍编码；禁用 B 帧和 open GOP |
| VP8 | `libvpx` | 两遍编码；单 token partition；禁用 alt-ref |
| VP9 | `libvpx-vp9` | 两遍编码；单 tile |
| AV1 | 优先 `libsvtav1`，否则 `libaom-av1` | 单 tile；SVT 单遍或 AOM 两遍 |
| H.266 | `libvvenc` | 单遍编码；10-bit 输入 |

五个生成版本使用相同的目标码率参数，默认从 H.264 输入推断，也可用 `--bitrate` 指定。各编码器的码率控制算法和实际码率不同，应查看生成的 `*_encode_report.txt`。H.264 输入直接复用，原有 GOP 和码率控制配置不会被改变。若只实验视频片段，应先生成该片段的 H.264 基准，再为同一片段编码其他版本。

每次指定 `--codec` 只调用所选 codec 的编码器；生成全部五种版本需要上述全部编码器。请求 AV1 或 H.266 而对应编码器不可用时，脚本会报错。仓库提供的依赖构建仍会启用文档列出的全部 codec 库。

按[依赖说明](dependencies/README.zh-CN.md)构建实验使用的补丁版 FFmpeg/AOM。补丁基于固定的公开上游版本，包含根据已知合成损坏字节范围进行错误隐藏的实现。测量结果反映了上述编码设置和定制解码恢复规则。

## 输出和数值汇总

每个完成的帧写入 `result/ssim_loss/<input_stem>/frame_<index>/`：

| 文件 | 内容 |
| --- | --- |
| `trials.csv` | packet 索引、两种相对位置、损坏字节范围、解码成功状态、SSIM 与 SSIM 损失 |
| `metadata.json` | 输入及帧身份、packet 大小、解码程序与调度信息 |
| `summary.json` | 逐帧均值、解码失败计数、packet 位置与 SSIM 损失的 Pearson 相关系数 |

用于评分的临时 PPM 重建图像在评分后删除，不生成 PNG/PDF 图。

原生进程异常时，对应帧目录下会保留诊断报告：

| 报告 | 异常类型及额外信息 |
| --- | --- |
| `frames/native_reference_error.json` | 完整参考帧提取失败；包含工作目录、帧索引、输出路径及非空输出是否存在。进程无法启动时，退出码为 `null`。 |
| `frames/native_batch_error_<start>_<stop>.json` | packet 批处理失败或结果不完整；包含预期/已上报/缺失的 packet 索引。每个批次独立保存报告，批次首末 packet 索引至少补齐四位。 |

两类报告都包含执行命令、输入路径、退出码、适用时的信号名称（如 `SIGSEGV`）、目标时间戳与 payload 偏移、packet 大小，以及完整 stdout/stderr。检查报告和运行日志、解决异常后，按前文说明使用新的结果根目录或 `--overwrite`，从头重跑当前 codec 的整个帧范围。

```bash
python3 summarize_results.py --result-root result/ssim_loss
```

汇总生成 `numeric_summary/frames.csv` 和 `numeric_summary/codecs.csv`。codec 汇总包括输入视频数、完成帧数、packet 试验数及失败数、有效逐帧相关系数中负值所占比例、平均逐帧相关系数及平均逐帧 SSIM 损失。后两项对完成帧等权平均。

无法定义的相关系数（例如所有 packet 的 SSIM 损失相同）留空，仅从相关性汇总中排除；失败 packet 试验仍计入损失统计。尚未运行的 codec 显示零计数和空指标。对照运行日志核对当前 codec 的 `frames_completed` 和 `packet_trials`。完全找不到逐帧 `summary.json` 时，汇总脚本报错。

汇总使用结果根目录内所有已完成帧记录中的 SSIM 损失和相关系数，跳过帧数从运行日志统计。

## 常见问题

| 现象 | 检查方法 |
| --- | --- |
| 找不到 `libvvenc` 或某个编码器 | 按依赖文档安装开发库并重建 FFmpeg；使用所选前缀下的 `ffmpeg -encoders` 检查 |
| 提示缺少某种 codec 输入 | 对当前 codec 运行 `prepare_matched_codecs.sh --codec NAME`，核对生成路径与输入主名 |
| 已有文件导致准备阶段退出 | 直接将已生成输入传给 Python 实验入口，或给准备脚本加 `--force` 明确允许覆盖 |
| `AV_CODEC_ID_VVC` 不存在、链接错误或加载了错误的库 | 核对头文件、pkg-config 与动态库来自同一 `FFMPEG_PREFIX`，然后清理并重建 `code/native` |
| Python 依赖导入失败 | 执行 `conda activate letitgo-motivation` 和 `export PYTHON_BIN=python3`，再在本模块目录运行 `python3 -m pip install -r requirements.txt` |
| 耗尽内存或同时运行的进程太多 | 降低 Python 入口的 `--frame-jobs` 与 `--jobs`，例如都设为 `1` |
| 完成帧数少于请求帧数 | 查看日志中的跳过信息，以及视频实际可用的帧范围 |
| 完整参考帧提取失败 | 检查报错指向的 `native_reference_error.json` 和运行日志；解决异常后从头重跑当前 codec 的整个帧范围 |
| 原生 packet 批处理异常退出或结果不完整 | 检查报错指向的 `native_batch_error_*.json` 和运行日志；解决异常后从头重跑当前 codec 的整个帧范围 |
