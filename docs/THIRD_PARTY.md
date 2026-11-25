# 第三方许可

by: Long <mycjzb@gmail.com>

核心库 `libmsmedia` **没有**第三方源代码或预编译库，也没有下载的模型。链接行只有 `libc`。

2026-11-20 21:21 CST 起：上述事实对核心库仍然成立。用户明确要求使用官方最新 FFmpeg 9 裁剪做取流/协议/解封装后，可选隔离库 `libmsff` 静态链入钉选的官方 FFmpeg 9.1。自 2026-11-21 11:26 CST 起，`libmsff` 还静态链入钉选的官方 OpenSSL 3.5.9 LTS；libxml2 与 zlib 仍为系统动态库。这替换了「产品不链 FFmpeg」的旧产品规则，但不把 FFmpeg/OpenSSL 源码打进 git，也不改变 `libmsmedia` 的链接行。详见下方新节与 `docs/FFMPEG_BACKEND.md`。MsMedia 顶层许可证已于 2026-11-24 由用户确认为 LGPL-2.1（见根目录 `LICENSE`）。

## 可选网络库 `libmsnet`（可整项关闭）

自有 HTTP/1.1 客户端在本仓库 `src/net/`，不是 libcurl，也不把 curl/ffmpeg 子进程当作产品网络。TLS 与所需算法调用**系统已安装**的 OpenSSL，经其公开高层 API（`SSL_CTX` / `SSL_set1_host` / SNI / 默认信任库），不把 OpenSSL 源码或二进制抄进仓库，不并行引入 mbedTLS 或 wolfSSL。

本机宿主自测核对（2026-11-20 20:00 CST，Asia/Shanghai；不是四平台或 el7 验收）：

| 项 | 记录 |
| --- | --- |
| 组件 | OpenSSL 3.x 共享库（`libssl.so.3`、`libcrypto.so.3`） |
| 本机版本 | OpenSSL 3.5.7 9 Jun 2026（Debian 包 `libssl3t64` / `libssl-dev` 3.5.7-1~deb13u3） |
| 许可证 | Apache License 2.0（发行包 `/usr/share/doc/libssl-dev/copyright`） |
| 链接方式 | 仅动态链入可选 `libmsnet.so`（及测试程序）；`libmsmedia.so` 的 NEEDED 仍只有 `libc.so.6` |
| 关闭开关 | `ENABLE_NET=0 ./build.sh …`，或不安装 OpenSSL 开发头。不构建、不链接 `libmsnet` 即不含 OpenSSL |
| 安全更新 | 跟随操作系统的 OpenSSL 更新；本项目不内嵌副本 |

HTTP 不由 OpenSSL 提供。未把 libcurl 引入默认核心或可选网络库（本机仅有 curl 运行时、无开发头；且与「自有 HTTP」目标不符，故不采用）。

测试用临时 CA 只由 `build.sh` 写在 `build/<mode>/net-ca/`，不入库、不进入源码包。

未记录许可的代码不能合并。


## 可选 FFmpeg 9.1 后端 `libmsff`（可整项关闭）

官方 tarball 按 `third_party/ffmpeg-9.1.pin` 校验后放入被忽略的 `third_party/cache/`，不入库。构建产物在 `build/ffmpeg-9.1/prefix`。默认关闭 GPL / nonfree / version3。本机构建字符串（2026-11-20 21:21 CST 宿主自测，非正式验收）：

| 项 | 记录 |
| --- | --- |
| 组件 | 官方 FFmpeg 9.1 “Gauss”（`FFMPEG_VERSION` / `av_version_info()` 为 `9.1`） |
| 来源 | https://ffmpeg.org/releases/ffmpeg-9.1.tar.xz ，PGP 签名 `ffmpeg-9.1.tar.xz.asc`，公钥 https://ffmpeg.org/ffmpeg-devel.asc 指纹 `FCF986EA15E6E293A5644F10B4322F04D67658D8` |
| SHA-256（本地在 Good signature 后计算并钉选） | `1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433` |
| 许可证字符串 | `LGPL version 2.1 or later`（`CONFIG_GPL 0`，`CONFIG_NONFREE 0`，`CONFIG_DECODERS 0`） |
| 链接方式（至 2026-11-21 11:12） | FFmpeg 三件静态库链入可选 `libmsff.so`；当时动态依赖系统 `libssl.so.3` / `libcrypto.so.3`（OpenSSL 3.5.7）与 `libxml2.so.2`（2.9.14）及 `libz`/`libm`/`libc` |
| 链接方式（2026-11-21 11:26 起） | FFmpeg 三件静态库 + 钉选 OpenSSL 3.5.9 LTS 的 PIC `libssl.a`/`libcrypto.a` 链入 `libmsff.so`（及 `test_ff_backend`）；动态依赖仍为系统 `libxml2.so.2`（2.9.14，MIT 风格）及 `libz`/`libm`/`libc`。**不再** NEEDED 系统 libssl/libcrypto。核心 `libmsmedia` 仍无 SSL。旧 `libmsnet` 仍动态链系统 OpenSSL 3.5.7，仅对照 |
| 关闭开关 | `ENABLE_FFMPEG=0 ./build.sh …`。不构建 `libmsff` 即产品进程可不链 FFmpeg |
| 分发义务 | 必须保留 FFmpeg LGPL 2.1+ 声明并提供对应源码（或书面要约）。静态链入还通常需要再链接条件。钉选 OpenSSL 3.5.9 为 Apache-2.0，静态合入后分发 `libmsff` 通常还须提供该 OpenSSL 源码（或书面要约）及 NOTICE/归属。与 LGPL 2.1 的组合要如实披露。不能声称没有任何开源义务，也MsMedia 顶层许可证为 LGPL-2.1 |

官方 `LICENSE.md` 称「据其所知 OpenSSL 与 LGPL 兼容」、与 GPLv2/v3 不兼容；因此本构建不得打开 `--enable-gpl`。这不是法律意见。

系统预装的 libav（本机可见 FFmpeg 7.1 系 `libavformat.so.61`）禁止出现在 `libmsff` / `test_ff_backend` 的 NEEDED 中；以绝对路径 prefix 静态库链接防误链。


2026-11-20 22:10 CST（Asia/Shanghai）补充：本轮未更换 XML 后端，仍动态链系统 libxml2 2.9.14；官方 DASH 依赖 libxml2，不是「FFmpeg 自带 XML」。未做一体静态依赖裁剪。`libmsff.so.1.0.0` 去调试符号后仍为 2105232 字节，该数字 **不含** 系统 libssl/libxml2 等外部动态库；约 11 MiB 只曾按动态文件求和估算，非实测，不作承诺。因适配器负 PTS 处理，BuildID 变为 `992811bab811ac66030a95322d4a8e9235f029d9`。分发义务与关闭开关同上。能力矩阵见 `docs/FFMPEG_BACKEND.md`。


2026-11-21 11:12 CST（Asia/Shanghai）补充：正确性里程碑未更换 XML/SSL 链接方式。`libmsff.so.1.0.0` 去调试符号后为 2109328 字节，仍动态依赖系统 OpenSSL 与 libxml2；不能称全自包含。已批准的 OpenSSL 源码裁剪静态合入尚未实施。


2026-11-21 11:26 CST（Asia/Shanghai）补充：已实施官方钉选 OpenSSL 3.5.9 LTS（SHA-256 `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`，PGP 指纹 `B146647E45A7B33947AB226B2A2C87D161692D40`，许可 Apache-2.0，pin 见 `third_party/openssl-3.5.9.pin`）。源码 tar 只缓存于被忽略的 `third_party/cache/`，不入库。`libmsff.so.1.0.0` 去调试符号后为 **7986448** 字节，直接 NEEDED 不再含 libssl/libcrypto，仍动态依赖 libxml2/zlib。静态 SSL 增量是与合入前 2109328 相减得到的 5877120 字节，不是系统动态 `.so` 相加。默认信任走宿主 `/etc/ssl/certs` 符号链接，不是捆绑 CA。测试 CA 不入库。顶层许可证后于 2026-11-24 确认为 LGPL-2.1。细节与裁剪开关见 `docs/FFMPEG_BACKEND.md` 第 9 节。


2026-11-21 21:49 CST（Asia/Shanghai）补充：当时评估过 libass，因切片范围未引入。轻量去标签/布局模型仍在 `libmsmedia`（无第三方）。

2026-11-22 04:16 CST（Asia/Shanghai）补充：在许可与体积核实后，完整 ASS 栅格采用可选隔离模块 `libmsass`，**不链入** `libmsmedia` / `libmsff`，**不改变**仍未选定的顶层许可证，产品 FFmpeg 仍 `CONFIG_LIBASS=0`。钉选官方 tarball（`third_party/*.pin`，缓存不入库）：

- **libass 0.17.3**：库本身为 ISC。发行包中 GPL-2+ 只出现在 `ltmain.sh` 与发行包装文件，不链入本库。构建关闭 fontconfig、libunibreak，不要求系统字体 provider。
- **FreeType 2.13.3**：双许可 FTL 或 GPLv2；本项目**只选 FTL**，关闭 bzip2/png/brotli/harfbuzz。须保留 FTL 宣传句：Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- **HarfBuzz 10.2.0**：Old MIT。不编入其测试/样例 OFL 字体，不打包进仓库。宿主构建因本箱 `/usr/bin` 无 g++，仅此依赖与 `libmsass.so` 最终链接使用已有 conda-forge g++ 16.2，并把 libstdc++ 静态链入可选 so（NEEDED 不含 libstdc++）。Android 使用 NDK clang++ / 静态 libc++。
- **FriBidi 1.0.16**：LGPL-2.1-or-later。仅静态链入可选 `libmsass`，处理方式同可选 LGPL FFmpeg（源码钉选 + 静态 `.a` + 构建脚本可重链）。不得把 FriBidi 链进核心库。
- **未采用** fontconfig、Graphite2、glib、icu、cairo、libunibreak、libpng。系统 zlib 动态依赖与项目其它可选库相同。
- **字体**：不下载、不把不明授权字体打进仓库或 APK。宿主使用已安装的 DejaVu Sans（Bitstream Vera）与 Noto Sans CJK Regular（SIL OFL 1.1）系统文件（测试里只做符号链接）。Android 扫描 `/system/fonts`。附件字体默认 `extract_fonts=0`。

关闭开关：`ENABLE_ASS_RENDER=0`。体积须单独报告可选 `libmsass` 与其静态依赖，不得并入默认核心。

## Linux VAAPI（运行时可选，可整项关闭于未编入时）

- **libva 2.22.0 / libva-drm**（Intel，MIT）：仅运行时 `dlopen` 系统 `libva.so.2` 与 `libva-drm.so.2`。`libmshw` 不 NEEDED 这两份库，也不 NEEDED `libdrm`。仓库只保留最小 ABI 子集 `src/hw/ms_hw_vaapi_libva.h`（类型、与 2.22 一致的 `VAProfile` 数值、函数指针），并保留 Intel MIT 版权声明；不纳入 libva 源码或完整头文件。
- 用途：DRM 节点探测与码流参数到 `VAProfile` 的映射。本构建不实现提交/出画，也不把系统 libva 软件后端当成硬解。
- 关闭方式：非 Linux 宿主构建走桩；设备缺失时能力查询为非硬解。详见 `docs/HW_LINUX.md`。

## 2026-11-24 阶段 6：单 so 静态合入裁剪 OpenSSL 3.5.9

路线 B 主力 `libmsplayer.so` 静态链接 `scripts/openssl_slim_build.sh` 构建的 OpenSSL 3.5.9（Apache-2.0，源码同上钉选，补丁 `scripts/openssl_slim_patch.py` 只去掉 default provider 的编码器/存储/KEM 算法表）。分发该 so 需附 OpenSSL 的 Apache-2.0 许可与 NOTICE，以及 FFmpeg 的 LGPL-2.1+ 声明与对应源码（或书面要约）。不使用 mbedTLS。


## 2026-11-25：Media3（androidx/media）音画同步算法移植

硬解播放路径的音频钟、出帧决策、VSYNC 吸附与帧率滞回按 `androidx/media` 仓库 `main@101e97c`（2026-11-24 核对）逐函数移植，对照表见 `docs/MEDIA3_SYNC_PORT.md`。

| 项 | 记录 |
| --- | --- |
| 组件 | AndroidX Media3（原 ExoPlayer 后继）音画同步相关类 |
| 来源 | https://github.com/androidx/media ，修订 `101e97c` |
| 许可证 | Apache License 2.0（https://www.apache.org/licenses/LICENSE-2.0） |
| 纳入方式 | 算法与状态机移植到本仓库 C/Java（`src/hw/ms_media3_*.c`、`android/hw/java/.../media3/`），不是二进制依赖，也不把 Media3 整库链入 |
| 版权声明 | 各移植文件头保留 “Copyright (C) The Android Open Source Project” 与 Apache-2.0 声明；接线注释 by: Long <mycjzb@gmail.com> |
| 与工程许可证 | 本仓库顶层为 LGPL-2.1。FSF 将 Apache-2.0 视为与 LGPL-2.1 **不兼容**；这不是法律意见，但分发合入本移植的二进制时存在兼容性风险，须自行评估。**未改**根目录 `LICENSE`。 |

API 18–20 的 `releaseOutputBuffer` 睡眠路径另外对照 ExoPlayer `release-v2@dd430f7` 的 `renderOutputBuffer`（Media3 main 最低 API 21），许可同为 Apache-2.0。
