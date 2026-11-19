# 进度笔记

by: Long <mycjzb@gmail.com>

时间一律使用北京时间（UTC+8）。

## 2026-11-19 15:08

### 阶段

设计初稿，等待用户确认。实现未开始。

### 已完成

- 按已批准方向写成可审阅文档（本文件所列路径）
- 作者署名为用户明确给出的 `by: Long <mycjzb@gmail.com>`
- 路线图 P0–P5 与设计中的接口、矩阵阶段对齐（见下方交叉检查）

### 进行中

- P0 设计审阅。用户尚未确认，因此 P0 验收未完成

### 未开始 / 未验证

- 全部 C 源码、构建脚本、测试、合成样本生成器
- 体积、内存、首屏（首个关键帧 AU）测量
- Android / iOS / Windows / Linux 硬解
- HLS、DASH、FLV、MP4、MOV、字幕、seek、倍速、AI 字幕的实现
- 实现代码仍未开始。公开仓库提交记录见下一节，不把未返回的 SHA 写成已完成

### 决策

- 确认前只写文档，不把草稿代码当成已批准实现（当前也没有实现草稿）
- 许可证未选，不添加 LICENSE，不把 MIT 写成已生效
- 目标 codec 与容器全部标记为未实现
- AI 字幕、保音调、协议插件、清单调度都在核心外，P0 只定接口
- 不修改全局 git 作者配置；署名写进文档和未来源文件
- 不读取、不复制其他私有仓库

### 检查证据

- 仓库在写这些文档前只有种子 README，没有 `include/`、`src/`、`tests/`
- 交叉检查（写文档时执行）：
  - DESIGN 的公共事件覆盖音轨、字幕、seek、倍速、AI cue；ROADMAP 把实现分别放在 P2/P3/P4/P5，没有写成已完成
  - 四态矩阵每一格的当前状态都是未实现；P1 只承诺 TS + H.264 + AAC 闭环和其余 codec 的查询入口
  - HLS/MPD 在 DESIGN 中是清单调度，ROADMAP P3 依赖 P2 的 TS/fMP4，不把清单当成 demux
  - 硬解成功条件在 DESIGN 与 P1 验收中一致：未核实必须报告非硬解
  - AVS+ 与 AVS2、解密与解析、软解与 time-stretch 三条都单独写了约束

### 阻塞与待确认

见 `docs/DESIGN.md` 末节。主要阻塞是：设计未确认、许可证未选、四平台工具链与真机尚未使用。

### 提交记录

- 2026-11-19：这 8 份设计文档只在草稿仓库里。本轮没有提交到 https://github.com/mycjzb/MsMedia 。公开仓库上的提交以该仓库实际历史为准，不在这里预写 SHA。

## 2026-11-19 15:13

收到一张快速换台参考图。已写入 `docs/DESIGN.md` 第 17 节和路线图可选项。该图不是实现，也不是测试证据。图中的运营商兼容和 0.35 秒出画均标为未验证、非本项目实测。

## 2026-11-19 15:14

收到一张兼容性清单参考图，已归类到 `docs/COMPATIBILITY.md`。全部未实现。E-AC-3 与 G.711 补进编码目标，不替换原有 codec 清单，不把列表、EPG、回看模板放进核心。

## 2026-11-19 15:15 决策

网络改为统一接口，保留系统 backend，并可选自有 HTTP/1.1。

## 2026-11-19 15:17 决策

自有网络第一版的 TLS 定稿为 OpenSSL。用户依据自己的使用经验认为它比 mbedTLS 稳定，并需要复用其算法；本项目没有做普遍对比测试。wolfSSL 未测，不纳入。第一版不并行实现多套 TLS 后端，只做一份供 HTTP(S)、RTMP(S)、RTSP(S) 共用的薄封装。摘要、HMAC 和加解密走 OpenSSL 公开高层 API。协议遗留算法与 TLS 安全策略分开，不为旧协议打开 legacy provider 或弱套件。版本取仍受维护的发行线，具体号引入时再核对。裁剪和体积以后实测。解析核心不强制链接 OpenSSL。默认包仍不双栈同时打包。此决定不开始编码，OpenSSL 尚未入库。默认包启用自有栈还是系统栈仍待确认。

### 下一步（已被后续记录取代）

用户随后确认在 Bot 电脑上写代码，见下一节。

## 2026-11-19 15:43（北京时间，UTC+8）

### 阶段

在 Bot 电脑 `/workspace/MsMedia` 开始 P1a 编码。这是会话 + MPEG-TS 识别/PAT-PMT 的可验收起步，不是完整播放器，也不是 P1 全部验收。

工作目录由公开仓库 `https://github.com/mycjzb/MsMedia` 克隆而来（当时已有两次文档提交，见下方）。本轮**没有** commit，也**没有** push。

### 已完成

- 补入 GitHub 上缺失的 4 份设计文档（`docs/DESIGN.md`、`docs/ROADMAP.md`、`docs/PROGRESS.md`、`docs/COMPATIBILITY.md`）。未覆盖已有文件中仅文档更完整的部分：`README.md` 增加了兼容性表链接；`docs/THIRD_PARTY.md` 采用含 OpenSSL 说明的较完整稿（仍无第三方源码）。
- 根目录 `build.sh`：`default` / `debug` / `release` / `release upx` / `clean`（`release-upx` 为别名）。库不做 UPX；缺少 `pack` 时明确失败。
- 公共 C ABI `include/msmedia/msmedia.h`：会话、上限、增量 push/pull、release、flush/reset/cancel/EOS、epoch、注册查询、硬解查询骨架。
- MPEG-TS 188/192 同步、PAT/PMT 轨道发现；PES 仅内部 `ms_session_debug_pull_pes` 可见，测试证明公共接口不发出 `AU` / `CODEC_CONFIG`。
- 合成样本测试：碎片输入、前缀垃圾、未知探测、坏 CRC、保留 adaptation、加扰 PMT、输出槽反压、reset/epoch、取消、跨会话、AVS 家族标识不得标成 AVS+、单次 push 上限、分配失败、PES 提取但非 AU。

### 文档交叉检查与合理修正

- GitHub 在 2026-11-19 15:13（北京时间）已有文档提交 `3f50d9e`、`d340ee9`，但缺 4 份主文档；旧进度里「尚未提交到 GitHub」按当时草稿状态已过时，现改为以仓库实际历史为准。
- `build.sh` 目标写成与维护规则一致的 `release upx` 两参数形式，并接受 `release-upx` 别名。
- 默认上限与确认后重同步策略按设计第 9/18 节采用暂定值（探测 1 MiB、队列 2 MiB、槽 8、单次 push 1 MiB、重同步 256 KiB；用尽返回 `MALFORMED`）。仍待最终确认。
- 不把 P0 写成已全部关闭：许可证未选，第 18 节其余项仍开放。用户要求按既定设计开始写代码，因此进入 P1a，而不是宣称设计已全部确认。

### 实际检查（宿主开发自检，不是正式验收）

环境：Bot 电脑，Debian GNU/Linux 13，**无 docker / podman，无 CentOS 7 镜像，无 `/usr/local/bin/pack`**。因此 **el7 容器正式编译与四平台验收均未执行**。需要的工具：可用的隔离容器运行时（docker 或 podman）以及 CentOS 7 / `ms-build-c7:latest` 一类镜像；`release upx` 还需要 `/usr/local/bin/pack`。

宿主自检使用系统工具链（`CC=/usr/bin/gcc`，并把 `/usr/bin` 置于 PATH 前，避免与本机 conda 链接器混用）：

- `./build.sh debug`：gcc (Debian 14.2.0-19) 14.2.0，ASan/UBSan 已启用
- `ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`
- 输出：全部 10 组测试通过，结束句为 `all tests passed`，退出码 0（2026-11-19 15:43，北京时间）
- `./build.sh` 与 `./build.sh release` 成功
- `./build.sh release upx` 退出码 1，报错缺少 `/usr/local/bin/pack`，并说明不对库打包

体积（仅此宿主条件，不能当成四平台或 el7 指标）：

- `build/release/libmsmedia.a`：22316 字节（gcc 14.2.0，`-O2 -DNDEBUG`，strip；对象 text+data 合计 17923 字节）
- 开启的能力：仅 P1a 核心（无网络、无 OpenSSL、无平台硬解）

### 未实现 / 未验证

- H.264 / AAC 完整 AU 组帧、SPS/ADTS 码流确认、codec config 事件
- FLV / MP4 / fMP4 / MOV、清单 HLS/DASH、seek、多音轨选择实现
- 自有 HTTP/TLS、OpenSSL 源码或链接、RTMP/RTSP/RTP/UDP、FCC
- Android / iOS / Windows / Linux 硬解与渲染；直播播放
- 正式 CentOS 7 容器构建；四平台发版体积
- 模糊测试未做
- 许可证未选，无 LICENSE

### 本轮未做的 Git 操作

- 未修改全局 git 配置
- 未 commit、未 push、未 force/reset/delete
- 克隆到的提交仍是 `d340ee9`（`docs: 添加 MsMedia 设计初稿与路线图`，2026-11-19 15:13:23 +0800）

### 下一步

- 在 P1a 回归保持绿色的前提下，做 H.264 Annex B 与 AAC ADTS 的完整 AU 组帧（仍用合成样本，不宣称硬解或出画）
- 许可证选定前不公开 push 实现
- 有 el7 容器后再做正式编译验收

## 2026-11-19 15:53（北京时间，UTC+8）

### 阶段

P1b：在 `/workspace/MsMedia` 继续 Bot 电脑开发。实现 MPEG-TS 上的 AAC ADTS 完整 AU 与 AudioSpecificConfig。H.264 Annex B 组帧范围过大，本轮**不实现**，保持容器标识，避免伪装 P1 闭环。

本轮**没有** commit / push，未改全局 git 配置，未添加 LICENSE，未启动 CloudAgent，未在宿主上编译或运行测试。

### 已完成（代码与文档）

- 扩展公共事件字段：`has_pts` / `has_dts` / `pts` / `dts` / 时基 / `is_keyframe` / `discontinuity` / `format_changed` / `preroll`
- 新增 `src/codec/ms_adts.c`：增量 ADTS 组帧；只在长度字段齐全时发 AU；由 ADTS 头生成 2 字节 ASC
- 复用已有 PES 组装、会话事件队列与上限：AU/config 计入 `au_queue_bytes`，满时 `LIMIT` 且不丢已完整帧；`pull` 会再 drain
- `ms_codec_query(AAC)` 改为 `AU`；H.264 仍为 `CONTAINER_ID`；硬解查询仍固定非硬解
- 合成测试（尚未执行）：跨 TS 边界、逐字节碎片、一 PES 两 AU 与 PTS 推算、跨 PES 重组与时间戳归属、EOS 丢半帧、flush/reset、配置变化、坏长度、反压后再取不丢帧；P1a 旧例保留

支持子集：ADTS layer=0，index 0..12，通道 1..7，protection_absent 0/1，AOT=profile+1。不支持 LATM/LOAS、裸 AAC、index 13-15、SBR/PS 扩展 config、软解。

### 隔离编译检查（只做一次，不重复搜索）

- `docker`：无
- `podman`：无
- 已有 `/home/box/ms/msServer/scripts/ms-docker.sh`，但它明确要求 `docker`，本环境无法使用
- 未安装巨大工具，未改宿主安全配置，未把 bwrap/宿主编译当作正式验收，也未再次运行宿主 gcc

因此：**本轮测试未执行。** 上一轮（2026-11-19 15:43，北京时间）P1a 宿主自检数据如实保留，不删除、不改写为本次结果。

### 待隔离环境执行的命令

在提供 docker/podman 以及 CentOS 7 / `ms-build-c7:latest`（或同等 el7 工具链）的容器中，于仓库根目录：

```bash
./build.sh debug
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core
./build.sh release
```

预期：P1a 原 10 组用例与新增 `test_aac_*` 全部通过，结束句 `all tests passed`，退出码 0。`./build.sh release upx` 在无 pack 时仍应明确失败。

### 静态审阅（本轮代替运行测试）

- AU 必须是完整 ADTS 帧，不把 PES 载荷直接当 AU；H.264 PES 路径仍只走调试 PES，不发 `AU`/`CODEC_CONFIG`（旧例 `test_pes_extract_not_au` 保留此断言）
- 坏 ADTS 长度（声明长度小于头长）返回 `MALFORMED`，不按声明长度分配
- 跨 PES 未完成帧锁定开始时的 PTS；同一 PES 后续帧按 1024 采样推算（48 kHz 时 +1920）
- EOS/flush 丢半帧；reset 增加 `session_epoch`
- 反压：队列字节上限，未发出的帧留在 ADTS 缓冲，`pull` 时 drain
- 审阅中修正：PES/ADTS 组装上限不再跟随缩小后的 `au_queue_bytes`。否则反压测试里「一 PES 两帧 + 仅够一帧的输出队列」会在 PES 组装阶段变成 `MALFORMED`，而不是保留第二帧。改为使用 `MS_DEFAULT_AU_QUEUE_BYTES`
- 未编译，仍有未能由运行抓住的集成风险（PMT 后 ADTS 初始化、LIMIT 与 pending 载荷、flush 后再推完整 TS）。当前 LIMIT 会停止并回退当前 TS 包，未消费的 PUSI 可重试；若后续改动吞掉 LIMIT，单槽 pending 有丢下一 PES 的风险，需再加队列或保持该回压约定

### 未实现 / 未验证

- H.264 Annex B AU、SPS/PPS config、IDR/参考判断
- LATM/LOAS、裸 AAC、可播放或出画、四平台硬解、网络/OpenSSL
- 本轮隔离容器测试；P1 验收仍未通过

### 下一步

- 有 el7 隔离环境后先跑上述命令并回归 P1a
- 再以同样小步做 H.264 Annex B（AUD/主图像边界、SPS/PPS config、IDR 关键帧），仍不做软解

## 2026-11-19 16:04（北京时间，UTC+8）

### 阶段

用户明确批准例外：允许 MsMedia 在 Bot 电脑本机编译自测，**不作为正式发版或四平台验收**。本轮收束上一轮未运行的 AAC ADTS 变更，并补上子集拒绝。仍无 CloudAgent、machineId、commit/push。H.264 AU 本轮未扩大。

### 工具链与命令（可复现）

环境：Debian GNU/Linux 13，Bot 电脑 `/workspace/MsMedia`。为避开残留 `CONDA_PREFIX=/home/box/.local/mamba-build`，编译时使用：

```bash
env -u CONDA_PREFIX -u CONDA_DEFAULT_ENV -u LIBRARY_PATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin \
  CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh debug

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build/debug/test_core

env -u CONDA_PREFIX -u LIBRARY_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh release
./build/release/test_core
```

实际编译器：`/usr/bin/gcc`（gcc 14.2.0，Debian 14.2.0-19），链接器 `/usr/bin/ld`（GNU Binutils 2.44）。`./build.sh debug` 输出 `debug: ASan/UBSan enabled (/usr/bin/gcc)`，**未关闭 sanitizer**。

### 测试结果

- `./build.sh debug`：退出码 0；ASan/UBSan 已启用
- `./build/debug/test_core`（上述 ASan/UBSan 选项）：**15 组全部通过**，打印 `all tests passed`，退出码 **0**，无 sanitizer 报错
- `./build.sh release`：退出码 0
- `./build/release/test_core`：同样 15 组全部通过，退出码 **0**
- `./build.sh release upx`：退出码 **1**（无 `/usr/local/bin/pack` 或 `/home/long/bin/pack`），属预期

15 组：P1a 原 10 组 + `test_aac_adts_basic_and_pts`、`test_aac_fragments_and_cross_ts`、`test_aac_two_au_one_pes_and_split_pes`、`test_aac_eos_flush_reset_badlen_config_backpressure`、`test_aac_adts_subset_crc_mpeg_version`。

覆盖：跨 TS/逐字节碎片组帧、一 PES 两 AU 的 PTS 推算、跨 PES 重组时间戳归属、config 变化、CRC 9 字节头（只按头长组帧，不校验 CRC）、截断+EOS、flush/reset 旧 epoch、反压不丢帧、坏长度 `MALFORMED`、`raw_data_blocks!=0` / `channel_config=0` / `sfi=15` 明确 `UNSUPPORTED`、MPEG-2 ADTS（ID=1）组帧。H.264 仍由 `test_pes_extract_not_au` 断言不发 AU/config。

### 体积（release，strip 后）

- `build/release/libmsmedia.a`：30400 字节（2026-11-19 16:04:32，北京时间）
- `build/release/test_core`：63768 字节
- 库内目标文件 text+data：`ms_crc32` 144、`ms_util` 2218、`ms_registry` 761、`ms_session` 6654+16、`ms_ts` 12102+8、`ms_adts` 3218，合计 **25121**
- debug（含 sanitizer）：`libmsmedia.a` 992964，`test_core` 1218976

较 P1a 宿主自测的 release 库 22316 / 目标 17923 有增长，主要来自 `ms_adts.o` 与 TS/会话接线，属预期。

### 本轮代码补强（验证前最小修正）

- `raw_data_blocks!=0`、`channel_configuration=0`（PCE）、`sfi` 13-15：对齐同步后明确返回 `MS_ERR_UNSUPPORTED`，不再按单块 1024 采样伪报 AU
- MPEG-2 与 MPEG-4 ADTS（ID=1/0）均按同一组帧路径接受，并写明不声称额外差异能力
- protection_absent=0 的 9 字节头有合成测试；不校验 CRC 值
- PES/ADTS 组装上限与缩小后的输出 `au_queue_bytes` 分开（上一轮静态审阅已改，本轮跑通反压用例）
- `ms_codec_query(AAC)` 仍为 `AU`，注释和文档写清这是 **TS ADTS 子集**，不是全部 AAC

### 边界（必须分开写）

- 这是经用户授权的**宿主开发自测**，不是 CentOS 7 / `ms-build-c7` 正式编译，也不是四平台验收，更不是可播放或出画
- docker/podman 仍不可用；el7 正式验收仍阻塞
- H.264 Annex B AU 未实现
- 不支持 LATM/LOAS、裸 AAC、多 raw_data_block、PCE、显式采样率、SBR/PS 扩展 config、软解

### 下一步

- 有 el7 隔离环境后再做正式容器编译与同样命令回归
- 再以同样小步做 H.264 Annex B（AUD/主图像边界、SPS/PPS config、IDR），仍不做软解

## 2026-11-19 16:19（北京时间，UTC+8）

### 阶段

P1c：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。实现 MPEG-TS 上 **带 AUD 的 H.264 Annex B 子集** 完整 AU 与 SPS/PPS codec config。无 CloudAgent、machineId、commit/push。未做其它协议，未把无 AUD 路径写成已支持。

### 支持与拒绝

支持：Annex B 3/4 字节起始码；以 AUD（nal_unit_type=9）为访问单元边界；逐行 `frame_mbs_only_flag=1`；config 为 Annex B（4 字节起始码 + SPS + 4 字节起始码 + PPS）；IDR 的 `is_keyframe=1`，非 IDR 为 0（不保证非 IDR 或「带参数集的 I 帧」可安全恢复）；PES 时间戳只属于在该 PES 内开始的第一帧，同一 PES 后续 AU 若无新时间戳则 `has_pts=0`，不杜撰帧率。

明确拒绝：无 AUD（不使用 first_mb 冒充通用边界）；field/PAFF/MBAFF；数据分割与 type>=14 扩展 NAL；不完整 NAL 或没有主图像的数据（如仅 AUD）不当成 AU。SPS 解析含 RBSP emulation prevention 与无符号指数哥伦布防溢出。

`ms_codec_query(H264)` 现为 `AU`，表示上述子集，**不是全部 H.264**。硬解查询仍固定非硬解/未核实。

### 工具链与命令

```bash
env -u CONDA_PREFIX -u CONDA_DEFAULT_ENV -u LIBRARY_PATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin \
  CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh debug

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build/debug/test_core

env -u CONDA_PREFIX -u LIBRARY_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh release
./build/release/test_core
```

实际编译器：`/usr/bin/gcc`（gcc 14.2.0，Debian 14.2.0-19）。`./build.sh debug` 输出 `ASan/UBSan enabled`，未关闭 sanitizer。

### 测试结果

- `./build.sh debug`：退出码 0，ASan/UBSan 已启用
- `./build/debug/test_core`（上述选项）：**18 组全部通过**，`all tests passed`，退出码 **0**，无 sanitizer 报错
- `./build.sh release` + `./build/release/test_core`：同样 18 组通过，退出码 **0**
- `./build.sh release upx`：退出码 **1**（无 pack），属预期

18 组 = 原 15 组全部回归 + `test_h264_aud_basic_config_idr`、`test_h264_fragments_ts_pes_timestamps`、`test_h264_eos_flush_reset_backpressure_reject`。

覆盖：SPS/PPS config 与 IDR 关键帧、含 EPB 的 SPS、跨 TS/逐字节碎片、3/4 字节起始码混用、一 PES 两 AU（第二帧不杜撰 PTS）、跨 PES 重组时间戳归属、EOS 截断、flush/cancel/reset 旧 epoch、配置变化、反压不丢帧、无 AUD / 隔行 SPS / forbidden bit / 短 SPS 拒绝。`test_pes_extract_not_au` 仍断言「仅 AUD、无主图像」不发 AU/config。

自测中曾因「最后一枚 NAL 需下一起始码或 EOS 才收口」和「无 AUD 的最后 VCL 在 EOS 才判定」调整测试；exp-Golomb 编码器对值为 0 时 `u(0)` 的错误已最小修复。未关闭 sanitizer 装通过。

### 体积（release，strip 后，2026-11-19 16:19:08，北京时间）

- `build/release/libmsmedia.a`：40420 字节；`test_core`：88344 字节
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 2218、`ms_registry` 777、`ms_session` 6654+16、`ms_ts` 14366+16、`ms_adts` 3218、`ms_h264` 6690，合计 **34099**
- debug（含 sanitizer）：`libmsmedia.a` 1281818，`test_core` 1648224

较 P1b release 库 30400 / 目标 25121 的增长主要来自 `ms_h264.o` 与 TS 接线，属预期。

### 边界

- 宿主开发自测，不是 CentOS 7 / el7 正式编译，不是四平台验收，不是可播放或实际硬解
- docker/podman 仍不可用
- 无 AUD 的通用主图像边界、隔行/PAFF/MBAFF、软解、参考帧是否齐全、非 IDR 安全起播，均未做

### 下一步

- 有 el7 隔离环境后再做正式容器编译与同样命令回归
- 若继续 P1，再评估是否以单独小步、有明确限制地做无 AUD 边界（必须基于 SPS/PPS 参数的主图像判定，而不是 first_mb 冒充通用支持）

## 2026-11-19 16:30（北京时间，UTC+8）

### 阶段

P1d：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。在现有 `ms_h264` 上补 **无 AUD 主图像边界**，按 ITU-T H.264 7.4.1.2.4 比较 SPS/PPS 与条带头字段，不把 `first_mb_in_slice=0` 当通用边界。保留 AUD 路径。无 CloudAgent、machineId、commit/push。未扩其它 codec / 网络 / 容器。

### 支持子集（明确可验证，不是全部 H.264）

- Annex B 3/4 字节起始码；逐行 `frame_mbs_only_flag=1`
- AUD 仍是边界；无 AUD 时比较 `frame_num`、`pic_parameter_set_id`、`nal_ref_idc` 是否为 0、`IdrPicFlag` / `idr_pic_id`，以及 POC 类型 0 的 `pic_order_cnt_lsb` / `delta_pic_order_cnt_bottom`、类型 1 的 `delta_pic_order_cnt[0/1]`（含 `delta_pic_order_always_zero_flag=1` 时这些字段推断为 0、靠其它字段分界）
- 同一主图像多 slice 合成一个 AU；不同主图像不并成一帧；不无限等待 AUD
- 参数集按 ID 管理（SPS 0..31，同时保留的 PPS 最多 32 个）
- 配置变化、反压、事件顺序、PES 时间戳只属于该 PES 内开始的第一帧，均保持
- IDR 的 `is_keyframe=1`，不保证非 IDR 可安全起播

明确拒绝：field/PAFF/MBAFF；FMO（`num_slice_groups_minus1!=0`）；`separate_colour_plane_flag=1`；未知 SPS/PPS；数据分割与 type>=14；畸形语法与截断 NAL（不越界、不把不完整数据当 AU）。

`ms_codec_query(H264)` 仍为 `AU`，表示上述子集，**不是全部 H.264**。硬解查询仍固定非硬解/未核实。

### 工具链与命令

```bash
env -u CONDA_PREFIX -u CONDA_DEFAULT_ENV -u LIBRARY_PATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin \
  CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh debug

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build/debug/test_core

env -u CONDA_PREFIX -u LIBRARY_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh release
./build/release/test_core
```

实际编译器：`/usr/bin/gcc`（gcc 14.2.0，Debian 14.2.0-19）。`./build.sh debug` 输出 `ASan/UBSan enabled`，未关闭 sanitizer。

### 测试结果

- `./build.sh debug`：退出码 0，ASan/UBSan 已启用
- `./build/debug/test_core`（上述选项）：**原 18 组全部通过**，并新增 3 组无 AUD 用例，合计 21 组，`all tests passed`，退出码 **0**，无 sanitizer 报错
- `./build.sh release` + `./build/release/test_core`：同样 21 组通过，退出码 **0**

原 18 组中 `test_h264_eos_flush_reset_backpressure_reject` 的「无 AUD 用占位条带期望 UNSUPPORTED」已改为合法条带头在 EOS 后发出 1 个 AU（产品行为按本轮契约更新）。隔行 / forbidden bit / 短 SPS 拒绝仍保留。

新增覆盖：合法多 slice 同一图像、连续两帧不分错、SPS/PPS 按 ID 切换与 `format_changed`、PPS 引用未知（`MALFORMED`）、FMO PPS（`UNSUPPORTED`）、POC 类型 0 / 类型 1（always_zero 与 delta）、`nal_ref_idc` 零与非零分界、逐字节跨 PES 碎片与时间戳归属、flush/reset/EOS、反压不丢帧、组帧器 `max_au` 上限、截断起始码、过短条带头。AUD 路径（含历史占位条带）仍通过。

### 独立审阅（指针寿命与 2 MiB 装配上限）

未发布指针：`on_au` / `on_config` 只在回调期间借用组帧缓冲；`evq_enqueue` 立即拷贝到会话节点，`ev.data` 在 `release` / `flush` / `reset` / `destroy` 前由会话持有。LIMIT 时 AU 留在组帧器、config 已成功入队则不再重复发出。未发现需立刻修补的释放后悬空或回调后继续引用组帧缓冲的实质缺陷。调试 PES 视图仍是内部缓冲别名，寿命到下一次 debug_pull / flush / reset / destroy，保持原契约，不是 AU。

硬编码 2 MiB 装配上限：`ts_bind` 仍把 PES / ADTS / H.264 组帧上限固定为 `MS_DEFAULT_AU_QUEUE_BYTES`，**不**跟随调用方缩小的 `au_queue_bytes`（否则一 PES 多帧的反压会在装配阶段变成 `MALFORMED`）。这是有意分离，但也造成调用方无法下调组装上限。多轨最坏峰值约为在用轨道数 ×（PES 组装 + ES 组帧器）× 2 MiB，外加 `in_buf`、输出 evq、按 ID 保存的参数集副本，可明显高于用户设置的 `au_queue_bytes`。本轮只在公共头和 `ts_bind` 注释写明该事实，**不把实现写成低内存**，也未重新把组装上限耦到输出队列（那会破坏已验证的反压行为）。待解决：若继续做资源约束，应增加可下调的独立组装上限（或跨轨总装配预算），并同步改 ABI/文档与反压测试。

### 体积（release，2026-11-19 16:30:36，北京时间）

- `build/release/libmsmedia.a`：44828 字节；`test_core`：108824 字节
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 2218、`ms_registry` 777、`ms_session` 6654+16、`ms_ts` 14302+16、`ms_adts` 3218、`ms_h264` 11234，合计 **53031**
- debug（含 sanitizer）：`libmsmedia.a` 1477306，`test_core` 2051224

较 P1c release 库 40420 / 目标 34099 的增长主要来自 `ms_h264.o`（6690 → 11234）与测试，属预期。

### 边界

- 宿主开发自测，不是 CentOS 7 / el7 正式编译，不是四平台验收，不是可播放或实际硬解
- docker/podman 仍不可用，未跑 `ms-docker.sh` / `ms-build-c7:latest`
- 隔行/PAFF/MBAFF、FMO、软解、参考帧是否齐全、非 IDR 安全起播，均未做
- 尚未对真实编码样本做硬解出画验证

### 下一步

- 有 el7 隔离环境后再做正式容器编译与同样命令回归
- 若继续 P1：评估独立组装上限 / 跨轨总预算，或其它已声明但未做的 codec；仍不做软解或网络/容器扩张

## 2026-11-19 16:44（北京时间，UTC+8）

### 阶段

P1e：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。优先修补上一轮审阅中的资源约束缺口：独立可调的 PES/ES 组装上限，以及跨轨会话动态总预算。无 CloudAgent、machineId、commit/push。未扩格式/网络/其它 codec。**不将 P1 写为通过，不进入 P2。**

### 合同与语义

- `au_queue_bytes` 只限制已发出、未 release 的 AU/config 载荷，与组装上限独立。缩小输出队列不再把合法多帧 PES 装配打成 `MALFORMED`。
- `max_assembly_bytes`（默认 2 MiB，绝对 16 MiB）：单个 PES 组装缓冲或单个 ES 组帧器上限。单 PES/AU 超过该值为 `MALFORMED`（会话失败）。
- `session_budget_bytes`（默认 16 MiB，绝对 64 MiB）：跨轨动态总预算，涵盖输入缓冲、输出队列载荷、各轨 PES、ES 组帧缓冲、ES pending、H.264 参数集 NAL 副本，以及调试 PES 载荷（预算不足时调试副本可放弃，不影响公共 AU 路径）。记账含每块内部头。
- **不计入预算（已写明，不是漏项）**：`ms_session` 本体（嵌入 TS 解析器、轨道表、PSI 固定数组、事件节点元数据、参数集槽位表本身），以及 create 时分配的输出槽表。这些在会话存活期内常驻。
- `struct_size`：新字段追加在旧五字段之后；`0` 视为旧布局，扩展字段取默认。`ms_session_limits_default`/`absolute` 会写入 `sizeof`。调用方只能调低。
- `LIMIT`：输出队列/槽或动态预算此刻用尽；旧状态保留。若 pull 仍能取到事件，release 后可重试。若 `consumed=0` 且 pull 为 `AGAIN`，当前输入在现有限额下无法前进，不要无限重试；可用 flush/reset 恢复。
- `AGAIN`：需要更多输入或当前无事件，不是资源上限。
- `NOMEM`：底层分配失败，已回滚，不把会话标为失败。
- 参数集/缓冲扩展采用先分配再释放，失败时旧块可用；短暂堆峰值可为 old+new。

### 工具链与命令

```bash
env -u CONDA_PREFIX -u CONDA_DEFAULT_ENV -u LIBRARY_PATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin \
  CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh debug

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
./build/debug/test_core

env -u CONDA_PREFIX -u LIBRARY_PATH -u LD_LIBRARY_PATH \
  PATH=/usr/bin:/bin CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib STRIP=/usr/bin/strip \
  ./build.sh release
./build/release/test_core
```

实际编译器：`/usr/bin/gcc`（gcc 14.2.0，Debian 14.2.0-19）。`./build.sh debug` 输出 `ASan/UBSan enabled`，未关闭 sanitizer。

### 测试结果

- debug ASan/UBSan：原 21 组全部通过，新增 4 组（上限合同、队列与组装独立性/反压不丢帧、多轨峰值/参数集更换/分配失败回滚、48 次固定种子畸形压力），合计 25 组，`all tests passed`，退出码 0，无 sanitizer 报错
- release：同样 25 组通过，退出码 0
- 固定种子压力（`0x4D534D31`，48 次、每次 3 个 188 字节包）只作回归，**不是**正式 fuzz

### 实测内存指标（不能把估算当实测）

同一组合成双轨（H.264+ADTS）样本，debug 与 release 测试进程打印的数字一致（2026-11-19 16:44，北京时间）：

- 固定对象（create 时向测试分配器请求的大小，不计入动态预算）：会话本体 **142584** 字节，输出槽表 **96** 字节（默认 8 槽）
- 动态记账（库内 `used`/`peak`，含记账头）：取出 AU 并 release 后 used **6723**，peak **7241**；flush 后 used **544**（参数集仍保留）；reset 后 used **0**
- 测试分配器 live peak **149921**（含固定对象 + 动态块 + 测试分配器自己的块头）
- 该会话 `session_budget_bytes` 设为 **65536**

这些数字只描述该合成样本、该工具链上的这一次运行，不是四平台峰值，也不是真实码流或首个关键帧出画的内存。未测设备硬解。

### 体积（release，2026-11-19 16:44:44，北京时间）

- `build/release/libmsmedia.a`：47516 字节；`test_core`：129312 字节
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7413+16、`ms_ts` 14534+16、`ms_adts` 3178、`ms_h264` 11322，合计 **41186**
- debug（含 sanitizer）：`libmsmedia.a` 1575458，`test_core` 2288584

较 P1d release 库 44828 / 目标 53031 的变化主要来自记账与上限合同（`ms_util`、`ms_session`）。

### 边界

- 宿主开发自测，不是 CentOS 7 / el7 正式编译，不是四平台验收，不是可播放或实际硬解
- docker/podman 仍不可用
- 隔行/FMO/软解/参考帧是否齐全/非 IDR 安全起播，均未做
- 调用方仍不能把组装上限或总预算调到高于默认值（沿用只能调低的合同）
- 固定会话对象约 140 KiB，在 32 轨槽位表全嵌入时偏大，本轮未压缩该结构，只把它排除在动态预算之外并实测

### 下一步

- 有 el7 隔离环境后再做正式容器编译与同样命令回归
- 在 P1 正式通过之前，不进入 P2。若继续：压缩固定会话对象、或 el7/硬解查询，仍不做软解或网络/容器扩张


## 2026-11-19（北京时间）P1f — 按需分配轨道解析状态

P1f：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。优先压缩上一轮实测出的固定 `ms_session`（142584 字节，由 32 路嵌入解析状态引起）。无 CloudAgent、machineId、commit/push。未扩格式/网络/其它 codec。**不将 P1 写为通过，不进入 P2。**

### 改前布局测量（同一工具链，改代码前）

`sizeof`（字节）：

- `ms_h264` 3616（其中 sps[32]+pps[32] 槽位表 3328）、`ms_adts` 160、`ms_ts_track` 3896、`ms_ts_parser` 128912、`ms_session` **142584**
- 会话构成：`tracks[32]` 124672（87.4%，其中 32 份 H.264 115712 / 81.2%）、`evq[64]` 13312（9.3%）、`pat+pmt` 2112、`pes[32]` 1536，其余约 952

结论：最小有效优化是把每轨嵌入的 `ms_h264`/`ms_adts` 改为指针，在首次相应 PES 时按 codec 分配；不把参数集槽再拆一层（整份 H.264 对象随轨惰性分配已覆盖）。PES 表与事件节点本轮不动。

### 实现要点

- `ms_ts_track` 仅保留元数据与 `ms_h264 *` / `ms_adts *`；PMT 只登记轨道，不预先分配解析状态
- 分配走现有 `p->alloc`（会话记账包装），失败返回 `LIMIT`/`NOMEM` 并回滚，不留下半初始化指针
- `track_release` / PAT·PMT 变更 / `reset` 释放对象；`flush` 保留已分配对象（与原先保留参数集一致），但对象现计入预算
- 旧 API、32 轨上限、结构对外 size、AAC/H.264 组帧路径不变
- 顺手修正：`ms_session_push` 在输入已拷入 `in_buf` 后仍须回传处理过程中的 `LIMIT`，不得改报 `OK`

### 测试结果

- debug ASan/UBSan：原 25 组全部通过，新增 1 组（按需分配、双轨累计、预算/分配失败注入回滚、重复 reset、PMT 版本变化释放旧对象），合计 26 组，`all tests passed`，退出码 0，无 sanitizer 报错
- release：同样 26 组通过，退出码 0

### 实测对比（同一合成双轨样本、同一工具链、同一 budget=65536）

P1e 基线（2026-11-19 16:44，北京时间，见上节）：会话本体 **142584**，槽表 **96**，记账 used/peak **6723 / 7241**，flush used **544**，reset used **0**，测试分配器 live peak **149921**。

P1f（2026-11-19 16:53，北京时间，debug 与 release 打印一致）：

- `sizeof`：`ms_ts_track` 136，`tracks[32]` 4352，`ms_ts_parser` 8592，`ms_session` **22264**
- 固定对象（仍不计入动态预算）：会话本体 **22264**（减少 120320），输出槽表 **96**（不变）
- 动态记账：取出 AU 并 release 后 used **10531**，peak **11049**（均增加 3808，即 `sizeof(ms_h264)+sizeof(ms_adts)` 加两块记账头 16+16）；flush 后 used **4352**（解析对象现计入预算并在 flush 时保留）；reset 后 used **0**
- 测试分配器 live peak **33409**（减少 116512）

说明：这不是无条件降低全部峰值。双轨样本的总分配峰值下降，是因为不再为 30 路未使用轨预留解析状态；动态记账峰值上升了 3808 字节。若 32 路都实际使用 H.264，动态侧会再增加约 32×3632，总峰值与改前接近，不会显著低于改前固定对象加上同样装配缓冲。固定本体 22264 与输出槽 96 仍在预算外，不能把 `session_budget_bytes` 说成进程 RSS 上限。当前固定对象里最大剩余项是 `evq[64]`（13312 字节，约 59.8%），本轮未改。

这些数字只描述该合成样本、该工具链上的这一次运行，不是四平台峰值，也不是真实码流或首个关键帧出画的内存。未测设备硬解。

### 体积（release，2026-11-19 16:53:50，北京时间）

- `build/release/libmsmedia.a`：47204 字节；`test_core`：137504 字节（测试体变大）
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7477+16、`ms_ts` 14158+16、`ms_adts` 3178、`ms_h264` 11322，合计 **40874**
- debug（含 sanitizer）：`libmsmedia.a` 1590386，`test_core` 2403880

较 P1e release 库 47516 / 目标 41186 略降；`test_core` 因新增用例增大。

### 边界

- 宿主开发自测，不是 CentOS 7 / el7 正式编译，不是四平台验收，不是可播放或实际硬解
- docker/podman 仍不可用
- 隔行/FMO/软解/参考帧是否齐全/非 IDR 安全起播，均未做
- 调用方仍不能把组装上限或总预算调到高于默认值（沿用只能调低的合同）
- 参数集槽仍在单个 `ms_h264` 对象内按 ID 定长数组；未再对 sps/pps 做二级惰性（收益相对整份对象按需分配较小，风险更高）

### 下一步

- 有 el7 隔离环境后再做正式容器编译与同样命令回归
- 在 P1 正式通过之前，不进入 P2。若继续：审视剩余固定开销（如事件节点表）、或 el7/硬解查询，仍不做软解或网络/容器扩张


## 2026-11-19（北京时间）P1g — 真实编码器输出交叉验证

P1g：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。停止 evq 等微优化，优先用更接近真实编码器的输出做开发交叉验证。无 CloudAgent、machineId、commit/push。未扩格式/网络/其它 codec。**不将 P1 写为通过，不进入 P2。不得把本轮样本写成全部 H.264 兼容。**

### 缺口与工具

- 原 26 组几乎全是库内自造 TS/NAL，不能单独证明与真实 muxer/编码器一致
- 本机已有系统包 `/usr/bin/ffmpeg` 与 `/usr/bin/ffprobe`（Debian 7.1.5-0+deb13u1，含 libx264 与原生 aac）。这不是 AVS 测试，未使用 `/ms/build/ffmpeg`，也未把 FFmpeg 链入 `libmsmedia.a`
- 未下载大型框架或成片。样本由 lavfi `testsrc`/`sine` 在本机即时编码封装，写入 `build/dev-samples/`（不入库）。许可：自有生成内容，FFmpeg 仅作可选开发工具

### 新增开发工具（可选，非正式入口）

- `tests/ms_ts_inspect.c`：只经公共 ABI 读取任意碎片 TS，统计轨道/config/AU、PTS/DTS、IDR、SHA-256，以及 AAC 去 ADTS 头后的 payload hash。`./build.sh` 会附带编出 `build/<mode>/ms_ts_inspect`，根入口仍是 `./build.sh`
- `tests/dev_crosscheck.sh` + `tests/dev_crosscheck.py`：生成样本并对照。命令：先 `./build.sh debug`，再 `tests/dev_crosscheck.sh`

生成条件（均为逐行 yuv420p，约 160x120、15 fps、1–2 秒、音轨单声道 AAC-LC、约 14–30 KiB）：

| 样本 | 条件 |
|---|---|
| bl_aud1_s1_48k.ts | baseline，AUD=1，1 slice，48 kHz，无 B 帧 |
| bl_aud0_s1_48k.ts | baseline，AUD=0，1 slice，48 kHz，无 B 帧 |
| bl_aud0_s2_48k.ts | baseline，AUD=0，2 slice，48 kHz，无 B 帧 |
| main_aud1_s1_441.ts | main，AUD=1，44.1 kHz，B 帧 |
| high_aud0_s2_48k.ts | high，AUD=0，2 slice，48 kHz，B 帧 |
| bl_pts_wrap.ts | baseline +AUD 48 kHz，时长 2s，偏移使 90 kHz 33 位 PTS 在中途回绕 |
| concat_disc.ts | 两段短 TS 直接拼接（CC/PCR 不连续，仅作观察） |

对照：视频 AU 数 = ffprobe frames；PTS/关键帧按 **packet（解码/码流顺序）** 对拍（`show_frames` 在有 B 帧时为显示顺序，不能按索引对拍）；视频载荷用独立 Python TS PES 重组后做 SHA-256；音频用 `ffmpeg -c copy -f adts` 去头后的 payload SHA-256。推入块大小 188/17/1 结果一致。

### 发现与最小修复

1. **EOS 未关闭 `PES_packet_length=0` 的最后一枚视频 PES**（ffmpeg mpegts 常见）。原理：此类 PES 只在下一 PUSI 结束，文件末尾没有下一 PUSI 时最后一帧留在 PES 缓冲，H.264 组帧器收不到，表现为少 1 个视频 AU。修复：`end_all` 在 EOS 时先 `close_open_pes` 再 `ms_h264_end`。用例：`test_unbounded_pes_eos_and_pts_wrap`。
2. **新 PES 的 PTS 被上一帧 `floor(90000*1024/rate)` 推算值覆盖**。原理：一帧发出后会预推下一帧时间戳；下一 PES 若 `len==0` 仍保留该推算，44.1 kHz 等不能整除 90 kHz 时与 muxer PTS 差 1 tick 并累积。修复：`ms_adts_begin_pes` 在没有未完成帧时清掉预推算，改用本 PES 时间戳；同一 PES 内后续帧仍用向下取整推算（已有「一 PES 两 AU」用例保持）。
3. **不是 bug、已记录**：同一 PES 内多帧 44.1 kHz AAC 相对 ffprobe 最多偏离 12 ticks（本构建明确向下取整，不编造更高精度）。B 帧码流的 AU 顺序是解码顺序，PTS 可以乱序，这与 packet 一致。`is_keyframe` 仍只表示 IDR。PTS 按 33 位原样给出，回绕后数值变小，不展开成 64 位。硬解/四平台未做。拼接断流没有作为产品保证。

### 测试结果

- debug ASan/UBSan：原 26 组通过，新增 1 组（EOS 关闭 length=0 PES、33 位 PTS 回绕、每 PES 一帧 44.1 kHz 采用新 PTS），合计 27 组，`all tests passed`，退出码 0
- release：同样 27 组通过，退出码 0
- `tests/dev_crosscheck.sh`：上表预期样本均与独立对照一致（44.1 kHz 组另注明 intra-PES 取整偏差）

### 体积（release，2026-11-19 17:06:47，北京时间）

- `build/release/libmsmedia.a`：47412 字节；`test_core`：141600；`ms_ts_inspect`：63848
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7477+16、`ms_ts` 14350+16、`ms_adts` 3194、`ms_h264` 11322，合计 **41082**
- debug（含 sanitizer）：`libmsmedia.a` 1601162，`test_core` 2465080，`ms_ts_inspect` 1458136

### 边界与未做项

- 宿主开发自测，不是 CentOS 7 / el7 正式编译，不是四平台验收，不是可播放或实际硬解
- 只覆盖本构建声明的 ADTS AAC + 逐行 H.264 子集（AUD 或 7.4.1.2.4）；未测隔行/FMO/LATM/场编码/专业摄像机码流
- 未把 x264 的 SEI、CABAC、High profile 工具集当「已解码」或「已硬解」
- 适配字段 discontinuity_indicator、乱序 TS 包、音视频交错更极端的断流，未作为本轮保证
- docker/podman 仍不可用

### 下一步（P1 剩余验收项）

- 有 el7 隔离环境后做正式容器编译与同样命令回归
- 四平台与真实硬解查询/出画仍未做
- 许可证等 P0 待确认项仍在
- 在 P1 正式通过之前，不进入 P2；不扩展其它 codec/容器/网络

## 2026-11-19（北京时间）P1h — AAC 余数时间戳、TS 连续性基本安全与有界实流抽样

P1h：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。优先两项真实直播风险，不做 evq 微优化，不扩新格式/网络模块。无 CloudAgent、machineId、commit/push。**不将 P1 写为通过，不进入 P2。不将开发解析阶段写成全部完成。**

### 实现

1. **AAC 同一 PES 多帧 44.1 kHz 时间戳余数累积。** `ms_adts` 增加 `ts_rem`/`ts_rate`：`rem += 90000*1024; inc = rem/rate; rem %= rate; pts += inc`（33 位掩码）。与 `floor(k*90000*1024/rate)` 等价，避免逐帧 floor 累积。48 kHz 整除，仍精确 +1920，余数恒 0。新 PES 的 PTS（`begin_pes` 且 `len==0`，以及 `attach_start_ts`）重新锚定 rem=0；reset / flush_partial / 不连续触发的 flush / 采样率变化清余数。无 PTS 不杜撰绝对时钟（与原先一样清掉 start ts）。跨 PES 的帧起点归属不变。

2. **TS 连续性基本安全丢弃。** 依据 ITU-T H.222.0 / ISO/IEC 13818-1 2.4.3.3–2.4.3.4 公开条文（不复制不明许可代码）：有载荷包 CC 应 +1（含 15→0 回绕）；`adaptation_field_control=10` 不递增；合法重复为连续第二个同 PID、同 CC、AFC 01/11 且载荷相同的包（PCR 可不同，故只比较载荷），丢弃第二包且不发事件；第三次同 CC 或同 CC 但载荷不同视为异常。丢包或显式 `discontinuity_indicator`、TEI、已在组装的媒体轨遇到加扰包：只丢该 PID 未完成 PES/ES（flush 部分 AU，保留 SPS/PPS 与 AAC 配置），发 `MS_EVENT_DISCONTINUITY`（能归属则填 `ev.track`），等待下一 PUSI 恢复，不把缺片当完整 AU。下一完整 AU 的 `ev.discontinuity=1`。不增加 `timeline_epoch` / `selection_epoch` / `session_epoch`。其它轨不被无故清空。`current_next=0` 的 PSI 仍忽略。AFC=00 仍为 MALFORMED。不承诺对网络/RTP 乱序重排。加扰的 PAT/PMT 或未知 PID 只跳过明文解析，不刷媒体不连续事件（避免把加扰 PMT 误报成节目断流）。

### 测试命令与结果（北京时间）

- `PATH=/usr/bin:/bin CC=/usr/bin/gcc ./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/debug/test_core`：原 27 组 + `test_aac_remainder_intra_pes` + `test_ts_continuity_safety`，合计 29 组，`all tests passed`，退出码 0，约 0.042 秒（约 17:24）。
- `./build.sh release` 后 `./build/release/test_core`：同样 29 组通过，退出码 0，约 0.004 秒。
- `tests/dev_crosscheck.sh --inspect build/release/ms_ts_inspect`：预期 lavfi 样本均对照通过；`main_aud1_s1_441.ts` 注明 `aac intra-PES PTS remainder vs ffprobe: 28 frames differ, max 1 ticks`（P1g 为 max 12 ticks）。`concat_disc.ts` 仍只作观察 NOTE，不是产品保证。约 3.102 秒。
- 48 kHz 三帧同一 PES 仍为 90000 / 91920 / 93840。单元测试用 16 帧 44.1 kHz 同一 PES 与精确公式对拍（一次 push 64 帧会先填满 `MS_PENDING_EVENT_MAX=64` 的 evq，按最低下标取事件会打乱顺序，本轮不改 evq；更长流由 inspect 按块拉取，dev_crosscheck 覆盖）。

针对连续性用例：合成丢一包、合法重复、第三次同 CC、同 CC 异载荷、CC 15→0、纯 adaptation、显式 discontinuity_indicator+CC 跳跃、TEI、加扰中间包；并锁住「视频轨不被音频缺片清空」。

### 发现与修复

1. 加扰 PMT 若一律 `notify_pid_gap`，会多发 DISCONTINUITY，打破「只有 FORMAT、没有轨道」的既有用例。改为仅媒体轨或已有未完成 PES 时才丢弃并发事件。
2. 默认 evq 64 槽按最低空闲下标入队/出队，不是严格 FIFO；一次排出超过槽数的 AU 时，测试若按 pull 顺序对拍 PTS 会错。本轮不扩展 evq，单元测试改为 16 帧，inspect 交叉验证走分块拉取。
3. 回归中途发现 `test_unknown_and_malformed` 依赖上述加扰 PMT 行为；已按标准语义收窄，而不是放宽断言。

### 体积与预算（release，2026-11-19 17:24 / 17:29 重建，北京时间）

- `build/release/libmsmedia.a`：50444 字节（P1g 为 47412）；`test_core`：153888；`ms_ts_inspect`：63848
- 库内目标 text+data：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7541+16、`ms_ts` 17142+16、`ms_adts` 3402、`ms_h264` 11290，合计 **44114**（P1g 为 41082）
- 固定会话对象 23064（P1g 为 22264），增加来自每槽 CC/重复标记与轨上 pending_disc，不是装配预算退化。双轨用例 `accounted_used_after_au=10547`、`accounted_peak=11065`、`alloc_live_peak=34225`、`flush_used=4368`、`reset_used=0`，`budget=65536`。所有权与惰性分配合同未改。
- debug（含 sanitizer）：`libmsmedia.a` 1688354，`test_core` 2764304，`ms_ts_inspect` 1532504

### 有界实流抽样（外部 curl，不是产品网络/HLS；非正式验收）

用户授权三个实流地址。仓库文档只写 **实流 A（HTTPS TS）/ 实流 B（HTTP TS）/ 实流 C（HTTPS m3u8）**，完整 URL 只留在 gitignore 的 `build/dev-samples/live/`（含 headers、writeout、curl.err、sha256、ffprobe/inspect 日志）。curl 使用系统 TLS 正常证书校验（未关验证，`ssl_verify_result=0`），未改远端。每条 TS 连接 `--max-time 15 --max-filesize 16777216`。不是硬解、不是出画。

第一次抽样（约 17:25:47–17:26:02）：A/B 均 HTTP 200、`video/mp2t`，15 秒超时（curl 28，**不是** 16MiB 限额；各约 3469806 字节），二者 SHA-256 相同。C 清单 587 字节、200、`application/vnd.apple.mpegurl`、完整成功。A 经 Cloudflare HTTP/2；B 为 HTTP/1.1 `msServer/3.5.1`。随后用第一次清单里的相对分片去拉，约 90 秒后全部 HTTP 404（清单窗口约 5 片×5 秒，已过期）——这是过期窗口，不是 15 秒/16MiB 限额。

第二次抽样（约 17:30:17–17:30:33）：A 1729483 字节、B 1747432 字节，**字节不再相同**（并行拉取、起点或路径不同），不能假设内容完全一致。两者都是 15 秒超时（仍非 16MiB）。立即重拉 C 清单（MEDIA-SEQUENCE 已前进）并马上取最新 3 个分片，HTTP 200、完整文件（约 1.02–1.09 MiB，对齐 188 字节，exit 0）。

ffprobe（系统 7.1.5，仅作参考）对 A/B/C 分片识别为同一节目形态：MPEG-TS，视频 H.264 High 1280×720 逐行 yuv420p 25 fps PID 0x100，音频 AAC LC 44.1 kHz 双声道 PID 0x101。另见 PAT 0、PMT 0x1000、0x0011。A/B 从直播中途截取，ffprobe 有「non-existing PPS」起始警告，但仍给出流信息。未见 HEVC 等未实现 codec，不存在伪报硬解。

`ms_ts_inspect`（公共 ABI，release）：

- 实流 C 三个独立分片（`#EXT-X-INDEPENDENT-SEGMENTS`）：status=ok，退出码 0。视频 AU 数与 ffprobe `nb_read_packets` 一致（125/125/125），音频 AU 数一致（216/215/215）。AAC 同一 PES 内增量出现 2090/2090/2090/2090/2089，与 44.1 kHz 余数累积相符。H.264 PTS 呈 B 帧解码顺序（含负增量）。每片 1 个关键帧。AAC/H.264 config 哈希在三片之间相同，且 AAC config 与 A/B 抽样相同。
- 实流 A/B 15 秒中途截取：只发出少量 AAC AU（本轮为 4 帧，增量 2090），视频 AU=0，summary `status=UNSUPPORTED`，inspect 退出码 1。尾部不足一包（余 71/160 字节）。**不能把这两段截取写成组帧已通过，更不能写成硬解成功。** 同节目的独立 HLS 分片可以组帧，说明问题更像中途截取/子集边界，尚未在本轮单独定因。

m3u8 只检查清单并跟随少量当前分片，明确用外部 curl 拉取，不是 MsMedia 原生网络或 HLS。

### 未做 / 未覆盖（事件与其它）

- 不增加 `timeline_epoch` 来表示 CC/TEI/adaptation 不连续（合同：仅 seek 类刷新才增加）
- 未单独检测「仅 PCR/PTS 跳变、但 CC 连续且无 discontinuity_indicator」
- 未实现 RTP/网络乱序重排（按间隙处理）
- `random_access_indicator`、私有适配字段未用
- evq 入队/出队按最低空闲下标，严格 FIFO 未改
- 实流 A/B 中途截取为何在少量 AAC 后变为 UNSUPPORTED、视频 0 AU，未在本轮定因
- 无 el7/docker/四平台安装，无真实硬解查询/出画
- 许可证等 P0 待确认项仍在
- 在 P1 正式通过之前，不进入 P2；不扩展其它 codec/容器/原生网络栈

## 2026-11-19（北京时间）P1i — 中途接入安全恢复与 evq 严格 FIFO

P1i：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。优先修复已暴露的两项正确性问题，不扩格式/网络。无 CloudAgent、machineId、commit/push。**不将 P1 写为通过，不进入 P2。不将开发解析阶段写成全部完成。不因缺少四平台/硬解而阻塞本轮解析正确性修复。**

### 1) 中途接入 H.264：等待 SPS/PPS/IDR，而不是会话级 UNSUPPORTED

用 gitignore 下已保存的实流 A/B/C（第二次抽样，约 17:30 CST，未再重采）对比 NAL 与组帧结果，证据写在 `build/dev-samples/live/analysis/nal_compare.txt`（不入库，不含 URL）。

可复现事实（同节目：H.264 High 1280×720 逐行 25 fps PID 0x100，AAC LC 44.1 kHz PID 0x101）：

- A/B 第一枚 NAL 是 AUD，随后 SEI+非 IDR；首枚 SPS/PPS/IDR 在 NAL 下标 302/303/306。A 有 93 枚 VCL 出现在首枚 SPS 之前（B 同样 93）。这是直播中途 GOP 接入，不是 High@720p 语法本身超出子集。
- C 三个独立分片以 AUD+SPS+PPS+SEI+IDR 开头，VCL-before-SPS=0。修复前后都是 status=ok。
- 修复前：`emit_au` 在 AUD 结束一幅尚无 SPS/PPS 的图像时把整个会话标为 `MS_ERR_UNSUPPORTED`，后续关键帧走不到，于是视频 AU=0、只剩开头几帧 AAC，inspect 退出码 1。
- 这与「截取残缺」（尾部不足一包）和「真正不支持语法」（FMO、隔行、NAL 2–4 或 ≥14）要分开：前者在到达 IDR 之后可以安全恢复；后者仍必须失败。

实现（`src/codec/ms_h264.c` / `ms_h264.h`）：

- 尚无 SPS/PPS，或尚未成功发出过 IDR（`started==0` 且本单元不是 IDR）时，丢弃该单元并继续，不把残片当 AU 发出，也不把会话标为失败。
- 条带引用尚未到达的参数集：`parse_slice_header` 返回 2（等待），不再当作 `MALFORMED`。
- 主图像之后的 AUD/SPS/PPS/SEI 先结束上一单元，避免残片 P 条带与后面的 SPS/IDR 粘在同一 AU。
- `started` 在成功发出 IDR 后置位；`reset` 清零，`flush_partial` 保留（与已解析 SPS/PPS 一样）。
- 真正不支持的特性仍失败：FMO、`frame_mbs_only=0`、NAL type 2/3/4 或 ≥14；合成用例保留，并新增 IDR 后 typ 14 仍须 `UNSUPPORTED`。

修复后对同一份保存样本（debug `ms_ts_inspect`，约 17:42 CST）：

| 样本 | 体积 | inspect | 视频 AU | 对照 | 音频 AU | 对照 |
| --- | --- | --- | --- | --- | --- | --- |
| A | 1729483 | status=ok，退出 0 | 107，首 IDR PTS=66157200 | 与 ffprobe 自第一个 K 包起的 107 包一致（ffprobe 全段 200，含 SPS 前 93 包，不得强行对齐） | 342 | ffprobe 344 |
| B | 1747432 | status=ok，退出 0 | 108，首 IDR PTS=66157200 | 同口径 108 / 全段 201 | 344 | ffprobe 346 |
| C_s0/s1/s2 | 1092844 / 1070096 / 1026104 | status=ok，退出 0 | 125/125/125 | 与 ffprobe 包数完全一致 | 216/215/215 | 完全一致 |

A/B 音频比 ffprobe 各少 2 帧，更像有界截取尾部不完整 ADTS/PES，不是本轮中途接入误报。不得把 A/B 写成硬解或出画成功，只说明组帧从首个 IDR 起已能安全发出。

### 2) evq 严格 FIFO

原先 `evq_enqueue` 占用第一个空闲下标，`pull` 再按下标从小到大取待交付节点。释放 `evq[0]` 后新事件复用下标 0，会被下一轮 pull 抢到前面，一次累计超过 64 个事件时出队顺序错乱（P1h 曾把 44.1 kHz 余数用例缩到 16 帧躲避，这不是产品修复）。

本轮小步改法：空槽仍只作存储；交付按已有 `ev_gen` 取最旧的待交付节点（`int32_t` 差值处理 32 位回绕）。不改 `MS_PENDING_EVENT_MAX=64`、默认 `out_slots=8`、预算、所有权、epoch 合同（CC/传输不连续仍不增加 timeline/selection/session epoch；reset 才增加 session_epoch；flush 清空队列但不增加 session_epoch，也不清 `eos_in`）。

回归：`test_evq_fifo_backpressure_lifecycle` 覆盖超过 64 累计事件、188 字节交错 push-pull、先灌满再拉的背压（最先必须是 FORMAT，配置先于 AU，AU PTS 按入队顺序而非槽下标）、flush/reset 生命周期。`test_aac_remainder_intra_pes` 改回同一 PES 内 80 帧 44.1 kHz，并用交错 feed 排空，不能再靠缩短用例躲过 bug。

### 命令与结果（约 17:40–17:43 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `./build/debug/test_core`：原 29 组 + `test_h264_midstream_join_wait_idr` + `test_evq_fifo_backpressure_lifecycle`，合计 31 组，`all tests passed`，退出码 0，ASan/UBSan 无报错。
- `./build.sh release` 后 `./build/release/test_core`：同样 31 组通过，退出码 0。
- `tests/dev_crosscheck.sh`：预期样本全部与独立参照一致，退出码 0（concat 对照仍只记 NOTE，不是产品通过条件）。
- 保存的 A/B/C 样本按上一节重跑 inspect/ffprobe，未改远端、未把 URL 写入仓库文档。

预算与体积（release，库内目标 text+data）：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7637+16、`ms_ts` 17142+16、`ms_adts` 3402、`ms_h264` 11430，合计 **44350**（P1h 为 44114）。固定会话对象仍为 23064。双轨用例 `accounted_used_after_au=10555`、`accounted_peak=11073`、`alloc_live_peak=34233`、`flush_used=4376`、`reset_used=0`，`budget=65536`。debug：`libmsmedia.a` 1709026，`test_core` 2905440，`ms_ts_inspect` 1547784。release：`libmsmedia.a` 50676，`test_core` 162080，`ms_ts_inspect` 63848。

### 未做 / 未覆盖

- 开放 GOP 里参考是否齐全、恢复点、非 IDR 的 I 帧能否安全出画：仍未做完整判定；本轮只保证首个 IDR 之前与缺参数集的残片不会作为 AU 发出
- A/B 音频相对 ffprobe 各少 2 帧的截取尾部细节未单独拆包定因
- 不增加 `timeline_epoch` 表示 CC/TEI/adaptation 不连续
- 未单独检测「仅 PCR/PTS 跳变、但 CC 连续且无 discontinuity_indicator」
- 未实现 RTP/网络乱序重排；无原生网络/HLS 模块
- 无 el7/docker/四平台安装，无真实硬解查询/出画
- 许可证等 P0 待确认项仍在
- 在 P1 正式通过之前，不进入 P2；不扩展其它 codec/容器/原生网络栈

### 提交说明

用户于 2026-11-19 明确要求将本阶段成果提交到 GitHub。许可证仍未选定，不添加 LICENSE，也不把尚未选许可证当作本次提交的阻塞。公开仓库上的提交以实际历史为准，此处不预写 SHA。


