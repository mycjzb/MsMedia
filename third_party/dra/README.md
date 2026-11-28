# DRA (Digital Rise Audio / GB/T 22726)

广电音频软解，从 msServer `FFmpeg-AVSPlus`（2026-08-21）抽出，接到 Android n6.0。

- `dradec.c` / `dradec.h`：解码核（maliwen2015/ffmpeg_cavs_dra，**只用 DRA，不用 CAVS 视频软解**）
- `libdradec.c`：FFmpeg n6.0 包装（`ff_dra_decoder`）

源码在 `jni/msplayer/dra`。n6 编前跑 `scripts/ffmpeg_dra_id.sh`（识别/parser）；解码核 overlay 仍用本目录 `dradec.c` / `libdradec.c`。
