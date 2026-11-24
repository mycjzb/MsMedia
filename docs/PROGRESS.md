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

## 2026-11-20（北京时间）P1j — PMT 前完整 AAC 回放与残缺尾帧定因

P1j：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。不等待 GitHub 认证，不发起新登录，不自动 commit/push；本地提交 `1092bec`（P1a–P1i）保持不动。无 CloudAgent、machineId。**不将 P1 写为通过，不落地完整 P2。** 本轮音频诊断与修复已构成一个可验证小步，P2 共用箱体读取留待下一轮。

### 独立拆包结论（不是猜测）

对 gitignore 下已保存的实流 A/B/C（第二次抽样，约 17:30 CST 2026-11-19，未再重采）做独立 PES/ADTS 行走，不经过产品库。证据在 `build/dev-samples/live/analysis/aac_frame_diff.txt` 与本轮 `inspect_p1j.txt` / `ffprobe_aac_p1j.csv`（不入库，不含 URL）。

同节目：H.264 High 1280×720 逐行 25 fps PID 0x100，AAC LC 44.1 kHz 双声道 PID 0x101，PAT 0，PMT 0x1000。

| 事实 | A | B | C_s0（对照） |
| --- | --- | --- | --- |
| 文件尾不足整包 | 71 字节 | 160 字节 | 0 |
| 音频连续计数间隙 / TEI | 0 / 0 | 0 / 0 | 0 / 0 |
| 独立完整 ADTS 数 | 343 | 345 | 216 |
| 首个音频 PES | 偏移 1128，PUSI=1，PTS=65814873，载荷 356，ADTS flen=356，PES 完整；位于 PAT/PMT 之前（A：媒体包 0–8，PAT 包 9，PMT 包 10） | 同形态，首 PES 完整且在 PAT/PMT 前 | PSI 先于媒体，首 PTS 与 inspect/ffprobe 一致 |
| 末个音频 PES | 偏移 1729224，PUSI=1，PTS=66531673，载荷 168，声明 ADTS flen=376，缺 208，EOS 时仍开放 | 载荷 168，声明 357，缺 189，开放 | 末 PES 完整 |
| 修复前 inspect 音频 AU | 342，首 PTS=65816963（第二帧） | 344，首 PTS=65816963 | 216，与 ffprobe 一致 |
| ffprobe 音频包 | 344，含首完整帧与残缺尾包 size=168 | 346，同样含残缺尾包 size=168 | 216 |

根因有两处，不能合成「少 2 帧」一笔带过：

1. **产品缺陷（已修）**：`parse_one_packet` 在 `finish_pmt` 登记轨道之前丢弃未知 PID。A/B 的第一枚完整 AAC PES（A 为包 6–8）因此被丢，inspect 从第二帧（PTS 65816963）起计。
2. **截取残缺（正确不发出）**：末 PES 在文件结束时仍开放，ADTS 长度未收齐。`ms_adts_end` 丢弃尾部不完整帧。ffprobe 把该残包计为 packet（size=168）。不得为对齐 ffprobe 而把它当作完整 AU。

C 三个分片 PSI 在媒体之前，不触发缺陷 (1)；C_s0 末 PES 完整，故修复前后均与 ffprobe 一致。

### 最小修复

在 `src/container/ms_ts.c` / `ms_ts.h` 增加有界预 PMT 回放，复用已有 `parse_one_packet` / `announce_pending_tracks` / `emit_disc` / `ms_mem_alloc`：

- 尚无 PMT 且非回放中的未知 PID 载荷包，写入最多 `MS_TS_PRE_PMT_PACKETS=128` 的 188 字节环形缓冲（懒分配，计入会话动态预算；预算不足则放弃暂存，行为退回旧逻辑）
- 首次（或 PAT 版本变化后的新）PMT 登记轨道并宣布后，按序把暂存包再交给 `parse_one_packet`；回放中不再二次暂存
- 超 128 丢最旧；`clear_tracks_and_pes` / reset / destroy 释放；`flush_partial` 在尚未回放时丢弃暂存；PAT 版本变化清 `have_pmt` 时允许再次暂存
- 不改变「残缺 ADTS 不当 AU」的既有路径

合成回归：`test_aac_pes_before_pmt_replay`（完整 AAC PES 出现在 PAT/PMT 之前必须发出，首 PTS 为该 PES 的 PTS）；`test_aac_truncated_tail_not_au`（PES 长度对齐的短载荷，以及 `PES_packet_length=0` 且声明 ADTS 长于实有载荷，EOS 后都不得作为 AU）。

### 修复后保存样本（debug `ms_ts_inspect` + 系统 ffprobe 7.1.5，约 16:31 CST）

| 样本 | 体积 | inspect | 视频 AU | 音频 AU | 对照 |
| --- | --- | --- | --- | --- | --- |
| A | 1729483 | status=ok，退出 0 | 107，首 IDR PTS=66157200 | 343，首 PTS=65814873 size=356，末 PTS=66529584 size=385 | 完整 ADTS=343；ffprobe 包 344，其末包 PTS=66531673 size=168 为残缺，正确未发出 |
| B | 1747432 | status=ok，退出 0 | 108，首 IDR PTS=66157200 | 345，首 PTS=65814873 size=356，末 PTS=66533763 size=398 | 完整 ADTS=345；ffprobe 包 346，末包 PTS=66535853 size=168 同样不发出 |
| C_s0/s1/s2 | 1092844 / 1070096 / 1026104 | status=ok，退出 0 | 125/125/125 | 216/215/215，首末 PTS/size 与 ffprobe 一致 | 与修复前相同 |

不得把 A/B/C 写成硬解或出画成功。

### 命令与结果（约 16:29–16:31 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`：原 31 组 + 上述 2 组，合计 33 组，`all tests passed`，退出码 0，ASan/UBSan 无报错。
- `./build.sh release` 后 `./build/release/test_core`：同样 33 组通过，退出码 0。
- `tests/dev_crosscheck.sh --inspect build/debug/ms_ts_inspect --outdir build/dev-samples/crosscheck-p1j`：预期样本全部与独立参照一致，退出码 0（concat 对照仍只记 NOTE，不是产品通过条件）。
- 保存的 A/B/C 按上一节重跑 inspect/ffprobe，未改远端、未把 URL 写入仓库文档。

预算与体积（release，库内目标 text+data）：`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 7669+16、`ms_ts` 18342+16、`ms_adts` 3402、`ms_h264` 11430，合计 **45582**（P1i 为 44350）。固定会话对象 **23104**（P1i 为 23064，预 PMT 状态字段约 +40）。双轨用例 `accounted_used_after_au=10555`、`accounted_peak=11073`（该用例无 PMT 前媒体，回放后缓冲已释放，动态峰值与 P1i 相同）、`alloc_live_peak=34273`（+40）、`flush_used=4376`、`reset_used=0`，`budget=65536`。预 PMT 环形缓冲上限 128×188=24064 字节，仅在首次 PMT 前计入动态预算，回放后释放。debug：`libmsmedia.a` 1765442，`test_core` 2996888，`ms_ts_inspect` 1599952。release：`libmsmedia.a` 51988，`test_core` 166176，`ms_ts_inspect` 67944。

### 未做 / 未覆盖

- 正式 el7/四平台安装与真实硬解查询/出画仍未做；宿主自测不是正式验收
- 开放 GOP 参考是否齐全、非 IDR 的 I 帧能否安全出画：仍未做完整判定
- 预 PMT 暂存不识别 CAT/SDT/NIT 等其它 PSI；若此类包很多，128 包上界可能挤掉更早的媒体包（本保存样本 PMT 前不足 16 包，未触发）
- PMT 早于 PAT 到达时，该 PMT 仍可能按未知 PID 被暂存且要等到后续 PMT 才解析（与修复前「PMT 先到会被丢」同类边界，本轮未扩）
- 不增加 `timeline_epoch` 表示 CC/TEI/adaptation 不连续
- 未单独检测「仅 PCR/PTS 跳变、但 CC 连续且无 discontinuity_indicator」
- 未实现 RTP/网络乱序重排；无原生网络/HLS 模块
- FLV / MP4 / fMP4 / MOV、清单、seek：未实现。建议下一可执行小步为共用 ISO BMFF 箱体读取基础设施（size/type、extended size、有界递归、未知箱跳过），用真实样本收束，不一次做完全部格式，解析测试与硬解/正式平台验收单列
- 许可证等 P0 待确认项仍在
- 本地 `1092bec` 仍未出现在 GitHub `origin/main`（仍为 `d340ee9`）；本轮不提交、不新开登录

## 2026-11-20（北京时间）P2a — ISO BMFF 共用有界箱体基础

P2a：在 `/workspace/MsMedia` 继续用户已授权的 Bot 电脑本机编译自测（不算正式发版验收）。不等待 GitHub，不 commit/push，不登录，不碰用户电脑。保留本地 `1092bec` 与未提交的 P1j。**不将 P1 写为正式通过，不将 P2 写为全功能完成，不提前打最终源码包。** 本步只做箱体基础，不做 sample 表或音视频 AU。

### 本步范围（已实现）

- 公共枚举增加 `MS_FORMAT_MP4` / `MS_FORMAT_FMP4` / `MS_FORMAT_MOV`（不改变既有 TS 值）
- `src/container/ms_bmff.c`：ISO/QT 共用箱体解析。识别只看字节 0 的合法顶层箱头，不扫描四字节标签
- 处理 32/64 位 size、`size=0`（仅顶层，直到 EOF）、头截断等待、size 过小/溢出、子箱超出父箱、递归深度 16、箱数 4096
- 白名单容器递归；未知箱（含 QuickTime `udta` 里以 0xA9 开头的 © 类型）按声明大小跳过
- `mdat` 用 skip 计数消耗，确认后不把大载荷拷进会话输入窗（窗约 256 字节）；解析器对象懒分配并计入动态预算
- 会话在探测阶段先看 BMFF 头，否则仍走 TS；可发出一次 `FORMAT`，不发出 TRACK/AU
- 分类：`qt  ` 或无 ftyp 的 moov/mdat → MOV；`moof`/`styp`/`mvex` → fMP4（等 moov 走完再定，以免漏 mvex）；其余 ftyp → MP4。`heic`/`avif` 等静止图像品牌 → `UNSUPPORTED`
- 为后续留下 `ms_bmff_is_sample_table_type` / `ms_bmff_is_fragment_type` / `ms_bmff_is_container_type`，本步不解析其内容
- 开发工具 `ms_bmff_inspect`（不链 FFmpeg）；`tests/dev_bmff_crosscheck.sh` 并由 `tests/dev_crosscheck.sh` 调用

### 命令与结果（约 16:44–16:48 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`：原 33 组 + `test_bmff_box_walk_and_limits` + `test_bmff_session_format_no_au`，合计 35 组，`all tests passed`，退出码 0，ASan/UBSan 无报错
- `./build.sh release` 后 `./build/release/test_core`：同样 35 组通过，退出码 0
- `tests/dev_crosscheck.sh`（debug `ms_ts_inspect`，目录 `build/dev-samples/crosscheck-p2a`）：TS 预期样本与独立参照一致（concat 仍只记 NOTE）
- `tests/dev_bmff_crosscheck.sh`（系统 ffmpeg 7.1.5 lavfi 自编码，不入库）：`prog_faststart.mp4`（moov 在前，45 箱，mp4/isom）、`prog_moov_at_end.mp4`（moov 在尾，45 箱）、`frag_empty_moov.mp4`（fmp4/iso5，53 箱）、`classic.mov`（mov/qt，47 箱，含 ©swr）均与独立箱体行走一致；chunk=1/17/256/4096。只验证结构，不组帧

回归覆盖：正常树、1 字节碎片、64 位 size、size=0 顶层 mdat、头截断、过小 size、子箱越界、深度溢出、未知叶箱、中部 ftyp 不识别、QT/moof/mvex 分类、heic 拒绝、uuid、会话 FORMAT 且无 AU、3MiB mdat 跳过且峰值远小于载荷、mdat 后再见 moov、reset 后 TS 仍可用。

### 体积与内存（release，库内目标 text+data）

`ms_crc32` 144、`ms_util` 3778+8、`ms_registry` 777、`ms_session` 8878+16、`ms_ts` 18342+16、`ms_adts` 3402、`ms_h264` 11430、`ms_bmff` 7291，合计 **54082**（P1j 为 45582）。固定会话对象 **23120**（P1j 为 23104；仅增加 demux 判别与 BMFF 指针，解析器懒分配）。双轨 TS 用例 `accounted_used_after_au=10555`、`accounted_peak=11073` 与 P1j 相同，`alloc_live_peak=34289`（+16）、`flush_used=4376`、`reset_used=0`，`budget=65536`。debug：`libmsmedia.a` 2120572，`test_core` 3495440，`ms_ts_inspect` 1889488，`ms_bmff_inspect` 402872。release：`libmsmedia.a` 61384，`test_core` 190752，`ms_ts_inspect` 76136，`ms_bmff_inspect` 22824。

### 未做 / 未覆盖 / 与正式验收的分列

- **未实现（本步明确不做）**：sample 表与样本输出、moof/trun 组帧、FLV、seek/Source、清单/网络、软解、硬解出画
- **边界**：`size=0` 的 moov/moof/ftyp 本步 `UNSUPPORTED`（不把整文件当容器体缓存）；moov 在 `size=0` 的 mdat 之后按 ISO 语义不可达（需要确定的 mdat 大小或后续 seek）；`meta`/`stsd`/`dref` 等 FullBox 本步当叶箱跳过，不进子记录；探测仍要求已知顶层类型，以免把非 BMFF 误认
- 正式 el7/四平台安装与真实硬解查询/出画仍未做；宿主自测不是正式验收
- 许可证等 P0 待确认项仍在
- 本地 `1092bec` 仍未出现在 GitHub `origin/main`（仍为 `d340ee9`）；P1j 与 P2a 均未提交
- 用户要求「先完成所有功能再打包到本机测试」：本步完成后不打最终包。最终包应含源码、设计/进度、`build.sh`、样本生成/回归脚本、本机测试说明与未验清单，并排除真实流地址、音视频样本、密钥、`build/` 产物与 `.git`

### 本机开发自测说明（非正式验收）

在仓库根：`PATH=/usr/bin:/bin CC=/usr/bin/gcc ./build.sh debug && ./build/debug/test_core`，再 `./build.sh release && ./build/release/test_core`。可选安装系统 `ffmpeg`/`ffprobe` 后运行 `tests/dev_crosscheck.sh`。不要把这些命令的通过写成 el7/四平台/硬解通过。

### 下一个可执行小步（通向音视频 AU）

只做渐进式、**moov 位于 mdat 之前**的 MP4/MOV：解析 `stsd`（本子集 `avc1`/`avcC` 与 `mp4a`/`esds`）、`stts`/`stsc`/`stsz`/`stco`（及 `stss`），按表把随后到达的 `mdat` 切成样本并复用已有 H.264/AAC 组帧路径发出 TRACK/CONFIG/AU。MP4 音频通常是 length-prefixed raw AAC 而不是 ADTS，需单独说明与测试。`moov` 在尾或需要随机读 `mdat` 时本小步应返回能力不足，不静默吞数据。fMP4 `moof`/`trun` 与 FLV 另开小步。不得一次做完全部格式。


## 2026-11-20（北京时间）P2b — 渐进 MP4/MOV H.264/AAC 样本 AU

P2b：在 `/workspace/MsMedia` 继续用户已授权的 Bot 宿主编译自测（不算正式发版验收）。不 CloudAgent、不 machineId、不 commit/push、不登录、不碰用户电脑。保留本地 `1092bec` 与未提交的 P1j/P2a。**不将 P1 写为正式通过，不将 P2 写为全功能完成，不提前打最终源码包，不把 FFmpeg 链入产品，不做软解。**

### 本步范围（已实现）

- 公共 ABI：`ms_event` 末尾追加 `ms_sample_format`（旧字段偏移不变）。TS 路径填 `ANNEXB`/`ADTS`；本路径填 `AVCC`/`AAC_RAW`。新增 `ms_format_name` / `ms_sample_format_name`
- `src/container/ms_bmff_sample.c`：有界解析 `tkhd`/`mdhd`/`hdlr`/`stsd`（`avc1`+`avcC`，`mp4a`+`esds`，MOV 可经 `wave` 找 `esds`）、`stts`/`stsc`/`stsz`、`stco` 或 `co64`、`stss`、`ctts`（v0 无符号，v1 有符号）。表长度、乘法/累计溢出、描述符嵌套、跨表一致性、chunk 重叠与数据偏移均校验
- 配置先于 AU。样本按文件偏移（多轨 chunk 交织）的存储/解码顺序输出，不重排显示顺序。H.264 AU 保持 length-prefixed 并校验长度和；AAC 以 sample size 定界，不经 ADTS 组帧器。时间戳为媒体时间线 DTS 与 DTS+ctts；合成 PTS 为负则 `UNSUPPORTED`，**不把 elst 映射进 uint64，不伪造 PTS**
- 当前样本与表计入动态预算，mdat 只按需收取当前样本，不整文件缓存。复用公共 evq 严格 FIFO、epoch、flush/reset、反压与 allocator
- moov 在尾且样本偏移已错过：发出 FORMAT、TRACK、CONFIG 与 `MS_EVENT_UNSUPPORTED`，不输出 AU，会话不因此标失败
- fMP4 仍只做箱体/格式识别；空初始化表不当畸形；本步不解析 `moof`/`trun`，不输出分片 AU
- `stz2` 该轨不可用；`encv`/`enca` 或非本子集 AAC（非 LC / 显式频率等）明确拒绝或该轨不可用
- 回归：`test_bmff_progressive_h264_aac`（含 ctts、co64、MOV 品牌、逐字节推入、回压、截断尾样本不当 AU、超装配上限、moov 在尾、reset 后 TS）。开发工具：`ms_ts_inspect` 打印 sample_fmt/time_base；`tests/dev_bmff_au_crosscheck.sh` 由 `tests/dev_crosscheck.sh` 调用

### 命令与结果（约 16:57–17:10 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`：原 35 组 + `test_bmff_progressive_h264_aac`，合计 **36** 组，`all tests passed`，退出码 0，ASan/UBSan 无报错
- `./build.sh release` 后 `./build/release/test_core`：同样 36 组通过，退出码 0
- `tests/dev_bmff_crosscheck.sh`：`prog_faststart.mp4` / `prog_moov_at_end.mp4` / `frag_empty_moov.mp4` / `classic.mov` 箱体行走仍与独立参照一致
- `tests/dev_bmff_au_crosscheck.sh`（系统 ffmpeg 7.1.5 lavfi 自编码，不入库，不链产品库）：`prog_baseline.mp4`（73 AU）、`prog_bframes.mp4`（73 AU，含 B 帧/ctts）、`prog_baseline.mov`（70 AU，含 wave/esds 音频）、`prog_moov_at_end.mp4`（无 AU，有 UNSUPPORTED）。chunk=1/17/512/整文件。对照独立 sample 表的轨道、avcC/ASC、完整 payload sha256、PTS/DTS、keyframe、sample_fmt。ffprobe 包数量与解码顺序一致；其 PTS 相对媒体时间线常见恒定 elst 偏移（例如 B 帧样本为 -1024），本步有意不改写，不是对照失败

### 体积与内存（release，库内目标 text+data）

`ms_crc32` 144、`ms_util` 4173+8、`ms_registry` 777、`ms_session` 10774+32、`ms_ts` 18342+16、`ms_adts` 3402、`ms_h264` 11430、`ms_bmff` 8619、`ms_bmff_sample` 13646，合计 **71363**（P2a 为 54082）。固定会话对象 **23640**（P2a 为 23120；主要来自 evq 内嵌 `ms_event` 末尾追加 `sample_format`，约 +520）。双轨 TS 用例 `accounted_used_after_au=10555`、`accounted_peak=11073` 与 P2a 相同，`alloc_live_peak=34809`（+520）、`flush_used=4376`、`reset_used=0`，`budget=65536`。debug：`libmsmedia.a` 2853256，`test_core` 4299288，`ms_ts_inspect` 2492832，`ms_bmff_inspect` 966800。release：`libmsmedia.a` 79626，`test_core` 223520，`ms_ts_inspect` 92512，`ms_bmff_inspect` 43304。

### 未做 / 未覆盖 / 与正式验收的分列

- **未实现（本步明确不做）**：moov 在尾的回看/size+range、任意时间 seek、fMP4 `moof`/`trun` 组帧、FLV、Source、清单/网络、软解、硬解出画、把 avcC 改写成 Annex B 或给裸 AAC 加 ADTS 来冒充 TS 合同
- **边界**：时间戳使用媒体时间线，不应用 elst；`stz2` 忽略该轨；非 `avc1`/`mp4a` 或非 LC ASC 的轨不可用；单样本超过 `max_assembly_bytes` 为 `MALFORMED`；截断样本不当完整 AU；最多 8 轨
- 正式 el7/四平台安装与真实硬解查询/出画仍未做；宿主自测不是正式验收
- 许可证等 P0 待确认项仍在
- 本地 `1092bec` 仍未出现在 GitHub `origin/main`；P1j、P2a、P2b 均未提交
- 用户要求「先完成所有功能再打包到本机测试」：本步完成后不打最终包

### 本机开发自测说明（非正式验收）

在仓库根：`PATH=/usr/bin:/bin CC=/usr/bin/gcc ./build.sh debug && ./build/debug/test_core`，再 `./build.sh release && ./build/release/test_core`。可选安装系统 `ffmpeg`/`ffprobe` 后运行 `tests/dev_crosscheck.sh`（含箱体与渐进 AU 对照）。不要把这些命令的通过写成 el7/四平台/硬解通过。

### 下一个可执行小步

为 **moov 位于 mdat 之后** 的渐进 MP4/MOV 做有界回看（或明确的 size/range 子集），让已解析的 sample 表能输出与本步相同合同的 H.264/AAC AU；失败则继续明确 `NOT_SEEKABLE`/`UNSUPPORTED`，不得静默丢样本。fMP4 `moof`/`trun` 与 FLV 仍另开小步。不得一次做完全部格式。


## 2026-11-20（北京时间）P2c — moov 在尾的有界回看

P2c：在 `/workspace/MsMedia` 继续用户已授权的 Bot 宿主编译自测（不算正式发版验收）。不 CloudAgent、不 machineId、不 commit/push、不登录、不碰用户电脑。保留本地 `1092bec` 与未提交的 P1j/P2a/P2b。**不将 P1 写为正式通过，不将 P2 写为全功能完成，不提前打最终源码包，不把 FFmpeg 链入产品，不做软解。** 用户补充的更广对照清单只在设计/路线中列为「已定核心」与「待评估纳入」，本轮不扩那些新编码/封装/协议/广播/HDR 的实现，不宣称全支持。

### 本步范围（已实现）

- 公共 ABI：`ms_session_limits` 末尾追加 `max_lookback_bytes`（旧字段偏移不变）。默认 8 MiB，绝对上限 16 MiB，仅可下调；`struct_size=0` 的旧布局仍取默认。该窗计入 `session_budget_bytes`
- moov 位于确定大小的 mdat 之后、且不是 fMP4 时：若声明载荷不超过回看上限且分配成功，则把该 mdat 载荷保留在有界窗内；表定稿后若全部已错过样本都完整落在窗内，按绝对偏移收获，合同与 P2b 相同（`AVCC`/`AAC_RAW`，配置先于 AU，媒体时间线 DTS 与 DTS+ctts，不应用 elst）
- 声明大小超过上限、`size=0`（直到 EOF）的 mdat、或分配失败：不缓存该 mdat，不把解析卡在 `LIMIT`，更不整文件缓存。随后若样本表仍需要这些字节，`push`/`end_of_input` 返回 `MS_ERR_NOT_SEEKABLE`，投递 `MS_EVENT_UNSUPPORTED`，会话不因此失败，FORMAT/TRACK/CONFIG 仍可取出，不输出部分 AU
- 回看成功后释放该窗。后续若还有 moov 之后的 mdat，仍走 P2b 的按偏移收获。fMP4 见到 `moof`/`mvex`/`styp` 时丢弃回看窗，本步仍不输出分片 AU
- 回归扩展原 36 组中的 `test_bmff_progressive_h264_aac`：默认上限下 moov 在尾的 MP4 逐字节与 MOV 碎片推送输出与 moov 在前相同的 payload/PTS/DTS/keyframe/config；128 KiB 填充 + `max_lookback_bytes=4096` 明确 `NOT_SEEKABLE`、无 AU、峰值低于该 mdat。P2a 的 3 MiB dummy 跳过与低峰值仍通过

### 命令与结果（约 17:12–17:22 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`：仍为 **36** 组，`all tests passed`，退出码 0，ASan/UBSan 无报错
- `./build.sh release` 后 `./build/release/test_core`：同样 36 组通过，退出码 0
- `tests/dev_bmff_crosscheck.sh`：`prog_faststart.mp4` / `prog_moov_at_end.mp4` / `frag_empty_moov.mp4` / `classic.mov` 箱体行走仍与独立参照一致
- `tests/dev_bmff_au_crosscheck.sh`（系统 ffmpeg 7.1.5 lavfi 自编码，不入库，不链产品库）：`prog_baseline.mp4`（73 AU）、`prog_bframes.mp4`（73 AU）、`prog_baseline.mov`（70 AU）、`prog_moov_at_end.mp4`（73 AU，与独立表一致，无 UNSUPPORTED）、`prog_moov_at_end.mov`（70 AU）。chunk=1/17/512/整文件。对照独立 sample 表的轨道、avcC/ASC、完整 payload sha256、PTS/DTS、keyframe、sample_fmt。ffprobe 包数量与解码顺序一致；其 PTS 相对媒体时间线常见恒定 elst 偏移，本步有意不改写

### 体积与内存（release，库内目标 text+data）

`ms_crc32` 144、`ms_util` 4173+8、`ms_registry` 777、`ms_session` 11022+32、`ms_ts` 18342+16、`ms_adts` 3402、`ms_h264` 11430、`ms_bmff` 8923、`ms_bmff_sample` 16102，合计 **74371**（P2b 为 71363）。固定会话对象 **23648**（P2b 为 23640；`ms_session_limits` 追加 `max_lookback_bytes`）。双轨 TS 用例 `accounted_used_after_au=10555`、`accounted_peak=11073` 与 P2b 相同，`alloc_live_peak=34817`（+8）、`flush_used=4376`、`reset_used=0`，`budget=65536`。debug：`libmsmedia.a` 2960122，`test_core` 4433584，`ms_ts_inspect` 2582840，`ms_bmff_inspect` 1055024。release：`libmsmedia.a` 82634，`test_core` 231712，`ms_ts_inspect` 96608，`ms_bmff_inspect` 43304。

### 未做 / 未覆盖 / 与正式验收的分列

- **未实现（本步明确不做）**：fMP4 `moof`/`trun`/`tfdt`/`tfhd`/`trex` 组帧、FLV、任意时间 seek、通用 Source size+range 随机读、清单/网络、软解、硬解出画、把 avcC 改写成 Annex B 或给裸 AAC 加 ADTS 来冒充 TS 合同、对照清单中待评估的新编码/封装/协议/广播/HDR
- **边界**：回看只覆盖 moov 定稿前已见且尺寸合规的 mdat；多段不连续且第一窗覆盖不全则报错；`size=0` mdat 不回看；超过上限或预算不缓存；不完整覆盖不输出部分 AU；时间戳仍不应用 elst；最多 8 轨
- 正式 el7/四平台安装与真实硬解查询/出画仍未做；宿主自测不是正式验收
- 许可证等 P0 待确认项仍在
- 本地 `1092bec` 仍未出现在 GitHub `origin/main`；P1j、P2a、P2b、P2c 均未提交
- 用户要求「先完成所有功能再打包到本机测试」：本步完成后不打最终包

### 本机开发自测说明（非正式验收）

在仓库根：`PATH=/usr/bin:/bin CC=/usr/bin/gcc ./build.sh debug && ./build/debug/test_core`，再 `./build.sh release && ./build/release/test_core`。可选安装系统 `ffmpeg`/`ffprobe` 后运行 `tests/dev_crosscheck.sh`（含箱体与渐进 AU 对照，现包括 moov-at-end 的 MP4/MOV）。不要把这些命令的通过写成 el7/四平台/硬解通过。

### 下一个可执行小步

只做 **fMP4** 的 `moof`/`trun`（及必要的 `tfdt`/`tfhd`/`trex`）组帧，使分片文件能输出与渐进路径合同清楚的 H.264/AAC AU；失败则明确报错。不在本步实现 FLV 或任意时间 seek。不得一次做完全部格式。


## 2026-11-20（北京时间）P2d — fMP4 init + moof/mdat 的 H.264/AAC AU

P2d：在 `/workspace/MsMedia` 继续用户已授权的 Bot 宿主编译自测（不算正式发版验收）。不 CloudAgent、不 machineId、不 commit/push、不登录、不碰用户电脑。保留本地 `1092bec` 与未提交的 P1j/P2a/P2b/P2c。**不将 P1 写为正式通过，不将 P2 写为全功能完成，不提前打最终源码包，不把 FFmpeg 链入产品，不做软解。** 用户补充的更广对照清单只在设计/路线中列为「已定核心」与「待评估纳入」，本轮不扩那些新编码/封装/协议/广播/HDR 的实现，不宣称全支持。

### 本步范围（已实现）

- 复用既有箱体遍历、样本装配缓冲、事件队列与会话预算。init（`ftyp`/`styp` + 含 `mvex`/`trex` 的 moov，`avc1`/`avcC` 与/或 `mp4a`/`esds`，空 sample 表允许）须先于首个可用 moof；随后顺序解析 moof/mdat
- `trex`/`tfhd`/`tfdt`/`trun`：缺省继承、flag、有符号 `data_offset`、`default-base-is-moof` 与隐式基址（第一 traf 为 moof 起点，后续为上一轨数据尾）、duration/size/flags、composition offset v0 无符号 / v1 有符号、first-sample-flags、`sample_is_non_sync`、多个 traf/trun、跨片轨配置与 `tfdt`/续接时间线
- 载荷合同与 P2b 相同：`MS_SAMPLE_FMT_AVCC` / `MS_SAMPLE_FMT_AAC_RAW`，配置先于 AU，PTS = DTS + cto，解码/存储序，不应用 elst，不把 B 帧显示序当解码序
- 每片定稿后释放当前分片样本表（上限 4096），不整文件缓存、无界索引。增量碎片、反压、flush/reset/EOS、尺寸/次数/算术溢出与预算按既有模块处理
- 错误合同：moof 在 moov 前不缓存媒体，到达 moov 后 `MS_ERR_NOT_SEEKABLE` + `UNSUPPORTED`、0 AU；`senc`/`saiz`/`saio`（即便在 trun 之后）丢弃该 traf 样本并投递 `UNSUPPORTED`，不把密文当帧；未知轨/描述不一致跳过该 traf；缺 init 等待而不吐假帧；截断中途样本不输出残缺 AU；size 0 为 `MALFORMED`；溢出为 `UNSUPPORTED`/`OVERFLOW`
- 可选：某片 mdat 若在其 moof 之前且仍被 P2c 回看窗覆盖，可按绝对偏移收获。hybrid moov 样本表 + `mvex` 时忽略 moov 表、只走 moof。`sidx` 当叶子跳过
- 新增第 37 组 `test_bmff_fragmented_h264_aac`：单片 default-base-is-moof 逐字节，合同与 P2b 相同；两片 + 无 default-base-is-moof（第二 traf `data_offset=0` 续接）碎片推送；moof 在 moov 前明确 `NOT_SEEKABLE`、无 AU；trun 后加 senc 无假帧；截断最后样本不当完整 AU；128 KiB mdat 填充峰值低于该填充；reset 后仍能跑 TS。原 36 组保持通过，渐进 MP4/MOV 与 TS 未被破坏

### 命令与结果（约 17:35–17:40 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc`，`/usr/bin/gcc` 14.2.0。

- `./build.sh debug` 后 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/debug/test_core`：**37** 组，`all tests passed`，退出码 0，ASan/UBSan 无报错
- `./build.sh release` 后 `./build/release/test_core`：同样 37 组通过，退出码 0
- `tests/dev_crosscheck.sh`（含既有 TS 对照、`dev_bmff_crosscheck.sh` 箱体、`dev_bmff_au_crosscheck.sh`）：全部预期样本通过。箱体仍为 `prog_faststart.mp4` / `prog_moov_at_end.mp4` / `frag_empty_moov.mp4` / `classic.mov` 与独立参照一致
- AU 对照（系统 ffmpeg 7.1.5 lavfi 自编码，不入库，不链产品库）：原有渐进/回看 5 个文件保持 73/73/70/73/70 AU。新增 `fmp4_baseline.mp4`（3 个 moof/mdat，6 traf/trun，73 AU）、`fmp4_multifrag.mp4`（5 片，10 traf/trun，73 AU）、`fmp4_bframes.mp4`（双轨 + B 帧 composition offset，3 片，73 AU）。chunk=1/17/512/整文件。对照独立 moof/trun 表的轨道、avcC/ASC、完整 payload sha256、PTS/DTS、keyframe、sample_fmt。这三份 fMP4 的 ffprobe PTS/DTS 与媒体时间线一致（无 elst 偏移）

### 体积与内存（release，库内目标 text+data）

`ms_crc32` 144、`ms_util` 4173+8、`ms_registry` 777、`ms_session` 11022+32、`ms_ts` 18342+16、`ms_adts` 3402、`ms_h264` 11430、`ms_bmff` 9115、`ms_bmff_sample` 22206，合计 **80667**（P2c 为 74371）。固定会话对象仍为 **23648**。双轨 TS 用例 `accounted_used_after_au=10555`、`accounted_peak=11073` 与 P2c 相同，`alloc_live_peak=34817`、`flush_used=4376`、`reset_used=0`，`budget=65536`。debug：`libmsmedia.a` 3371474，`test_core` 4953760，`ms_ts_inspect` 2934040，`ms_bmff_inspect` 1402104。release：`libmsmedia.a` 89010，`test_core` 248104，`ms_ts_inspect` 104808，`ms_bmff_inspect` 51504。合成 128 KiB 填充 mdat 的会话峰值仍远低于该填充。

### 未做 / 未覆盖 / 与正式验收的分列

- **未实现（本步明确不做）**：FLV、任意时间 seek、通用 Source size+range 随机读、清单/网络、CMAF 全集、解密（只识别并拒绝密文样本）、软解、硬解出画、把 avcC 改写成 Annex B 或给裸 AAC 加 ADTS 来冒充 TS 合同、对照清单中待评估的新编码/封装/协议/广播/HDR
- **边界**：只覆盖能真实输出帧的 init+顺序 moof/mdat 子集；不支持把 moof 置于 moov 之前仍出 AU；不解密；不维护无界碎片索引；时间戳仍不应用 elst；负 PTS 为 `UNSUPPORTED`；最多 8 轨；单片最多 4096 个分片样本
- **四平台硬解未验证**：Android / Windows / Linux / iOS 的硬解查询与出画均未做；宿主组帧自测不是硬解成功，也不是正式 el7 验收
- 许可证等 P0 待确认项仍在
- 本地 `1092bec` 仍未出现在 GitHub `origin/main`；P1j、P2a、P2b、P2c、P2d 均未提交
- 用户要求「先完成所有功能再打包到本机测试」：本步完成后不打最终包

### 本机开发自测说明（非正式验收）

在仓库根：`PATH=/usr/bin:/bin CC=/usr/bin/gcc ./build.sh debug && ./build/debug/test_core`，再 `./build.sh release && ./build/release/test_core`。可选安装系统 `ffmpeg`/`ffprobe` 后运行 `tests/dev_crosscheck.sh`（含箱体与渐进/fMP4 AU 对照）。不要把这些命令的通过写成 el7/四平台/硬解通过。

### 下一个可执行小步

FLV 标签组帧（AVC/AAC sequence header 与完整 tag 载荷），或任意时间 seek / Source size+range。二者都另开小步。不得一次做完全部格式。


## 2026-11-20（北京时间）release 共享库产物（P2d 基线）

在 `/workspace/MsMedia` 按用户询问「编译后的 so 去调试符号后有多大」补真实共享库构建与测量。不 CloudAgent、不 machineId、不 commit/push。宿主机自测，非正式发版。FLV 解封装小步此时尚未合入编译基线，本产物只含已完成的 P2d。

### 构建方式

`build.sh` 在原有 `libmsmedia.a` 之外，用独立 `obj-pic/` 以 `-fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED` 重编同源文件，再 `-shared -Wl,-soname,libmsmedia.so.1 -Wl,--version-script=src/core/libmsmedia.map -Wl,-z,defs` 链接为 `libmsmedia.so.1.0.0`，并建立 `libmsmedia.so.1`、`libmsmedia.so` 符号链接。不是把 `.a` 改后缀。公共头对共享构建为公共函数加上 `MSMEDIA_API` default 可见性；版本脚本只导出 `msmedia.h` 中的 23 个 ABI 函数。链接行只有 `libc`，不出现 FFmpeg、OpenSSL 或其它网络/解码库。`test_core` / inspect 仍链 `.a`，测试未关闭。不对 `.a` / `.so` 做 UPX。debug 共享库保留调试符号；release 使用 `/usr/bin/strip --strip-unneeded`。

### 命令与结果（约 17:51 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc STRIP=/usr/bin/strip`，gcc 14.2.0，GNU strip/readelf（Binutils，`/usr/bin`）。

- `./build.sh debug` 后 ASan/UBSan `test_core`：**37** 组 `all tests passed`。debug `libmsmedia.a` 仍为 3371474；debug 共享库未 strip，`libmsmedia.so.1.0.0` 为 265368 字节（含 `-g`，且未链入 sanitizer）
- `./build.sh release` 后 `test_core`：同样 37 组通过。`libmsmedia.a` 仍为 **89010** 字节（与 P2d 17:39 测量一致）。共享库 strip 前 101352 字节，`--strip-unneeded` 之后 **92272** 字节
- `readelf -h`：ELF64、little endian、System V、Type=DYN（Shared object file）、Machine=Advanced Micro Devices X86-64
- `file`：`ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked, BuildID[sha1]=47dc5982d138e0b14b54fb270394b64fcb7dac65, stripped`；`libmsmedia.a` 仍为 `current ar archive`
- `readelf -d` / `ldd`：`NEEDED` 仅 `libc.so.6`，`SONAME` 为 `libmsmedia.so.1`；`ldd` 另见 `linux-vdso.so.1` 与 `/lib64/ld-linux-x86-64.so.2`（动态装载器），无 libssl、libcrypto、libav* 或 libpthread
- 版本依赖：libc 的 GLIBC_2.2.5 与 GLIBC_2.14（`memcpy`）
- 动态导出：23 个已定义 FUNC，均为 `MSMEDIA_1.0` 公共 ABI；内部 `ms_*` 未进入动态符号表。strip 后无 `.debug_*` 节
- `size`（strip 后 .so）：text 84842、data 704、bss 8，合计 85554
- `ms_ts_inspect` 104808、`ms_bmff_inspect` 51504、`test_core` 248104，与 P2d release 一致

### 边界

- 这是 Bot 宿主 **Linux x86-64** 自测 `.so`，不能写成 Android NDK ABI（armeabi-v7a / arm64-v8a / x86 / x86_64 Android）或 iOS / Windows 结果，也不能代替 el7 正式发版包
- 能力边界仍是 P2d：MPEG-TS 与渐进/有界回看 MP4/MOV、fMP4 init+moof/mdat 的 H.264/AAC AU。无 FLV 解封装、无 Source/seek、无清单/网络、无软解、无四平台硬解验证
- 不把 AU 输出写成可播放或硬解成功

### 下一个可执行小步

继续经典 FLV（AVC/AAC）解封装真实出帧；完成后再单独报告体积（届时 .so 会因新代码而变化）。任意时间 seek / Source size+range 以及协议清单仍另步，不得宣称完成。

## 2026-11-20（北京时间）P2e 经典 FLV H.264/AAC AU

在 `/workspace/MsMedia` 续做经典 FLV 真实出帧小步，并按用户新增要求：本轮不推翻已有 ownership/queue 合同，出帧回归完成后在 DESIGN/ROADMAP 记录数据路径拷贝现状与下一步可选零拷贝设计。不 CloudAgent、不 machineId、不 commit/push、不打最终包。宿主机自测，非正式发版。HEAD 仍为本地 `1092bec`。

### 已实现（宿主自测，非正式验收）

- 新增 `MS_FORMAT_FLV`。探测优先于 BMFF/TS。会话 demux=3，懒分配 `ms_flv_parser`（`ms_flv_parser_size()`），确认后输入窗约 256 字节；媒体 tag 可直接喂解析器。
- 经典 FLV 子集：`"FLV"` + version 1，flags 仅 A/V 位，DataOffset 9..4096；校验 prevTagSize（首个 0）与 StreamID=0。tag 8/9/18；脚本/未知按声明大小跳过，不做 AMF、不以 dataSize 分配。
- AVC/AAC：复用 `src/codec/ms_avcc.c` 的 `ms_avcc_parse` / `ms_avcc_validate_sample` / `ms_asc_ok`。配置为完整 avcC / ASC，AU 为 `AVCC` / `AAC_RAW`。packet type 2（end-of-sequence）不是 AU。
- 32 位扩展毫秒 DTS，有符号 24 位合成时间，PTS=DTS+cto，`time_base` 1/1000。负 PTS 返回 `UNSUPPORTED`，不回绕。配置先于 AU；缺配置不吐 AU；配置变化发新 CONFIG 并在下一 AU 标 `format_changed`。轨道 id：视频 1、音频 2。
- 视频 `IsExHeader` 视为 Enhanced FLV，诚实拒绝，0 AU。非 AVC/AAC 同样拒绝。不做 RTMP/网络客户端。
- 反压：完整 tag 在 `ST_EMIT` 保留到入队成功；`ms_session_pull` 会 drain 并继续 `in_buf`；`end_of_input` 在 LIMIT 时不清空未消费输入。中途 EOS 丢弃半截 tag，不判畸形。flush/reset 后可再喂 TS（生命周期用例已覆盖）。
- 本轮**未**改 copy-push ABI：事件载荷仍拷入队列自有缓冲。DESIGN/ROADMAP 已专节记录逐段复制现状、已规避的 in_buf 冗余拷贝、以及下一步可选 retain/release/borrow/push_owned（须有测量，禁止裸指针悬空）。硬解零拷贝按平台能力分述，不把压缩输入与 GPU 零回读混淆，不宣称 MediaCodec 输入恒为零拷贝。

### 命令与结果（约 18:05 CST，Asia/Shanghai）

宿主：`PATH=/usr/bin:/bin CC=/usr/bin/gcc STRIP=/usr/bin/strip`，gcc 14.2.0。

- `./build.sh debug` 后 ASan/UBSan（`detect_leaks=1:halt_on_error=1`，UBSan `halt_on_error=1`）`test_core`：**38** 组 `all tests passed`（原 37 组 + `test_flv_h264_aac`）。`sizeof(ms_session)` 为 **23664**。debug `libmsmedia.a` 3686082；debug 共享库未 strip，`libmsmedia.so.1.0.0` 为 290480 字节（含 `-g`，**未**链入 sanitizer）。
- `./build.sh release` 后 `test_core`：同样 38 组通过。`libmsmedia.a` **98458** 字节。共享库 strip 前 114480 字节，`/usr/bin/strip --strip-unneeded` 之后 **104560** 字节。
- release 目标文件 text+data 合计 **88413**（P2d 为 80667）。其中 `ms_flv` 6021、`ms_avcc` 589、`ms_session` 12526、`ms_bmff_sample` 21846、`ms_bmff` 9115、`ms_ts` 18358。
- 共享库：ELF64 LSB 共享对象、x86-64、System V，soname `libmsmedia.so.1`，`NEEDED` 仅 `libc.so.6`（GLIBC_2.2.5 与 GLIBC_2.14），导出 23 个 `MSMEDIA_1.0` FUNC。BuildID `f921c865250be5abd2e27ffd830dbd00614f7448`。`size`：text 92769、data 720、bss 8，合计 93497。不链接 FFmpeg 或 OpenSSL。不对 `.a`/`.so` 做 UPX。
- `ms_so_abi_smoke` 实际链接本目录 `libmsmedia.so`（rpath `$ORIGIN`），debug/release 均不加 sanitizer，动态依赖为 `libmsmedia.so.1` + `libc.so.6`。冒烟走过一条与静态路径一致的短 FLV（FORMAT+TRACK+CONFIG+1 AU）并解析全部 23 个公共符号。**sanitizer 覆盖以链 `.a` 的 `test_core` 为准，不能把未加 sanitizer 的 `.so` 冒烟说成已覆盖。**
- `tests/dev_crosscheck.sh`（release `ms_ts_inspect`）：原有 TS、BMFF 箱体、渐进/fMP4 AU 对照仍通过；新增经典 FLV 对照（独立 tag 表 + inspect + ffprobe，chunk=1/17/512/0）全部通过：
  - `flv_video_only.flv`：25 AU + 1 cfg，PTS/DTS/关键帧与 ffprobe 一致
  - `flv_audio_only.flv`：48 AU + 1 cfg，同上
  - `flv_baseline.flv`：73 AU + 2 cfg，同上
  - `flv_bframes.flv`：73 AU + 2 cfg（含有符号合成时间），同上
- 单元用例还覆盖：逐字节双轨合同、B 帧 cto、负 cto 不回绕且 0 AU、坏 prevTagSize、Enhanced IsExHeader、缺配置、截断残片、128 KiB 脚本垫片峰值远小于垫片、超长媒体 tag、end-of-sequence 不是 AU、反压后仍 4 AU、reset 后改喂 TS。
- release 其它产物：`ms_ts_inspect` 108904、`ms_bmff_inspect` 51504、`test_core` 268584、`ms_so_abi_smoke` 14688。

这是 Bot 宿主 **Linux x86-64** 自测，不能写成 Android NDK ABI 或 el7/四平台正式包。

### 边界与未做

- 只覆盖经典 FLV 的 AVC/AAC 顺序出帧子集；不是 Enhanced FLV、不是 HEVC、不是脚本关键帧索引、不是 RTMP/HTTP 取流、不是任意时间 seek / Source。
- 未改变对外 copy-push 所有权；未实现 retain/release/borrow/push_owned。不得把当前路径写成全链路零拷贝。
- 不是可播放、不是软解、不是硬解出画。四平台硬解（Android MediaCodec Surface、Windows MF/D3D11、iOS VideoToolbox/IOSurface、Linux VAAPI/V4L2/DMABUF）均未验证。
- 更广对照清单的新编码/协议项仍只是「已定核心 / 待评估纳入」，本步未评估纳入。
- 许可证等 P0 待确认项仍在。P1j 起至 P2e 的改动均未提交。用户要求完成功能后再打包：本步完成后不打最终包。

### 下一个可执行小步

优先做**有测量的零拷贝 buffer 所有权**小步（旧 ABI 兼容，copy 计数，寿命/反压/flush/reset 明确），然后再 Source/seek。不得一次做完全部格式或宣称硬解零拷贝已完成。


## 2026-11-20（北京时间）启动路径设计（FLV 收尾之后，未改 ABI）

经典 FLV（P2e）出帧回归、交叉验证、共享库测量与零拷贝专节已在同日 18:05 记录完成，本补充未推翻、未重开该实现。按用户要求，在下一缓冲所有权/零拷贝编码之前，查阅 Media3、FFmpeg、libVLC、mpv 的官方公开接口与许可证，把「尽快加载」写成可测设计，而不是空接口或固定毫秒承诺。

### 已记入文档（未实现新 ABI）

- `docs/DESIGN.md` 第 4 节新增「启动路径与快速加载」：来源与许可证表、结构/引用/有界探测/参数集与关键帧就绪/decoder submit/低延迟与稳态缓冲的借鉴边界；启动阶段为 首字节→`FORMAT`→`TRACK`→`CONFIG`→可解码关键 AU；复用 `consumed`、现有事件和 `ms_session_debug_budget`；硬解未实现时不虚造首画面或 submit。
- `docs/ROADMAP.md` 已写明实现顺序：先有测量的零拷贝所有权，启动统计可随该步或 Source/seek 落地，然后再 seek/Source。不引入 FFmpeg/VLC/mpv/Media3 依赖，不复制 GPL 代码。

### 未做

- 未改公共 ABI，未增加 `set_latency` 一类空接口，未实现零拷贝所有权，未做 Source/seek，未做硬解。
- 本补充没有新的性能基准数字（避免在无约定样本/机器/缓存条件时填写）。后续实测必须写明这些条件，且不承诺固定毫秒。
- 未 commit/push，未打最终包。HEAD 仍为本地 `1092bec`。


## 2026-11-20（北京时间）有测量的缓冲所有权与启动统计

在 P2e 与启动路径设计之后，落地可验证的安全子集：减少完整 AU 入队时的二次复制，保留旧 copy-push 入参 ABI，并给出可复现的复制计数与启动阶段观测。只在 Bot 宿主 `/workspace/MsMedia` 本机进行，未使用 CloudAgent，未登录，未 commit/push，未加新依赖，未做软解或最终包。

### 实现范围

- 入参仍是 copy-push：`ms_session_push` 只在调用期间借用调用方指针，不把该指针送进事件队列。
- 完整 AU 已在会话记账分配器的独占块中时，事件队列改为所有权移交（`owned_base` 必须是 `ms_mem_alloc` 返回的块首；`ev.data` 可为块内视图）。成功则解析器不得再释放；LIMIT/失败则不置 taken，所有权留在解析器。
- 适用路径：FLV 媒体 tag body（载荷在 5/2 字节头之后）、BMFF 整样本 `sbuf`、TS 侧 H.264/ADTS 在移交前若有剩余碎片则先把剩余拷到新块再移交（剩余拷贝计入 `copy_asm`）。
- 配置（avcC/ASC 等）仍拷贝，因其需粘滞复用。不完整组帧保留必要复制。
- 未做公开 `retain` / 借用调用方输入 / `push_owned`，未做共享引用计数。事件不能活过会话；`cancel` 不释放已 pull 事件，`destroy` 回收。有界预算不因移交丢失或重复计费。
- 公共 ABI：新增 `ms_session_get_stats`，调用方先填 `struct_size`；加入 `libmsmedia.map` 的 `MSMEDIA_1.0`。`MSMEDIA_ABI_VERSION` 仍为 1，旧调用不调用新函数即不受影响。内部 `ms_session_debug_set_force_copy` 仅测试对照。

### 测试与对照

- 原 38 组 + 新组 `test_copy_ownership_and_startup`：debug（ASan/UBSan）与 release 均通过。
- 新组用同一份合成经典 FLV（208 字节，2 视频 AU + 2 音频 AU + 2 份配置）：移交 `move_out_ops=4`、`copy_out_ops=2`（仅配置）；`force_copy` 对照 `move_out_ops=0`、`copy_out_ops=6`；`baseline_out_bytes` 两侧均为 84。逐字节与整文件的出队基线相同。覆盖输出槽反压、`au_queue_bytes` 上限 LIMIT 后缓解且回调一次、reset 清统计、cancel 后仍可 release 已 pull 事件、destroy 回收未取走项、`struct_size` 前缀、坏 prevTagSize。
- 全部既有 TS / 渐进 MP4/MOV / 回看 / fMP4 / 经典 FLV 交叉验证通过（`tests/dev_crosscheck.sh --inspect build/debug/ms_ts_inspect`）。
- `ms_so_abi_smoke` 实际链接 `.so`（debug 与 release；该程序不加 sanitizer）解析到 `ms_session_get_stats`，短 FLV 路径 `move_out_ops=1`、`copy_out_ops=1`。不得把该冒烟写成共享库已覆盖 ASan。

### 复制与启动观测（本机，不得外推）

条件：Bot 宿主 Linux x86-64，gcc 14.2.0（Debian 14.2.0-19），`PATH=/usr/bin:/bin`，debug inspect（ASan/UBSan），lavfi 样本即时生成不入库，页缓存为交叉验证刚写过的热缓存。墙钟未作为核心指标（外包测量会有调度噪声），不承诺固定毫秒。一次推入整文件时，各启动阶段的 `bytes_in` 会落在同一窗口。

| 样本 | 字节 | chunk | copy_in ops/bytes | copy_asm ops/bytes | copy_out ops/bytes | move_out ops/bytes | baseline_out | 启动阶段 bytes（format/track/config/key_au/video_key） |
| --- | ---: | ---: | --- | --- | --- | --- | ---: | --- |
| 合成 classic_flv（单测） | 208 | 0 | （计入组帧；单测打印 asm=133） | 133 | 2 / （配置） | 4 / 56 | 84 | 整文件窗口 |
| lavfi `flv_baseline.flv` | 11931 | 0 | 1/11931 | 78/10515 | 2/44 | 73/10194 | 10238 | 全部 @11931 |
| 同上 | 11931 | 1 | 3/3 | 10473/10515 | 2/44 | 73/10194 | 10238 | 9 / 375 / 375 / 559 / 3543 |
| lavfi `prog_baseline.mp4` | 15887 | 0 | 1/15887 | 73/13895 | 2/44 | 73/13895 | 13939 | 全部 @15887 |
| 同上 | 15887 | 1 | 8/8 | 13895/13895 | 2/44 | 73/13895 | 13939 | 1976 / 1976 / 1976 / 2129 / 5093 |
| lavfi `fmp4_baseline.mp4` | 20043 | 0 | 1/20043 | 73/17699 | 2/44 | 73/17699 | 17743 | 全部 @20043 |
| lavfi `prog_moov_at_end.mp4` | 15889 | 0 | 1/15889 | 74/27794 | 2/44 | 73/13897 | 13941 | 全部 @15889（回看窗使 asm 大于移交载荷） |
| lavfi `bl_aud1_s1_48k.ts` | 14664 | 0 | 1/14664 | 142/55186 | 2/38 | 63/8901 | 8939 | 全部 @14664 |
| 同上 | 14664 | 188 | 78/14664 | 138/54213 | 2/38 | 63/8901 | 8939 | 564 / 564 / 3948 / 3948 / 3948 |

说明：FLV/BMFF/TS 在本批 lavfi 样本上，73 或 63 个完整 AU 的出队载荷走了移交；配置仍拷贝。TS 的 `copy_asm` 明显高于出队载荷，因为 PES/AU 装配与剩余碎片仍要复制。moov 在尾的回看会把 mdat 先拷进回看窗再收获到样本缓冲，asm 接近两倍载荷。这不是全链路零拷贝。

### 体积与内存

同一宿主、同一编译器，`./build.sh release` 后 `strip --strip-unneeded`：

- `libmsmedia.so.1.0.0` strip 后 **108656** 字节（strip 前 119120）；P2e 对照为 104560 / 114480。
- `libmsmedia.a` **102978** 字节（P2e 为 98458）。
- BuildID `dbace36d1e2122d6af42859bc73d61181357363f`；NEEDED 仅 `libc.so.6`；导出 24 个 `MSMEDIA_1.0` FUNC；GLIBC_2.2.5 与 GLIBC_2.14（`memcpy`）。
- `size`：text 97420、data 720、bss 8，合计 98148。库内目标 text+data 约 92935（`ms_session` 14920，`ms_bmff_sample` 22174，`ms_ts` 18646，`ms_h264` 11878，`ms_bmff` 9155，`ms_flv` 6261，`ms_util` 4453，`ms_adts` 3938 等）。
- debug：`.a` 3899346，`.so` 未 strip 305680（无 sanitizer），`test_core` 5594296，`ms_ts_inspect` 3362608，`ms_so_abi_smoke` 27600。
- release：`test_core` 276776，`ms_ts_inspect` 113000，`ms_bmff_inspect` 51504，`ms_so_abi_smoke` 14696。
- 会话对象（不含动态预算）本机构造测得 **23816** 字节（P2e 为 23664），增加的是统计字段；槽位仍 96。合成 FLV 单测动态预算峰值移交路径 966、强制拷贝 986（样本很小，差值有限）。

宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做

- 未做公开跨会话 retain、借用调用方缓冲、`push_owned`，也未做共享引用的碎片零拷贝。
- 未做 Source / 任意时间 seek，未做硬解/软解/网络，未虚造首画面或 submit 统计，未增加空的 `set_latency`。
- 未宣称全平台全链路零拷贝。未 commit/push，HEAD 仍为本地 `1092bec`。未打最终包。

### 下一个可执行小步

继续 **Source / 任意时间 seek**，而不是再做一轮拷贝微优化。

## 2026-11-20 18:48 CST — Source 随机读 + 渐进 MP4/MOV 时间定位（宿主自测，非正式验收）

在所有权小步之后落地可验证子集，没有一次宣称任意格式全 seek。工作仅在 Bot 宿主 `/workspace/MsMedia`。未使用 CloudAgent / machineId，未 commit/push，HEAD 仍为本地 `1092bec`。未软解，未打最终包。硬解出画、四平台、el7 仍未验证。

### 已实现子集

- 公共 Source vtable：`struct_size`、`ctx`、`size_fn`、`read_at_fn`、可选 `cancel_fn`。`ms_session_set_source` 拷贝函数指针，不接管 `ctx`。调用方缓冲区仅在当次回调内有效。
- 短读允许；EOF 为 `*out_n=0` 且 `OK`；将阻塞则 `AGAIN` 且不吐字节。`ms_session_drive` 每次最多一次 `read_at`，不内部忙等或无限重试。回调不得重入任何会话 API，仅同一会话线程。`cancel` 会调用 `cancel_fn`。
- 开发适配器：`ms_mem_source_*`（可注入短读 / AGAIN / 失败）、`ms_file_source_*`（POSIX `open`/`pread`/`lseek`/`close`）。不是 HTTP/RTMP 客户端，核心无新网络依赖。文件大小用 `lseek`，避免宿主 libc 的 `fstat@GLIBC_2.33`。
- 旧 `push`/`pull` 兼容：未挂 Source 时尾 moov 仍走有界回看；回看超限仍为 `NOT_SEEKABLE` + `UNSUPPORTED`、会话不失败。
- 已挂可随机读且能给出确定大小的 Source 时：跳过（不缓存）当前 mdat 去读尾 moov，再按 `stco`/`co64` 用 `read_at` 取样本。证明：`max_lookback_bytes=16` 加 128 KiB mdat 填充时，无源路径仍不可回看，有源路径输出完整 AU，峰值远低于该填充，`bytes_skipped` 计入跳过的 mdat。
- 时间定位只针对已定稿、非分片的渐进 MP4/MOV。`time_90k` 是请求时间（90 kHz，与 PTS 同轴，不含 elst）。视频落到目标之前（含等于）最近的真实同步样本（`stss`；无 stss 时每样本可同步）；没有同步样本则 `NOT_SEEKABLE`，不伪造 IDR。若没有更早的关键帧，则落到第一个同步样本。超出时长落到最后一个同步样本。负时间为 `INVALID_ARG`，不改状态。
- 请求时间 vs 落点：`ms_session_stats` 增补 `has_seek_land` / `seek_request_90k` / `seek_land_90k`（`struct_size` 兼容）。视频落点之前、请求时间之前的视频 AU 置 `preroll=1`；音频从**请求时间**起输出，请求之前的音频裁剪与后续显示丢弃由控制层做。核心不修改用户解码器状态。
- 成功：先算落点再改游标，失败回滚；`timeline_epoch` 增加；flush 未取走队列（与 `flush` 相同，未 release 的 outstanding 指针失效）；重发 `CODEC_CONFIG`，首个随后 AU `format_changed=1`。反压下 `drive` 返回 `LIMIT`，pull/release 后可继续。
- 明确不做（返回 `NOT_SEEKABLE`，不是桩成功）：fMP4、经典 FLV、MPEG-TS 的任意时间定位；把字节偏移当成时间 seek 的接口也不提供。

### 测试与对照

- 新增第 40 组 `test_source_and_prog_seek`：内存源 + 本地文件、含 B 帧 ctts/stss 与尾 moov 的 MP4/MOV、多次前后 seek、边界与超范围、取消、慢短读、AGAIN、失败注入与恢复、反压、回调重入拒绝、fMP4 明确不可定位。样本 payload/PTS/DTS/关键帧与独立 mux 表一致。
- 原 39 组在 sanitizer debug 与 release 下回归通过，含生命周期与 copy 统计。
- 全部交叉脚本通过：`dev_crosscheck.sh`、`dev_bmff_crosscheck.sh`、`dev_bmff_au_crosscheck.sh`（`prog_moov_at_end.mp4/.mov` 增加 `--source --max-lookback 16`，AU 与独立表一致）、`dev_flv_au_crosscheck.sh`。`.so` ABI 冒烟（debug 无 sanitizer 与 release）通过。
- `ms_ts_inspect` 增加 `--source` 与 `--max-lookback`。驱动步数有界，避免 UI 无限等待。

### 体积与内存

同一宿主、同一编译器，`./build.sh release` 后 `strip --strip-unneeded`（2026-11-20 18:48 CST）：

- `libmsmedia.so.1.0.0` strip 后 **116896** 字节（strip 前 128664）；所有权小步对照为 108656 / 119120。
- `libmsmedia.a` **113982** 字节（所有权小步为 102978）。
- BuildID `5a26ad46fb7e095c330aebcc60347c27188b760f`；NEEDED 仅 `libc.so.6`；导出 31 个 `MSMEDIA_1.0` FUNC；GLIBC_2.2.5 与 GLIBC_2.14（`memcpy`）。
- `size`：text 108802、data 784、bss 8，合计 109594。库内目标 text+data 约 103111（`ms_bmff_sample` 26661，`ms_ts` 18646，`ms_session` 18051，`ms_h264` 11846，`ms_bmff` 10241，`ms_flv` 6277，`ms_util` 4589，`ms_adts` 3938，`ms_source` 约 1392 等）。
- debug：`.a` 4278458，`.so` 未 strip 331560（无 sanitizer），`test_core` 6069736，`ms_ts_inspect` 3729080，`ms_bmff_inspect` 1597944，`ms_so_abi_smoke` 33088。
- release：`test_core` 301408，`ms_ts_inspect` 129416，`ms_bmff_inspect` 55600，`ms_so_abi_smoke` 14744。
- 会话对象（不含动态预算）本机构造测得 **23920** 字节（所有权小步为 23816），增加的是 Source/seek 字段；槽位仍 96。带 128 KiB mdat 填充的尾 moov 随机读峰值低于 64 KiB。

宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做

- 未做 HTTP/RTMP/HLS 等网络 Source，未做 fMP4 / FLV / MPEG-TS 任意时间定位（明确 `NOT_SEEKABLE`）。
- 未做 elst 编辑列表换算、字幕队列清空（本子集无字幕轨）、硬解/软解出画。
- 未宣称全平台全链路零拷贝。未 commit/push，HEAD 仍为本地 `1092bec`。未打最终包。

### 下一个可执行小步（当时）

不要再为未实现的格式堆 seek 桩。随后已接续 P3 HLS media playlist 调度首步（见下节）。

## 2026-11-20 19:18 CST — P3 首步 HLS media playlist 调度（宿主自测，非正式验收）

Asia/Shanghai（UTC+8）。Bot `/workspace/MsMedia` 宿主自测。未 commit/push（HEAD 仍为本地 `1092bec`），无 CloudAgent / machineId / 新登录，无 FFmpeg 产品依赖，未打最终包。

### 已做

- 公共 ABI：`ms_hls_limits` / `create` / `destroy` / `bind` / `open` / `drive` / `cancel` / `ms_hls_resolve_uri`；内存与文件 fetch 开发适配器；`ms_session_signal_discontinuity`（flush 残缺装配、保留 evq、抬 `timeline_epoch`、入队 `DISCONTINUITY`）。符号仍在 `MSMEDIA_1.0`。
- 调度不是容器 demux，也不是 HTTP 客户端。`ms_hls_drive` 每次最多发起或读取一个 fetch，或把已读字节有界推进已绑定会话；AGAIN 不忙等。回调禁止重入。
- 已接线子集：M3U8 media playlist 的 VOD（`EXT-X-ENDLIST`）与无 ENDLIST 的直播窗口、MEDIA-SEQUENCE、EXTINF、RFC 3986 基址/相对/query（相对路径不继承基址 query；空引用保留基址 query 与 path）、有界刷新去重、`EXT-X-DISCONTINUITY`、`EXT-X-MAP`（fMP4 init）、可选 `EXT-X-BYTERANGE`。复用已有 TS / fMP4 AU 合同。
- 明确 `UNSUPPORTED`：master（`EXT-X-STREAM-INF` / `EXT-X-MEDIA` / I-FRAME-STREAM-INF / SESSION-*，本步不做半成品首变体选择）、`EXT-X-KEY` METHOD 非 NONE / DRM、LL-HLS（PART / PRELOAD-HINT / SKIP / SERVER-CONTROL / RENDITION-REPORT）、`I-FRAMES-ONLY` / `DEFINE`、替代音轨/字幕、ABR。不把密文当媒体，master 与 media 标签不混淆。
- 缺失分片、请求耗尽重试、序号跳跃或已知截断（声明 BYTERANGE 长于实际数据）发不连续，不把残缺装配与下一片简单拼接。无 BYTERANGE 的提前 EOF 按完整分片结束处理。
- 组 41 `test_hls_media_playlist`：URI 解析、VOD TS 闭环、live 窗口滑动去重、序号跳跃、DISCONTINUITY、master/KEY/LL-HLS 拒绝、缺失片、截断、重试预算、取消、反压、fMP4+MAP、文件 fetch。原 40 组 sanitizer debug + release 回归；`tests/dev_crosscheck.sh`（含箱体、渐进/fMP4 AU、FLV AU）通过；debug/release `ms_so_abi_smoke` 通过。
- 当时观察到 gcc 14.2.0 `-O2` 在「跳过缺失分片 + signal_discontinuity」上返回 `MS_ERR_NOMEM`，release 曾临时全局加上 `-fno-tree-vrp`。该规避已被 2026-11-20 19:35 CST 的源码侧修复取代，见下节；不得再把全局关优化写成现行做法。

### 体积与内存

同一宿主、同一编译器，`./build.sh release` 后 `strip --strip-unneeded`（2026-11-20 19:18 CST）：

- `libmsmedia.so.1.0.0` strip 后 **141552** 字节（strip 前 154776）。Source 小步对照为 116896 / 128664。
- `libmsmedia.a` **133562** 字节（Source 小步为 113982）。
- BuildID `34253a7bd6ed5a1c7e5d05dc8786265defa2f3b5`；NEEDED 仅 `libc.so.6`；导出 44 个 `MSMEDIA_1.0` FUNC；GLIBC_2.2.5 与 GLIBC_2.14。
- `size`：text 128778、data 888、bss 8，合计 129674。库内目标 text+data 约 121633（`ms_bmff_sample` 27005，`ms_session` 18891，`ms_ts` 18614，`ms_hls` 17632，`ms_h264` 11570，`ms_bmff` 10313，`ms_flv` 6245，`ms_util` 4581，`ms_adts` 3954，`ms_source` 1352 等）。
- debug：`.a` 4893940，`.so` 未 strip 376592（无 sanitizer），`test_core` 6905936，`ms_ts_inspect` 3737304，`ms_bmff_inspect` 1597944，`ms_so_abi_smoke` 39992。
- release：`test_core` 342456，`ms_ts_inspect` 129416，`ms_bmff_inspect` 55600，`ms_so_abi_smoke` 18944。
- 会话对象（不含动态预算）仍为 **23920** 字节；HLS 调度器自有分配，不占用会话动态预算。

宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做（诚实分列）

- **未联网**：没有 HTTP/TLS 客户端，不链接 curl/OpenSSL；内存/文件 fetch 只是 harness。
- **未硬解出画**：统一 C 硬解调用接口与平台后端仅写入 DESIGN/ROADMAP 与后续计划，本步没有实现代码，不得写成已实现。计划包括：Android API 21+ AMediaCodec；API 16–20 JNI MediaCodec（非 reflection，缓存 global `jclass`/`jmethodID`；DirectBuffer 不得跨 queue/flush 永久复用）；Win MF/D3D11；iOS VT/平台 surface；Linux VAAPI/V4L2。硬解外挂、不软解回退；压缩输入拷贝与 Surface 零回读分开；不保证所有芯片无条件硬解。
- 未做 master 变体选择/ABR、KEY/DRM、LL-HLS、替代音轨/字幕、DASH、RTMP/RTSP。
- 未 commit/push，未打最终包。

### 下一个可执行小步

后续按计划再续 HTTP/TLS 网络外挂、DASH，以及平台硬解后端。不要把本步写成已联网或已硬解出画。正确性调查见下节。

## 2026-11-20 19:35 CST — gcc 14.2 -O2 缺失分片路径调查与 HLS 总预算（宿主自测，非正式验收）

Asia/Shanghai（UTC+8）。Bot `/workspace/MsMedia` 宿主自测。未 commit/push（HEAD 仍为本地 `1092bec`），无 CloudAgent / machineId / 新登录，无 FFmpeg 产品依赖，未打最终包。未向外部报编译器 issue。

### 根因与修复

- 独立复现 `tests/repro_hls_miss.c`（不依赖前 40 组或组 41 前段状态）在 gcc 14.2.0（Debian 14.2.0-19）下：`-O0` / `-O1` / `-O2 -fno-tree-vrp` 通过；裸 `-O2`（有无 `NDEBUG`）与 `-O2` ASan/UBSan、`-fno-strict-aliasing`、`-fwrapv` 均失败，症状同为 `ost=NOMEM nau=1 ndisc=0 nreq=2`（只拉到 playlist 与 s1）。ASan/UBSan 无空间或 UB 诊断。宿主无 clang、无第二套 gcc，交叉编译器不可做。未发现 `NDEBUG`/pack 导致的会话布局变化；`ms_session` 只在本翻译单元定义。
- 失败不是会话预算耗尽，也不是 `evq_enqueue` 真的返回 `NOMEM`：无载荷 `DISCONTINUITY` 入队成功后，`ms_session_signal_discontinuity` 把 `MS_OK` 误报成 `MS_ERR_NOMEM`，测试未再 pull 故 `ndisc=0`。缺失分片失败路径没有被跳过或改成假成功。
- 源码形态为 `if (st != OK && st != LIMIT && st != AGAIN) return st; return (st == AGAIN) ? OK : st;`。`-O2` 的 gimple 已特化成对掩码 `0x9`（即 `{OK,LIMIT}`，且与 `MS_ERR_NOMEM==9` 同值）做成员测试，IR 本身若 st∈{0,3} 返回 st、否则返回 9，是正确的；随后降成 `bt`+`cmovae` 时条件被颠倒，st==OK 时返回 9。同文件里其他 `bt`+`jae`/`jb` 分支形式未表现该颠倒。`-fno-tree-vrp` 只是挡住了这条特化/降低路径，不能当作已证明「纯 VRP 通道缺陷」或用来掩盖 UB。未把最小玩具程序稳定复现到与该函数完全相同的 `cmovae`（需要与 `evq_enqueue` 相同的 IPA 返回集合）。证据留在 `tests/repro_hls_miss.c`、`scripts/opt_matrix.sh` 与 `build/opt-matrix/`（不删除既有 `build/debug`、`build/release`）。
- 修复：将该返回改为 `if (st == MS_AGAIN) return MS_OK; return st;`。修复后 `-O2` 在 `evq_enqueue` 之后直接返回其状态，函数内不再有 `bt`。全局 `-fno-tree-vrp` 已从 `build.sh release` 去掉，release 现为 `-O2 -DNDEBUG`。未请用户填 issue，也未外发。

### HLS 有界预算

- `ms_hls_limits` 增补末字段 `max_bytes`（`struct_size` 兼容，0=默认）。默认按对象、分片表、清单、chunk、URI、一块 hold 及分配头合计；绝对值 4 MiB。调度器自记账，超出返回 `NOMEM`（显式超过绝对值则 `INVALID_ARG`）。不计入会话动态预算，但不能因此无上限。未扩新协议。组 41 增加过小预算与超绝对值用例；缺失分片等失败路径仍保留。

### 测试矩阵

- 独立目录 `build/opt-matrix/` + `scripts/opt_matrix.sh`：同一 miss 复现于 o0 / o1 / o2 / o2-novrp / o2-ndebug-off / o2-ubsan / o2-asan / o2-asan-ubsan / o2-nosa / o2-fwrapv，源码修复后全部通过。
- 完整 41 组：debug ASan/UBSan 与 release（无全局关 VRP）均 `all tests passed`，含缺失分片路径。debug/release `ms_so_abi_smoke` 通过。`tests/dev_crosscheck.sh`、`dev_bmff_crosscheck.sh`、`dev_bmff_au_crosscheck.sh`、`dev_flv_au_crosscheck.sh` 通过。

### 体积与内存

同一宿主、同一编译器，`./build.sh release` 后 `strip --strip-unneeded`（2026-11-20 19:35 CST）：

- `libmsmedia.so.1.0.0` strip 后 **141552** 字节（strip 前 154800）。HLS 首步对照 strip 后同为 141552 / 前 154776；Source 小步为 116896 / 128664。
- `libmsmedia.a` **133306** 字节（HLS 首步为 133562）。
- BuildID `9fdd869c5f78a9bca2b977b365505b64ad34d150`；NEEDED 仅 `libc.so.6`；导出 44 个 `MSMEDIA_1.0` FUNC；GLIBC_2.2.5 与 GLIBC_2.14。
- `size`：text 128642、data 888、bss 8，合计 129538。库内 PIC 目标 text+data 共 121335（`ms_bmff_sample` 26661，`ms_session` 18603，`ms_ts` 18646，`ms_hls` 17672，`ms_h264` 11846，`ms_bmff` 10241，`ms_flv` 6277，`ms_util` 4589，`ms_adts` 3938，`ms_source` 1352 等）。
- debug：`.a` 4919060，`.so` 未 strip 377456（构建期 ASan/UBSan 启用；该 `.so` 本身为未加 sanitizer 的调试构建），`test_core` 6927728，`ms_ts_inspect` 3737272，`ms_bmff_inspect` 1597944，`ms_so_abi_smoke` 40016。
- release：`test_core` 338360，`ms_ts_inspect` 129416，`ms_bmff_inspect` 55600，`ms_so_abi_smoke` 18944。
- 会话对象（不含动态预算）仍为 **23920** 字节。

宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做（诚实分列）

- 当时仍未做 HTTP/TLS 客户端或 DASH，不链接 curl/OpenSSL；本步只做正确性与预算，不扩网络。随后的可选网络首步见 2026-11-20 20:00 CST 一节。
- 仍未硬解出画；统一 C 硬解接口仅是设计/计划。
- 未把该工具链现象自动对外报 issue。宿主无 clang 或其他 gcc 可供交叉。
- 未 commit/push，未打最终包。

### 下一个可执行小步

当时计划再续 HTTP/TLS 网络外挂；该首步已于 2026-11-20 20:00 CST 交付，见下节。不要把正确性小步写成已联网或已硬解出画。

## 2026-11-20 20:00 CST — 可选 HTTP/HTTPS 网络库首步（宿主自测，非正式验收）

by: Long <mycjzb@gmail.com>

### 已做

在 `/workspace/MsMedia` 以可选独立库实现可取消的 HTTP/1.1 GET 与 HTTPS，并接到既有 HLS fetch，而不是把 TLS 打进解析核心，也不是用 shell curl/ffmpeg 或 libcurl 冒充产品网络。

- 新公共头 `include/msmedia/ms_net.h` 与实现 `src/net/ms_tls.c`、`src/net/ms_net.c`，版本脚本 `MSMEDIA_NET_1.0`。产物为 `libmsnet.a` / `libmsnet.so.1`。`ENABLE_NET=0` 或缺少 OpenSSL 开发头时跳过。
- TLS：系统 OpenSSL 3.5.7 高层 API；`SSL_VERIFY_PEER`、默认信任库、可选附加 PEM CA（仅测试）、SNI、`SSL_set1_host`；最低 TLS 1.2；无关闭校验的公共开关；拒绝 https→http 降级。
- HTTP 子集：只 GET、`Connection: close`、`Accept-Encoding: identity`；Range；301/302/303/307/308 最多 5 次并检测循环；拒绝 URL userinfo；Content-Length / chunked / EOF 分界（二者同时出现则失败）；非 identity 的 Content-Encoding 为 `UNSUPPORTED`；401/403/其它非 200/206 为错误且不重试凭据。
- 线程：可选 worker 泵 DNS/连接/握手/IO；`request`/`read`/`cancel` 不做不可取消的无限同步读。无 worker 时仅有界 poll 切片。日志回调不得包含完整 URL/query/userinfo，自测已检查。
- `ms_net_hls_fetch_bind` 接到 `ms_hls_fetch`（同时一个在途请求）。受控 HTTP 上 media playlist → 真实 GET → TS AU 已闭环；master 仍 `UNSUPPORTED`。
- 测试 CA 只由 `build.sh` 生成在 `build/<mode>/net-ca/`，不入库。`test_net` 覆盖 200/206/chunked/重定向/循环/401/403/404/编码拒绝/超时/取消/TLS 200/不受信任证书/错误主机名/降级拒绝/HLS 闭环。
- 未采用 libcurl：与「自有 HTTP」目标不符，且本机无 curl 开发头。不把 curl 引入默认核心。
- 修复：302 跟随后不得再 poll 已关闭的 fd；头体同包到达时不得因提前 EOF 误报截断；测试服务器不得在原请求缓冲里截断路径以致读不到 Range。共享库导出补上 `MSMEDIA_API`（`-fvisibility=hidden` 下否则 `.so` 仅有版本节点）。`libmsnet.so` 设 `RUNPATH=$ORIGIN`。静态库不再 strip，以免无法再链接。

### 测试与回归

- `./build.sh debug`（gcc 14.2.0，ASan/UBSan）与 `./build.sh release`：原 **41** 组 `test_core` 全部通过；`ms_so_abi_smoke` 通过；独立 `test_net` 通过（含 leak 检测）。
- 交叉脚本（release 检查器）：`tests/dev_crosscheck.sh`、`dev_bmff_crosscheck.sh`、`dev_bmff_au_crosscheck.sh`、`dev_flv_au_crosscheck.sh` 均通过。`concat_disc.ts` 仍只有既有 NOTE，不作为产品失败。
- 已有三路 URL 文件（`build/dev-samples/live/urls.env`，仍不入库、不公开地址）在 16 MiB / 15 秒上限内做了短时抽样，不遇阻改走其它通路：一路 HTTPS 200、893 字节、正文魔数为 m3u8；另两路（一 HTTPS、一 HTTP）为 HTTP 404、0 字节。这不是实网兼容验收。

### 体积与依赖

同一宿主、同一编译器，`./build.sh release`（2026-11-20 20:00 CST）：

- `libmsmedia.so.1.0.0`：**141552** 字节（unstripped 154800），BuildID `9fdd869c5f78a9bca2b977b365505b64ad34d150`，NEEDED 仅 `libc.so.6`，44 个 `MSMEDIA_1.0` 函数。`size` text 128642、data 888、bss 8。核心无 SSL 符号。`libmsmedia.a`：**187532** 字节（不再 strip）。
- `libmsnet.so.1.0.0`：**35432** 字节（unstripped 41352），BuildID `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`，NEEDED `libmsmedia.so.1`、`libssl.so.3`、`libcrypto.so.3`、`libc.so.6`，14 个 `MSMEDIA_NET_1.0` 函数。`libmsnet.a`：**36470** 字节。本机 OpenSSL 3.5.7，Apache-2.0，见 `docs/THIRD_PARTY.md`。ldd 可见 libcrypto 的间接依赖（zlib/zstd），不是本项目直接链接行。
- 会话对象仍为 23920 字节（与正确性小步相同）。宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。TLS 库验证不等于四平台实机。

### 未做（诚实分列）

- 未实现 MPEG-DASH、RTMP/RTSP/RTP/UDP、系统平台 HTTP backend、HTTP/2 或 HTTP/3、POST、代理、连接复用、内容压缩、Cookie/鉴权头。
- 未把 `libmsnet` 做成通用 Source 随机读后端；本步接到的是 HLS fetch（以及可流式读取的 GET）。
- `getaddrinfo` 仍可能在解析阶段阻塞，需放在 worker 上；不是异步 DNS。
- 未软解、未硬解出画；统一 C 硬解真实后端仍待做。
- 未 commit/push，未打最终包，未登录远程。

### 下一个可执行小步

后续按计划再续 MPEG-DASH（MPD），以及视需要补齐代理/复用等 HTTP 后续能力；不要把本步写成 DASH、可播放或硬解已完成。


## 2026-11-20 20:25 CST — MPEG-DASH（MPD）清单与分片调度最小子集（宿主自测，非正式验收）

### 阶段

P3 在 HLS 调度与可选 HTTP/HTTPS 网络库首步之后，增加可验证的 MPEG-DASH MPD 调度最小子集。不是全部 DASH profile，不是 ABR/DRM，不是硬解出画，不是四平台或 el7 验收。

### 已完成

- 先按现有 HLS 调度、`ms_hls_fetch`、会话 fMP4（只接受一次 init/`moov`）与 TS AU 合同设计界限，再实现 `ms_dash_*`（create/destroy/bind/open/drive/cancel 与 limits），导出计入 `MSMEDIA_1.0`。
- 复用既有 fetch vtable（类型名仍为 `ms_hls_fetch`，内存/文件 harness 与 `ms_net_hls_fetch_bind` 无需改调用方）、`ms_hls_resolve_uri`、不连续信号、反压与自记账 `max_bytes`（默认分量合计，绝对值 4 MiB）。不占用会话动态预算，但不能无上限。
- 有界自制 XML 分词：只解本子集所需字段；允许注释与 `<?xml ...?>`；只接受预定义实体与数值字符引用；拒绝 `<!`（DOCTYPE/实体/CDATA）、未知实体、NUL 与过深嵌套；不引入重型 XML 依赖，不做 XXE。
- 支持常见 static/dynamic MPD 的单 Period、单 AdaptationSet、单 Representation（混排 A/V 或单轨）；SegmentBase（无 indexRange，Initialization 范围后同一资源余下为一段 media）、SegmentList、SegmentTemplate（`$Number$` / `$Time$` / `$RepresentationID$` / `$Bandwidth$`，含可选宽度）；Initialization + media；可选字节范围。dynamic 窗口刷新时跳过已播放序号（去重）。
- 缺片、截断、序号跳跃发 `ms_session_signal_discontinuity`，不把残缺与下一片拼接。见加密 ContentProtection 返回 `UNSUPPORTED`，不得把密文当媒体。
- 明确拒绝：多 Representation ABR 自动选路、xlink、字幕/其它类型 AdaptationSet、独立音视频 AdaptationSet（会话不能在不连续后吃第二份 init）、Period 数≠1、SegmentBase `indexRange`/本步不解析 sidx、未知模板标识、CMAF LL 全集。
- 会话 `ms_session_pull` 在 BMFF 路径上于 drain 之后续做 `process_inbuf`（与已有 FLV 处理一致），避免 end_of_input 后仍停在 pending 的完整 fMP4 AU 因直接发 EOS 而丢失。这是反压+EOI 的正确性修补，不是新的 demux 合同。
- 调度器在媒体分片 `request_fn` 永久失败（如缺 URI）时，与读失败一样发不连续并跳过该序号，而不是把 `UNKNOWN` 当整次调度失败。
- `test_dash_mpd`：受控 MPD + 自造 fMP4/TS 分段出真实 AU；覆盖 tight 预算、Template/List/Base、`$Time$`+SegmentTimeline+`$Number%03d$`、拒绝路径、缺片、截断 mediaRange、取消、反压 LIMIT 后仍收齐 AU、dynamic 列表增长去重、TS SegmentTemplate。固件不入库，产品不链 FFmpeg。
- 回归：原 41 组 + 本 DASH 组共 42 组，`./build.sh debug`（ASan/UBSan，leak 检测）与 `release` 均全部通过；`test_net` debug/release 回归通过（仍为 HTTP/HTTPS+HLS，未加 DASH HTTP 用例）；四份交叉脚本与 `ms_so_abi_smoke`（debug/release）通过。核心 `.so` NEEDED 仍仅 `libc.so.6`。
- 文档按事实更新。未 commit / push，未打最终包，未登录远程。

### 体积与依赖

同一宿主、同一编译器，`./build.sh release`（2026-11-20 20:25 CST，gcc 14.2.0）：

- `libmsmedia.so.1.0.0`：**166152** 字节（unstripped 180424），BuildID `0638a8fc5ae9aa3f8ca2f7257e9bc83a61dfc4fe`，NEEDED 仅 `libc.so.6`，51 个 `MSMEDIA_1.0` 函数（新增 7 个 `ms_dash_*`）。`size` text 155524、data 920、bss 8，合计 156452。核心无 SSL 符号。`libmsmedia.a`：**223570** 字节（不再 strip）。
- `libmsnet.so.1.0.0` 仍为 **35432** 字节（unstripped 41352），BuildID `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`，14 个 `MSMEDIA_NET_1.0` 函数；`libmsnet.a` **36470** 字节。本步未改网络库。
- 会话对象仍为 23920 字节。宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做（诚实分列）

- 未实现多变体 ABR 自动选路、DRM/ContentProtection 解密、字幕/交替轨、xlink、复杂 multi-period 广告插入全集、CMAF LL 全集、SegmentBase indexRange/sidx 解析。
- 独立音视频 AdaptationSet 与后继 Period 的新 Initialization：因会话 fMP4 只接受一次 `moov`，本步明确 `UNSUPPORTED`，不是已支持。
- 未把 DASH 接到 `test_net` 的本机 HTTP/HTTPS 闭环；`libmsnet` 仍可绑定同一 fetch，但本步没有该用例。
- 未做 DASH 时移 seek、HTTP/2 或 HTTP/3、代理/连接复用等网络后续；未实现 RTMP/RTSP/RTP/UDP。
- 未软解、未硬解出画；统一 C 硬解真实后端仍待做。
- 未 commit/push，未打最终包，未登录远程。

### 下一个可执行小步

视需要把本 DASH 调度接到已有 `libmsnet` 做本机受控 HTTP 闭环，或先评估会话多 Initialization（否则不能宣称分轨 AS / Period 切换）。不要把本步写成 ABR、DRM、硬解出画、四平台或全部 DASH profile 已完成。


## 2026-11-20 20:34 CST — DASH 调度接到 libmsnet 的受控 HTTP 闭环（宿主自测，非正式验收）

### 阶段

在已交付的 DASH MPD 调度最小子集之上，把 `ms_dash_*` 接到已有 `ms_net_hls_fetch_bind`（同一 fetch ABI），用本机受控 HTTP 服务 MPD + 分段并出真实 AU。不扩 ABR/DRM/分轨 AS/多 Period，不把网络测试写成四平台实机验收。

### 已完成

- 复用既有 `libmsnet` 与测试 HTTP 服务，不新增重型依赖，不改公共 ABI。受控服务增加可 Range 的静态资源表；DASH 用例加在 `test_net`（不并入 42 组 `test_core`）。
- 本机 HTTP 闭环（合成 MPEG-TS 分段，复用已有 `ms_ts_synth`；产品不链 FFmpeg，固件不入库）：
  - 200 VOD：`SegmentTemplate` 相对 URI 经真实 GET 出 AU；
  - 可选 Range：`SegmentList` `mediaRange` 由库发出 `Range`，服务端 206，出 AU；
  - 缺片 404 与过长 `mediaRange` 截断：发不连续，不拼接残缺，自包含后段仍可出 AU；
  - 取消：`ms_dash_cancel` 后 `drive` 为 `CANCELED`（经 fetch `cancel_fn` → `ms_net_cancel`）；
  - 反压：多帧单段 + 小 `au_queue_bytes`，出现 `LIMIT` 后仍收齐全部 AU；
  - `ContentProtection` 明确 `UNSUPPORTED`，无 AU。
- 会话正确性：`ms_session_pull` 在 TS 路径于 `ms_ts_parser_drain` 之后续做 `process_inbuf`（与已有 FLV/BMFF 处理一致），避免 `end_of_input` 后仍留在 `in_buf` 的完整 AU 因直接发 EOS 而丢失。不是多 Initialization，不是新的 demux 合同。
- 回归：42 组 `test_core` debug ASan/UBSan（含 leak）与 release 全部通过；`test_net` debug/release 通过（原 HTTP/HTTPS+HLS 回归保留，并新增 DASH HTTP 用例）；四份交叉脚本与 `ms_so_abi_smoke`（debug/release）通过。核心 `.so` NEEDED 仍仅 `libc.so.6`。
- 文档按事实更新。未 commit / push，未打最终包，未登录远程。

### 体积与依赖

同一宿主、同一编译器，`./build.sh release`（2026-11-20 20:34 CST，gcc 14.2.0）：

- `libmsmedia.so.1.0.0` strip 后仍为 **166152** 字节（unstripped 180424），BuildID `e58e4faa1e6ef64f2f411361f2b5ce13e40b763e`（相对 20:25 的 `0638a8fc…` 因上述 TS `pull` 修补而更新），NEEDED 仅 `libc.so.6`，仍为 51 个 `MSMEDIA_1.0` 函数。`size` text 155588、data 920、bss 8，合计 156516。核心无 SSL 符号。`libmsmedia.a`：**223634** 字节（不再 strip）。
- `libmsnet.so.1.0.0` 仍为 **35432** 字节（unstripped 41352），BuildID `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`，14 个 `MSMEDIA_NET_1.0` 函数；`libmsnet.a` **36470** 字节。本步未改网络库源码。
- 会话对象仍为 23920 字节。宿主 Linux x86-64 产物不是 Android NDK ABI，也不是 el7/四平台正式包。

### 未做（诚实分列）

- HTTP 闭环本步以 TS 分段验证网络+调度合同；fMP4 主路径仍以 `test_core` 内存/文件 harness 覆盖，未在 `test_net` 再走一遍 fMP4/HTTPS DASH。
- 未扩展 ABR、DRM 解密、字幕/交替轨、xlink、分轨 AdaptationSet、多 Period、indexRange/sidx、CMAF LL、DASH 时移 seek。
- 未做 HTTP/2 或 HTTP/3、代理/连接复用等网络后续；未实现 RTMP/RTSP/RTP/UDP。
- 未软解、未硬解出画；统一 C 硬解真实后端仍待做。
- 未 commit/push，未打最终包，未登录远程。

### 下一个可执行小步

评估会话多 Initialization（否则不能宣称分轨 AS / Period 切换）。不要把本步写成 ABR、DRM、硬解出画、四平台或全部 DASH profile 已完成。


## 2026-11-20 21:21 CST — 官方 FFmpeg 9.1 裁剪 + 可选后端首个可验证接入（宿主自测，非正式验收）

授权：[pt67u] 用户明确改变架构（不是助手建议），要求常用 RTSP/RTMP/UDP/RTP/HTTP/HTTPS 都支持，并使用最新 ffmpeg9 剪裁优化。先前已答复保留统一 C 接口与系统硬解、取流/协议/解封装改 FFmpeg，且须先核实官方 9 版本。本步按该方向实施：不继续扩展自写 HLS/MPD/网络，不删除现有代码与未提交改动，保留回归对照。无 CloudAgent / machineId / commit / push / 登录 / 最终包；宿主编译自测不能代替 el7/四平台/硬解验收。

官方版本（ffmpeg.org/download.html 与 /releases/，不以系统 7.1.5 或用户日期推断为准）：最新稳定版为 **FFmpeg 9.1 “Gauss”**（2026-11-20 发布，分支 2026-10-24 从 master 切出）。tarball `https://ffmpeg.org/releases/ffmpeg-9.1.tar.xz`，PGP Good signature（公钥指纹 `FCF986EA15E6E293A5644F10B4322F04D67658D8`）。官方不发布 SHA-256；本地在验签后钉选 `1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433`（`third_party/ffmpeg-9.1.pin`）。解包 `RELEASE` 与 `FFMPEG_VERSION` 均为 `9.1`。当时首页新闻尚未刊登 9.1，仍以 download/releases 为权威源。未把 master 冒充 9，未静默换版本。

构建：`scripts/ffmpeg9_build.sh`，由 `./build.sh` / `./build.sh ffmpeg` 调用。`--disable-everything` 后开启协议/解复用/parser/bsf 与 openssl、libxml2；`CONFIG_GPL=0`、`CONFIG_NONFREE=0`、`CONFIG_DECODERS=0`，许可字符串 `LGPL version 2.1 or later`。无软编解码器。TLS 强制校验，拒绝 https→http。DASH XML 用系统 libxml2，不重造解析。缓存不自动升级。测试未链系统 libav。

接入：可选隔离库 `libmsff`（`include/msmedia/ms_ff.h`），映射公共 `ms_event`；`ms_session` 兼容。`ms_format` 追加 HLS/DASH。`test_ff_backend`：本地 TS/MP4/FLV 出事件；受控 HTTP 多层 HLS；302 后相对 URI 按跳转后基址；受控 HTTPS（测试 CA）成功、无 CA 失败、降级拒绝；反压与 cancel。`avformat_find_stream_info` 在无 decoder 时对上述样本返回 0，短 TS 仍可能缺采样率（已如实记录）。**未验证**：RTSP、RTMP、裸 UDP/RTP、DASH-over-HTTP。

回归：`test_core` 全部通过，`test_net` 通过，`ms_so_abi_smoke` 通过，四组 `dev_*crosscheck.sh` 通过（系统 7.1.5 只作样本生成，不是产品后端）。

同一宿主、同一编译器，`./build.sh release`（2026-11-20 21:21 CST，gcc 14.2.0，x86_64，非正式验收，不能冒充 NDK/旧 Android）：

- 核心 `libmsmedia.so.1.0.0` 去调试符号后仍为 **166152** 字节（text 155588 / data 920 / bss 8）。NEEDED 仅 `libc.so.6`。`libmsmedia.a` **223834** 字节（20:34 为 223634，增量来自格式名称）。BuildID `efd939e4d131d6c406387f015a6707698bc6a935`。
- 可选 `libmsnet.so.1.0.0` 仍为 **35432** 字节，BuildID 仍为 `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`。
- 可选 `libmsff.so.1.0.0` 去调试符号后 **2105232** 字节（unstripped 9389640），BuildID `03b86d173258d9ae1f4b192536f450d757670c74`。这是适配器加上钉选 FFmpeg 9.1 裁剪静态库的总量，不是单独 19190 字节的 `libmsff.a`。NEEDED：`libssl.so.3`、`libcrypto.so.3`、`libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`。`size` text 1935486 / data 157688 / bss 44872。相对旧核心的宿主 strip 增量约 1.94 MiB。

许可证义务（FFmpeg LGPL 2.1+ 静态链入、OpenSSL Apache-2.0、libxml2 MIT 风格）见 `docs/THIRD_PARTY.md` 与 `docs/FFMPEG_BACKEND.md`。未代选 MsMedia 顶层许可证，未声称无开源义务。HEAD 仍为本地 `1092bec`，未 commit/push。


## 2026-11-20 22:07–22:11 CST — libmsff 受控 DASH-over-HTTP 与 UDP/RTP 子集联调（宿主自测，非正式验收）

授权：[pt71u]「进度到哪里了 继续」。延续钉选官方 FFmpeg 9.1 与隔离 `libmsff`，不恢复自写协议路线，不更换 libxml2，不开展一体静态依赖裁剪。RTSP/RTMP 仍为另里程碑，本轮未自行扩展。无 CloudAgent / machineId / commit / push / 登录 / 最终包 / 软解。

本轮实现：`scripts/gen_ff_samples.sh` 仅用系统 ffmpeg/ffprobe 在被忽略的 `build/dev-samples/dash-ff/` 生成分离 A/V AdaptationSet、SegmentTimeline 的 DASH VOD（MPD + init + 多段 fMP4）及约 1s 的 `short.ts`；产品不 exec 该脚本或 ffmpeg。`build.sh test` 在运行 `test_ff_backend` 前自动生成并设置 `MS_FF_DASH_DIR`。适配器将负 PTS/DTS 记为 `has_pts`/`has_dts`=0，不再绕成巨大 uint64。

DASH-over-HTTP（受控本机 HTTP，真正 FFmpeg 后端）：format=dash，分离 A/V，相对 URL；视频 75 包与系统 ffprobe 7.1.5 的 PTS/DTS/size/key 一致；音频 9.1 多 1 个尾包（141→142），已记录。无扩展名 MPD 在 `Content-Type: application/dash+xml` 下可探测。302 后基址成功。缺中片 video_au=50、截断 mid video_au=25（均未收齐 75）。401 失败（status=5）。取消后 pull=CANCELED。停顿超时经 interrupt 表现为 `MS_ERR_CANCELED`（不是单独 timeout 码）。不把每个 AVPacket 写成完整 AU。探测缺 metadata（stream_info=0）如实记录。不绕 401、不降级 TLS、无界重试未做。

在第一部分已稳且时长允许的情况下，继续 localhost 受控子集：UDP MPEG-TS（pkt_size=1316，listen+超时，延迟突发发送）events=77 v=25 a=48 format=mpeg_ts；RTP/SDP H264 payload 96、packetization-mode=1，经 `file:sdp`，events=15 h264_packets=12，公共 format=unknown。RTP 包可能是 NAL/FU-A，不是完整 AU，不等价所有摄像机。端口临时绑定，进程退出回收。

回归（2026-11-20 22:08–22:11 CST）：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（debug，适配器与测试带 ASan/UBSan；FFmpeg 9.1 prefix 无 asan）`test_core` 42 组、`test_net`、`test_ff_backend` 均通过，日志无 sanitizer runtime 诊断。不得声称对 libav 全覆盖。`./build.sh release` 后 `ms_so_abi_smoke` 通过；四组 `tests/dev_*crosscheck.sh` 对 release 的 inspect 均 RESULT 匹配（系统 7.1.5 只生成样本）。

体积复测（2026-11-20 22:10 CST，同一宿主 gcc 14.2.0 x86_64，非正式验收，不能冒充 NDK/旧 Android）：

- 核心 `libmsmedia.so.1.0.0` 去调试符号后仍为 **166152** 字节（text 155588 / data 920 / bss 8），NEEDED 仅 `libc.so.6`，BuildID 仍为 `efd939e4d131d6c406387f015a6707698bc6a935`。`libmsmedia.a` 仍为 **223834** 字节。
- 可选 `libmsnet.so.1.0.0` 仍为 **35432** 字节，BuildID 仍为 `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`。
- 可选 `libmsff.so.1.0.0` 去调试符号后仍为 **2105232** 字节（unstripped 9389640），`size` 仍为 text 1935486 / data 157688 / bss 44872。因负 PTS 处理，BuildID 现为 `992811bab811ac66030a95322d4a8e9235f029d9`（21:21 为 `03b86d17…`）。薄适配层 `libmsff.a` 现为 **19126** 字节（21:21 记载 19190）。NEEDED 仍为 `libssl.so.3`、`libcrypto.so.3`、`libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`。相对旧核心的宿主 strip 增量仍约 **1.94 MiB**。约 2.01 MiB **不含** 系统 SSL/XML 等外部动态库；约 11 MiB 只曾按动态文件求和估算，非实测，不作承诺。未换 XML 后端，无新源码包。

能力矩阵见 `docs/FFMPEG_BACKEND.md`。剩余：RTSP（TCP/UDP transport）、RTMP/RTMPS/RTMPT、SRTP、完整摄像机 RTP、HEVC、live seek/discontinuity、四平台/NDK/el7 正式编译、系统硬解出画。HEAD 仍为本地 `1092bec`，未 commit/push。


## 2026-11-21 11:12 CST — 时间戳 origin、TIMEOUT 与 DASH trun 尾包（宿主自测，非正式验收）

授权：[pt73u] 纠正「每轮仅报告不推进」，要求先完成正确性再接 OpenSSL/RTSP 等。本里程碑只做正确性；OpenSSL 静态合入、RTSP/RTMP、Android 硬解虽已批准，未在本轮实施。无 CloudAgent / machineId / commit / push / 登录 / 最终包 / 软解。未更换 libxml2。

时间戳：废除将合法负 PTS/DTS 标为 `has_*=0` 的做法。`ms_event` 布局不变，ABI 版本仍为 1。`pts`/`dts` 为相对会话级统一 origin（90 kHz，所有轨相同）的非负 uint64；`AV_NOPTS_VALUE` 才无时间戳。经 `ms_ff_time_origin_90k` + `ms_event_signed_ts` / `ms_event_media_time_90k` 还原。DASH 音频 priming -1024 保留，origin_90k=-1920；bframes.mp4 中 DTS&lt;PTS 21 次，min signed=-1024，origin=-7200。不按轨截 0。

超时：追加 `MS_ERR_TIMEOUT`（枚举末尾）。`io_timeout_ms` 触发 interrupt 或 `AVERROR(ETIMEDOUT)` 返回 TIMEOUT，不再伪装 CANCELED；用户 cancel 优先。EOF / EAGAIN / EIO 区分。原子标志避免 cancel 与 I/O 线程数据竞争。探测阶段 `find_stream_info` 缺 metadata 或超时仍只记录，不把已打开输入判失败。

DASH 尾包：独立 trun 表音频 142（末样本 pts=143360 size=146 duration=640，补齐 MPD 3.0s）；ffprobe 7.1.5 为 141，属版本解封装差异，不是残帧或适配器重复。后端与 trun 一致。不按跨版本 packet 数断言正确。

RTP/SDP 帧合同（FU-A/多 NAL/marker）本轮明确排队，不以感觉声称 NAL/FU-A。

回归：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（debug，ASan/UBSan 仅适配器与测试；FFmpeg 无 asan）42 组 `test_core`、`test_net`、`test_ff_backend` 通过，无 sanitizer runtime 诊断。release 后 `ms_so_abi_smoke` 与四组 `dev_*crosscheck.sh` 通过。

体积（2026-11-21 11:11 CST，gcc 14.2.0 x86_64，非正式验收）：`libmsmedia.so.1.0.0` **166168** 字节（text 157622 / data 936 / bss 8），BuildID `d84caf76b9d81eb3c5688c32732a06ef54c3a1ae`，NEEDED 仅 libc，`.a` 225726；`libmsnet` 仍 **35432** / `cb3ea5b7…`；`libmsff.so.1.0.0` **2109328**（unstripped 9393816），BuildID `7df116d2015a254420d44e8521401771ca100901`，`size` text 1939394 / data 157688 / bss 44872，`.a` 23032。相对核心增量约 1.85 MiB。约 2.01 MiB 不含系统 SSL/XML；11 MiB 非实测。HEAD 仍为 `1092bec`，未 commit/push。


## 2026-11-21 11:26 CST — OpenSSL 3.5.9 LTS 按需静态裁剪合入 libmsff（宿主自测，非正式验收）

授权：用户 t74「openssl也要合并进去」、t75「openssl也要裁剪只留用到的」、t78「继续完成任务按这些要求交付」。正确性里程碑已交付，本轮只实施该合入。RTSP/RTMP 与 Media3 硬解/低内存零冗余 copy 保持后续，未堆入本轮。无 CloudAgent / machineId / commit / push / 登录 / 最终包 / 软解。未删旧路径。未更换 libxml2。

钉选官方仍受安全支持的 3.5.9 LTS，不是宿主 3.5.7，也不是最新主版本 4.1。官方 SHA-256 与 PGP Good signature 已核。`./build.sh openssl` / `ffmpeg` 可重复，离线缓存不自动升级。PIC 静态 `libssl.a`/`libcrypto.a` 合入最终 `libmsff.so`；NEEDED 不再有 libssl/libcrypto；符号 hidden，动态导出仅 `ms_ff_*`。`ms_ff_openssl_version` / `ms_ff_openssl_version_num` 实测 3.5.9 / `0x30500090`。核心仍无 SSL；`libmsnet` 仍动态链系统 OpenSSL 作对照。libxml2/z 仍动态。no-gpl/no-nonfree/no-decoders 保持。默认 provider 内置，无系统 provider `.so` / 系统 openssl.cnf；无跳过校验开关。默认信任为宿主 `/etc/ssl/certs` 符号链接（Android 未测）。裁剪开关、兼容代价与算法审查见 `docs/FFMPEG_BACKEND.md` 第 9 节。未宣称更快，未宣称未来证书 100% 兼容，未宣称静态改善运行内存。

回归：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（debug，ASan/UBSan 仅适配器与测试；FFmpeg 与 OpenSSL prefix 无 asan）42 组 `test_core`、`test_net`、`test_ff_backend` 通过，含 HTTP/HLS/DASH、TLS 成功、不受信/hostname 错/降级失败、TLS1.2/1.3×RSA-2048/ECDSA-P256、TLS1.1-only 失败、TIMEOUT/CANCELED、DASH origin_90k=-1920 a=142、UDP/RTP 子集。无 sanitizer runtime 诊断。release 后 `ms_so_abi_smoke` 与四组 `dev_*crosscheck.sh` 通过。

体积（同一宿主 gcc 14.2.0 x86_64，`./build.sh release`，非正式验收）：`libmsmedia` 仍 **166168** / `d84caf76…`；`libmsnet` 仍 **35432** / `cb3ea5b7…`；`libmsff.so.1.0.0` **7986448**（unstripped 16054352），text 7362738 / data 614656 / bss 54616，BuildID `28b2d694a66df83d27b2f840e0e1a81731e2752e`，`.a` 23472。直接 NEEDED：libxml2、libz、libm、libc。相对合入前 2109328 的静态 SSL 增量 **5877120** 字节。HEAD 仍为本地 `1092bec`，未 commit/push。下一已批准阶段：RTSP/RTMP 与 RTP 帧合同，之后 Media3 Android 硬解 + 内存/复制计量。

## 2026-11-21 11:58 CST — RTSP/RTP 帧合同与 RTMP 本机拉流联调（宿主自测，非正式验收）

授权：t67「rtsp rtmp udp rtp http https都支持」、t78「继续完成任务按这些要求交付」、t73 不要每轮停。正确性与 OpenSSL 静态合入已交付且未改 pin。本轮只做受控 loopback 联调与独立 AU 参考对照。无 CloudAgent / machineId / commit / push / 登录 / 最终包 / 软解。`CONFIG_DECODERS=0`。产品不 exec 系统 ffmpeg 或测试脚本。

实现：追加 `MS_FORMAT_RTSP/RTMP/RTP`（ABI 仍为 1）；残帧/`AV_PKT_FLAG_CORRUPT` 拒绝；FFmpeg 9.1 失包仅日志不置 CORRUPT，适配器捕获「RTP: missed」后丢当前包并等到下一关键帧；打开后补发 CODEC_CONFIG；RTMP 不把通用微秒 `timeout` 写入会隐含 listen 的选项键。测试工具 `scripts/rtp_h264_testutil.py` 仅测试。

宿主实测：RTP 完整 16/16 与独立参考一致（FU-A/STAP-A/多 slice）；重排序 16/16；丢 mid-FU-A 后 11/16、mismatch=0、first_key=1；RTSP TCP/UDP 各 16/16 format=rtsp；RTSP stall=`TIMEOUT`；RTMP pull format=rtmp，cfg+H264/AAC 媒体包。RTMPS/AAC-over-RTSP 未测。TLS 矩阵与既有 HTTP/HLS/DASH/TIMEOUT 回归仍过。

回归：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 42 组 `test_core`、`test_net`、`test_ff_backend` 通过；ASan 仅适配器与测试。release 后 `ms_so_abi_smoke` 与四组 `dev_*crosscheck.sh` 通过。

体积（gcc 14.2.0 x86_64 release）：`libmsmedia` 仍 **166168**（BuildID `6b13748bd29ce2939112c8b4bddd6c3b5de28f9e`）；`libmsnet` 仍 **35432** / `cb3ea5b7…`；`libmsff.so` **7986520**（+72 vs 11:26），BuildID `0c556a20868e653ac7f542d0ac497d5a040049b9`，`.a` 27112。NEEDED 仍无系统 libssl。未测 RSS。HEAD `1092bec`，未 commit/push。下一已批准阶段：Media3 Android 硬解 + 内存/复制计量。

## 2026-11-21 12:19 CST — 体积优化（-Os / --enable-small / section-gc / strip；宿主 + 非正式 v7a 同 ABI）（宿主自测，非正式验收）

授权：t80「要开启优化」、t82「可以看看 cjzb 的编译」、t83「tls 是 openssl」。先完成体积优化，本轮不扩展硬解。CJZB 只读：不把 4,214,156 字节 mbedTLS v7a so 当 OpenSSL 对照，不换 TLS 库，不照搬 1.1.1 减功能选项。已有 `build/release` 基线保留。

实现：`MS_SIZE_OPT=1` 独立 prefix；`./build.sh release-size` / `test-size`；OpenSSL `-Os` + `OPENSSL_SMALL_FOOTPRINT`；FFmpeg `--enable-small` 且 extra-cflags 为 `-Os`；适配器 `-Os` + function/data sections + gc + 隐藏符号 + strip。配置后检查最后一个 `-O` 不被覆盖。`scripts/android_v7a_size.sh` 做 NDK r29 API 21 同 ABI 对照。无 `__int128` 的目标在 `ms_ff.c` 使用便携向 -inf 乘除（宿主仍走 `__int128`）。

宿主 gcc 14.2.0 x86_64 strip：`libmsff.so` 7986520 → **5767128**（-27.79%），NEEDED 仍为 libxml2/libz/libm/libc；`libmsmedia` 166168 → **125112**（-24.71%，仅 libc）；`libmsnet` 35432 → **31240**（-11.83%，仍系统 OpenSSL）。非正式 v7a（静态 libxml2，两侧相同，不可与宿主或 mbedTLS so 比）strip：5390136 → **4235076**（-21.43%）。有效 flags 见 `build/release-size/effective-flags.txt` 与 `docs/FFMPEG_BACKEND.md` 第 11 节。

回归：`./build.sh test-size`（无 sanitizer）42 组、`test_net`、`test_ff_backend`（含 TLS/HTTP/HLS/DASH/RTSP/RTP/RTMP、负时间戳、TIMEOUT、无软解）通过；`ms_so_abi_smoke` 与四组 crosscheck 通过。性能与内存未测。HEAD `1092bec`，未 commit/push。本条之后的硬解小里程碑已于 2026-11-21 12:36 CST 交付宿主 mock 与非正式 NDK 交叉编译（`docs/HW_ANDROID.md`）；真实设备硬解仍未跑。

## 2026-11-21 12:36 CST — 可选 Android H264 硬解路径（libmshw；宿主 mock + 非正式 NDK 交叉编译）（非正式验收）

授权：体积优化完成后继续 Media3 成熟方式的 Android 硬解、低内存与减少冗余 copy。仅 Bot `/workspace/MsMedia`；不 CloudAgent、不 machineId、不 commit/push、不删除未提交内容。不扩展音频/字幕。

设计：新可选 `libmshw` + `include/msmedia/ms_hw.h`，不改核心 `ms_hw_query` 桩与 `MSMEDIA_ABI_VERSION`。不以 `createDecoderByType` 成功报硬解；`MediaCodecList` 选择后 `createByCodecName` / `AMediaCodec_createCodecByName`。API≥29 用硬件/软件标志，否则用与公开 Media3 启发式同类的名称规则，未知命名空间当软件。默认拒绝软解。Surface 不 CPU 回读。输入引用持有 + 有界硬上限 + `platform_copy_*` / `staging_copy_*` / `pin_peak_bytes` 分列计量。参考公开 Media3 经验，不宣称等同 Media3，不链 androidx。

验证：`test_hw_backend` 在 debug ASan/UBSan 与 `./build.sh test-size` 通过（仅宿主 mock）。`test-size` 同时回归 42 组、`test_net`、`test_ff_backend`（TLS/协议）。体积优化数字未改：`libmsff.so` 仍 5767128，`libmsmedia` 125112，`libmsnet` 31240；`build/release` 基线 7986520 保留。非正式 NDK r29 API 21 v7a：strip `libmshw.so` 21368 字节（NEEDED libmediandk/libandroid/liblog/libc/libdl）；JNI 16–20 源码以同一工具链编为 `libmshw-legacy.a`（NDK r29 不能 target API 16）；探测 APK `MsHwSmoke.apk` 33243 字节已签名未安装。`adb devices` 为空，未启动共享 AVD，**未做真实硬解运行**。详见 `docs/HW_ANDROID.md`。HEAD 仍为 `1092bec`，未 commit/push。

## 2026-11-21 16:21 CST — 编排播放器与 HLS 分片剥离实证

在 PNG/JPEG 有界扫描与 file/HTTP 剥离之后，本轮完成可验证的 `ms_hw_player` 编排（CSD、时间戳、关键帧门闩、反压、取消、EOS、flush、切换释放、引用/必要 copy/staging copy 分列）及宿主 mock 状态机测试；`ms_ff` 压缩 AU 可喂入该播放器（实测 `short.ts` 25 个视频 AU）。分层计时区分首包、首可解 AU、首次 submit、首次交 Surface（**不是显示**）与仅 `note_display` 的显示钩子。交叉编译可运行 `MsHwPlayer.apk`（180774 字节，已签名，含 `libmshw.so`+`libmsmedia.so` 与 Surface 编排界面及本地 TS/FLV 入口；未安装、不是硬解/出画验收）。FFmpeg HLS 内部分片伪装已单独实证：`ms_ff_ts_prefix_stats` 记录 `ops=1 last_bytes=457 kind=png` 且拉到 AU。完整 `test_ff_backend` 在既有 CA/样本下 exit 0，未把 SKIP 写通过。详见 `docs/HW_ANDROID.md` §9 与 `docs/FFMPEG_BACKEND.md` 文末。未 commit/push，未启动共享 AVD。

补记（2026-11-21 16:26 CST）：非正式 NDK r29 API 21 v7a 重编后，`MsHwPlayer.apk` 为 **180774** 字节（`libmshw.so` 33128，NEEDED 含 `libmsmedia.so`；`libmsmedia.so` 126832）。APK 含本地 `file://` / path 的 `ms_session` 入口，仍未打入 `libmsff`。`armeabi-v7a` 因无 `__int128` 使用 `ms_util.c` 便携乘除。未安装、未启动共享 AVD。

## 2026-11-21 16:39 CST — 网络 APK 集成优化 v7a libmsff

把 size-opt v7a FFmpeg/OpenSSL 前缀重链当前适配器打入 `MsHwPlayer.apk`（4,424,302 字节；`libmsff.so` 4,241,096，NEEDED 无系统 libssl）。Java 异步 URL/取消/Surface 生命周期；HTTPS 无信任锚返回 UNSUPPORTED，不关验证。宿主 mock 实测 UDP/RTP 喂入播放器各层计时且 display=0；TS/FLV 与 `test_ff_backend` 全矩阵（HLS PNG 分片、TLS、取消、无 CA 拒绝）通过。OnFrameRendered 仅作分列证据（API 33+ dlsym），不写入 display，不挂钩 Choreographer。只读核查 AVD：无设备、无本用户 KVM 权限，未启动共享 AVD。未 commit/push。详见 `docs/HW_ANDROID.md` §10 与 `docs/UNVERIFIED.md`。

## 2026-11-21 16:53 CST — 帧守恒审查、直播去向解释与 JNI 生命周期（宿主自测，非正式验收）

授权：完整实现继续；本轮限定代码审查与受控验证。不 CloudAgent、不 machineId、不 commit/push、不覆盖已交付 `MsMedia-deliver-20261121-1639/`、不重试 KVM/模拟器、不切 TLS、不加壳。

结论：先前 UDP `in=25 submitted=15`（本轮同类 16/9）与 RTP `25/13` 是 `short.ts` 单 IDR（仅 index 0）上中途入 GOP 的合法关键帧门闩，`drop_nokey` 解释全部缺口，`preroll=0`、本跑 `backpressure=0`。先 listen 再发的 UDP-from-start 为 25/25。守恒断言与已知 GOP/twice.ts skip-10 回归已加入 `test_hw_player`。

缺陷修复：LIMIT 不计 `in`；未消费事件不释放；EOS 与未 start 的 drive 不再把 `last_error` 标 STATE。JNI 去掉 UI 线程 join open、排队换 URL、`op` 锁串行化 Surface/flush/feed、销毁后清空引用。

回归（ASan/UBSan，2026-11-21 16:52 CST）：`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` 全过。计时见 `docs/HW_ANDROID.md` §11 与 `docs/UNVERIFIED.md`。mock 输出不是出画。有改动且回归通过后另存新源码包/APK，不覆盖 16:39 归档。HEAD 仍 `1092bec`。剩余依赖：真机硬解与出画、APK 设备联调、RTMPS、el7/四平台。

## 2026-11-21 17:03 CST — 请求端到端计时与 nativePlayPath 异步（宿主自测，非正式验收）

授权：t90u 完整实现内修复明确缺陷。不 CloudAgent、不 machineId、不 commit/push、不覆盖已交付归档、不重试 KVM/模拟器。

修复：`nativePlayPath` 与 `nativeOpenUrl` 共用异步队列，UI 不再同步读整文件；单一工作泵。新增 `ms_hw_player_mark_request` 与 `t_req_*`；保留并准确命名 `t_first_*` 为管线局部计时。受控 150ms+300ms IDR 等待与延迟 250ms 发送的 UDP-from-start 断言请求端到端包含注入等待。先前约 0.28ms 的直播数字不含 open/probe 前等待，已在文档纠正。display 仍为 0。

回归（ASan/UBSan，2026-11-21 17:02 CST）：`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` 全过。HEAD 仍 `1092bec`。设备出画与 1 秒目标仍未验。

## 2026-11-21 17:14 CST — 有界快速起播：拆分 4~5 秒探流并保留稳健默认（宿主自测，非正式验收）

授权：继续原完整实现，优化可独立的 open/probe 瓶颈。不 CloudAgent、不 machineId、不 commit/push、不覆盖已交付 1639/1654/1704 归档、不重试 KVM/模拟器、不扩其它平台编码。

真正瓶颈：受控 UDP/RTP 约 4~5 秒的 `request_e2e` 来自官方默认 `analyzeduration`（5s）下 `avformat_find_stream_info` 等待不完整的 AAC 参数，再被 `io_timeout` 打断；`read_timeout` 不是起播预算。同条件拆分：UDP DEFAULT mid-GOP `find_info=3815.432ms`、submit=4052.227ms；FAST `find_info=585.341ms`、submit=863.851ms。UDP-FAST from-start（注入 250ms）submit=408.332ms（对照 17:02 默认探流 5060.988ms）。HTTP TS/FLV 本机控制服务 FAST submit 约 2.3ms（VOD 速率，非 `-re`）。不预读、不超速、不剪掉关键帧等待。仍 mock，display=0，不声称 1 秒出画。

改动：加法 ABI 仍为 1。`MS_FF_PROBE_DEFAULT` 保持官方探测；`MS_FF_PROBE_FAST_START` 有界（400ms/512KiB/48 packets/500ms deadline，非 0，不关音轨）。JNI 播放路径用 FAST；`test_ff_backend` 仍 DEFAULT。`mark_request` 前移到入队，`mark_worker_start` 区分 worker；CA 缓存/后台导出；destroy 2 秒有界等待后再 join。

回归（ASan/UBSan，2026-11-21 17:13 CST）：`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` 全过。HEAD 仍 `1092bec`。下一具体阻塞：真机/AVD 硬解与出画、APK 设备联调、RTMPS、el7/四平台；音画同步/缺轨/配置变化仍为后续 fallback。

## 2026-11-21 17:40 CST — FAST 起播稳定性边界（探测恢复、CSD/epoch、取消/长 GOP、有界销毁）（宿主自测，非正式验收）

授权：t90u 完整原授权内完成本阶段快速起播稳定性边界，不扩大其它平台编码。17:16 源码/APK已交付且不得覆盖。不 CloudAgent、不 machineId、不 commit/push、不 UPX、不改 TLS 校验、不碰共享 AVD/用户电脑。

已实现（宿主 ASan/UBSan）：FAST deadline 软 interrupt 可恢复并继续读（`probe_incomplete=1`），不把部分失败当满成功；上下文不可用或探测失败且硬错误则返回可诊断错误并保留计时；探测已识别流后的 pb 硬错误（短 RTMP EIO）保持打开。缺音轨不关音频，文件路径仍发 TRACK；CSD 变化按 epoch 重配并等关键帧；探测取消 `CANCELED` 101.433ms，不可达 `TIMEOUT` 805.140ms；真实 2s GOP mid-join request_e2e submit=2791.105ms（≥1000ms）。`nativeDestroy` 2 秒等待后改为 detach + 线程自释，禁止超时 free 与 UI 无界 join。DEFAULT `test_ff_backend` 全过（RTMP events=36）。同条件：UDP-DEFAULT mid-GOP submit=4020.069ms（25/15/10）；UDP-FAST mid-GOP submit=771.741ms（25/17/8，find=585.068ms）；from-start 250ms 注入 submit=412.977ms。仍 mock，display=0。

明确未做：音频输出与 A/V 同步；公共事件无 sample_rate/width/height；中途 AU 无 CONFIG 的带内 SPS；1 秒出画；真机/AVD 硬解与销毁；不可中断 I/O 上放弃线程可能活到进程退出。不得以仅视频 H264 宣称完整有声播放器。

回归：`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` exit 0，无 sanitizer 诊断。HEAD 仍 `1092bec`。下一具体阻塞：真机硬解与出画、APK 设备联调、RTMPS、el7/四平台，以及音频输出/同步。

## 2026-11-21 18:02 CST — AAC 输出接口、拒绝语义与宿主 A/V 同步（宿主自测，非正式验收）

授权：继续 t90u「完整的要做」。用户要的是完整播放链路而非无声视频。本阶段只在既定 Android / H264+AAC 范围补齐 AAC 输出与同步，不扩平台/编码。不 CloudAgent、不 machineId、不 commit/push、不 UPX、不改 TLS/顶级 license、不碰共享 AVD/用户电脑，不覆盖 1639/1654/1704/1716/1742 与 `build/release`、历史 v7a size-opt。

已实现（宿主 ASan/UBSan）：可选 `ms_hw_audio`（MediaCodec+AudioTrack 兼容路径的接口与宿主 mock）；公共 `ms_aac_asc_parse`；默认拒绝软件 AAC（系统仅软解则诊断 `UNSUPPORTED`，视频 fallback，统计不冒充硬件）；时钟用已播放帧而非排队字节；视频调度含快速首帧、early 保持（+80ms）、late 呈现（-200ms）、无音频时钟立即呈现；无 ASC 的音频 AU 丢弃以免卡住复用泵。`nativeDestroy` 放弃线程帽=2，满帽拒绝新建并可诊断，不只靠 detach。`test_hw_backend` 覆盖配置/PCM 变化/背压/部分 write/EOS/flush/取消；`test_hw_player` 覆盖同步与拒绝及线程帽。回归：`test_core`、`test_ts_scan`、`test_ff_backend`（DEFAULT，RTMP events=36）；FAST 长 GOP mid-join request_e2e submit=2782.678ms；UDP-FAST mid-GOP 25/17/8 submit=713.762ms。display=0。

明确未做：真机出声与设备同步；许多设备可能只有软件 AAC，按政策将拒绝，本轮不放开；公共事件仍无 sample_rate/width/height；中途 AU 无 CONFIG 的带内 SPS；1 秒出画；APK 未安装。不得以宿主 mock 或仅视频 H264 宣称完整有声播放器。

非正式 APK（NDK r29 API 21 v7a）：`MsHwPlayer.apk` 4,448,878 字节，SHA-256 `d3448c333a0658e9d928ef46f9d7094ff44156d97659f05c23fecc3a24e9d06a`；`libmshw.so` 57,132；打入 `libmsff.so` 4,244,104（无系统 libssl）。归档 `/workspace/MsMedia-deliver-20261121-1802/`。HEAD 仍 `1092bec`。当时下一步中，无独立 CONFIG 的带内 SPS/PPS 仍是原授权内可独立修复的缺口（已在 18:19 CST 补齐）。真机硬件 AAC 可用性仍未验；硬解 only 政策未放开，仅软件 AAC 设备将无声。

## 中途 AU 内 SPS/PPS 与音频时钟复核（2026-11-21 18:19 CST）

已实现（宿主 ASan/UBSan）：公共有界 `ms_h264_ps_parse`/`equal`/`pack`（Annex B、AVCC 样本、avcC）；统一在 `ms_hw_player` 增量提取带内配置并在相应 AU 前应用。禁止新 SPS 与旧 PPS 拼接；不成对则消费丢弃并有界等待 16 个 AU，超限放弃 pending；PS 超限 `OVERFLOW`，畸形 AVCC 不应用配置。真实变化才关解码器并 `epoch++`，等新配置可解关键帧；相同 CSD 不计重建。官方 `CODEC_CONFIG` 仍按原始字节比较。未在 `libmsff` 复制路径，ABI 仍为 1，未加公共 `sample_rate`/`width`。合成与真实受控 TS/FLV（跳过独立 CONFIG）覆盖初始化与中途切换；背压不丢配置。音频复核：设备时钟按 `AudioTrack.getPlaybackHeadPosition` 无符号 32 位并处理回绕；`write` 为 PCM 字节，部分写入进 leftover；flush 先 pause 再归零；无硬件 AAC 拒绝语义不变。

回归：`test_core`（含 `test_h264_ps_parse_unit`）、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（合成 in-band + 真实 TS `in=48/48 inband=2 epoch=2`、FLV `in=50/50 inband=2 epoch=2`）、`test_ff_backend`（DEFAULT，RTMP events=36）、`test_net`。FAST 长 GOP mid-join request_e2e submit=2807.005ms；UDP-FAST mid-GOP 25/17/8 submit=713.118ms。display=0。

明确未做：真机硬解/出画/1 秒首帧/音频实听；许多设备可能只有软件 AAC，按政策将拒绝；公共事件仍无 sample_rate/width；有界等待内始终不成对则放弃 pending（规定行为）；RTMPS、el7/四平台、APK 未安装。不得以宿主 mock 宣称完整有声或已出画播放器。

非正式 APK（NDK r29 API 21 v7a）：`MsHwPlayer.apk` 4,452,974 字节，SHA-256 `ad9efc68e69e3913698b45586c8646202bdc8c523227e33f141746727e6c8a09`；`libmshw.so` 60,024；打入 `libmsff.so` 4,244,104（无系统 libssl）。归档 `/workspace/MsMedia-deliver-20261121-1819/`。不覆盖 1639/1654/1704/1716/1742/1802 与 `build/release`、历史 v7a size-opt。HEAD 仍 `1092bec`。剩余依赖是未连接设备上的硬解、出画、1 秒与实听，以及硬解 only 下可能普遍无声；不是把已授权缺口写成「另行决定」。

## 系统 AAC 有界例外、非正式 arm64-v8a 与受控 RTMPS（2026-11-21 18:36 CST）

授权：用户「继续」（t92u），仍在 t90u「完整的要做」范围内。无验收设备（adb 空，用户电脑 disconnected，无 `/dev/kvm`），不启动共享 AVD，不碰用户电脑，不停工空等。只在 Bot `/workspace/MsMedia` 做可独立完成的工作。不扩 HEVC/字幕/其它平台/P4P5。不 CloudAgent、不 commit/push、不 UPX，不覆盖 1639..1819 与 `build/release`、历史 v7a size-opt。

已实现（宿主 ASan/UBSan）：明确 `ms_hw_aac_policy`（`MS_HW_AAC_POLICY_HARDWARE_ONLY=0` / `MS_HW_AAC_POLICY_ALLOW_SYSTEM=1`）与 `ms_hw_player_opts.allow_system_aac`；不扩大中间的 `ms_hw_create_opts`；不把 `allow_software` 当作系统 AAC 开关。库默认仍为 0。仅 AAC 可选用系统软件名（`omx.google.` / `c2.android.` / `c2.google.`）+ AudioTrack；成功时「系统 AAC，非硬件」，`audio_is_hardware=0`，`audio_system_decode_is_not_hardware=1`。视频软解、非系统软解、HE-AAC、FFmpeg 软解（`CONFIG_DECODERS=0`）仍拒绝。测试 APK 显式打开系统 AAC。宿主 mock 覆盖默认拒绝、显式允许、FOREIGN_SW 拒绝、生命周期/背压/flush/时钟、无音频 fallback。非正式 arm64-v8a 复用 size-opt 前缀重链；测试 APK 同时带 v7a+v8a。受控 RTMPS：无 CA 拒绝、本地 CA 拉流 events=36、主机名不匹配非 OK；校验未关。

回归：`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（`test_system_aac_policy`；中途 CSD TS `in=48/48 inband=2 epoch=2`、FLV `in=50/50 inband=2 epoch=2`；FAST 长 GOP submit=2794.318ms；UDP-FAST mid-GOP 25/17/8 submit=723.643ms）、`test_ff_backend`（受控 RTMPS）、`test_net`。display=0。

明确未做：真机硬解/出画/1 秒首帧/音频实听；库默认在仅系统软解 AAC 的设备上仍无声；测试 APK 的系统 AAC 路径尚未在设备上听到。RTMPT、公网/设备 RTMPS、el7/四平台、APK 未安装。不得以宿主 mock 或「系统 AAC」状态行宣称完整有声或已出画播放器。

非正式 APK（NDK r29 API 21，v7a+v8a）：`MsHwPlayer.apk` 9,921,347 字节，SHA-256 `0963764b63ad651f216873fe6fee7ea9b9a59a540bee53fd14ca65c0dd853ce9`；v7a/v8a `libmshw.so` 62,112 / 71,480；打入 `libmsff.so` v7a 4,244,104、v8a 5,250,888（无系统 libssl）。归档 `/workspace/MsMedia-deliver-20261121-1838/`。不覆盖 1639/1654/1704/1716/1742/1802/1819 与 `build/release`、历史 v7a size-opt。HEAD 仍 `1092bec`。剩余依赖是未连接设备上的硬解、出画、1 秒与实听。

## 2026-11-21 20:10 CST — P4 音轨枚举/切换与文本字幕切片（宿主自测，非正式验收）

授权：t94u「都完成」、t95u「全部都做好再给我」。本轮只做可独立执行的 P4 音轨/字幕完整切片，然后返回真实结果；不因无设备停止开发。禁止 CloudAgent、machineId/用户电脑、共享 AVD、commit/push、UPX、覆盖旧归档。不另发阶段交付包；内部测试 APK 可重建。自测授权不等于正式验收。

设计选择（先核对 DESIGN §10 / ROADMAP P4 再落地，加法 ABI，不平行重写）：

- 统一 C ABI：`MSMEDIA_ABI_VERSION` 与 `MSMEDIA_HW_ABI` 仍为 1。新 API 与 `ms_hw_player_stats` 尾部字段只追加。`ms_event` 无 `struct_size`，字幕 cue 用已有 `pts`=开始、`dts`=结束（同一 time_base），`data` 为 UTF-8。
- 轨道目录为会话/播放器内部状态，懒分配，不挤占固定会话对象。默认：第一路视频与第一路音频选中，字幕关闭。用户「关闭」不被 forced 标记覆盖。未知 id 返回 `INVALID_ARG`。
- 同容器切轨不重开 Source；切换 flush 旧音频会话与 ASC，等待新 `CODEC_CONFIG` 后重建时钟并增加 `selection_epoch`。无音轨/关闭音轨走既有无音频回退。HLS 备选 rendition（另一 URL）本切片 `UNSUPPORTED`。
- 字幕解析走自写有界路径（`src/sub/ms_sub.c`）：外挂 SRT/WebVTT（UTF-8 BOM、跳过 VTT STYLE/REGION/NOTE、去简单标签但不宣称样式）；tx3g/mov_text 提取 UTF-8/UTF-16 纯文本。上限：单条 1024、条数 2048、文档 256 KiB；整份超限 `OVERFLOW` 且不保留半成品。位图（DVB/PGS 等）与 ASS/SSA 可枚举，选择播放 `UNSUPPORTED`。不打开 FFmpeg 解码器（`CONFIG_DECODERS=0` 未改；当前裁剪也不含 srt/webvtt demuxer）。`libmsff.so` 将同一份 `ms_sub.c` 隐藏静态编入，仍不 NEEDED `libmsmedia`。
- 视频硬解 only 与上轮系统 AAC 有界例外保持：库默认 `allow_system_aac=0`；测试 APK 仍显式允许并写明「系统 AAC，非硬件」。
- Android：JNI `nativeListTracks` / `nativeSelectTrack` / `nativeCurrentCue` / `nativeLoadSubtitle`（同时驱动 `ms_ff_*` 与播放器）；Activity 在 Surface 上叠加 TextView，并提供 Tracks / Next Audio / Next Sub / Load SRT / Sub Off。不记录私密 URL 或字幕路径。

宿主回归（2026-11-21 20:07–20:10 CST，Asia/Shanghai，`./build.sh debug`，gcc 14.2.0，ASan/UBSan；`OPENSSL_CONF` 指向钉选 3.5.9 配置，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`）：

- `test_core`（含 `test_p4_tracks_and_subs`）、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（含 `test_p4_audio_switch_and_subs` 与既有系统 AAC / 中途 CSD / FAST 等）、`test_ff_backend`（含 `test_p4_ff_tracks`：双 AAC MP4 `a0=1 a1=2` 切换后仅新轨 AU，以及既有受控 RTMPS）、`test_net` 均 exit 0。未把 SKIP 写通过。
- 这是宿主 mock / 事件路径，**不是设备听见或看见**。`output_is_not_display` 仍为 1。

非正式 `./build.sh android-hw`（NDK r29 API 21，v7a+v8a，由既有 size-opt 前缀重链，不重编 OpenSSL/FFmpeg）：内部工作副本 `build/android-hw/MsHwPlayer.apk` **9,990,979** 字节，已签名未安装，SHA-256 `24f5a101bacd27e836dc7e122f88eecff7b94b18932581a6bb7e01b2787eeea3`。打入 `libmsff.so` v7a 4,257,688、v8a 5,261,344（相对 1838 的增量来自隐藏编入的字幕解析，不是又编了一遍 FFmpeg）；NEEDED 仍为 libz/libm/libdl/libc，无系统 libssl/libcrypto，无 libmsmedia。v7a/v8a `libmshw.so` 69,868 / 80,528；`libmsmedia.so` 140,928 / 144,664。历史 v7a size-opt `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，未覆盖。本轮**不**写入新的 `MsMedia-deliver-*` 目录，不覆盖 1639–1838 与 `build/release`。HEAD 仍 `1092bec`，未 commit/push。

明确未做 / 下一可继续块：P4 倍速与快进快退（相对 seek）、其余编码（HEVC 等）的证据级识别与组帧；位图/ASS 渲染；HLS 备选 rendition；P5 AI 字幕；四平台/el7/真机硬解、出画、实听与测试 APK 系统 AAC 路径的设备验证。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 20:25 CST — P4 倍速与相对 seek/快进快退（宿主自测，非正式验收）

授权：t94u「都完成」、t95u「全部都做好再给我」。本轮继续既定 P4，只做倍速与相对/绝对 seek 完整切片，并整理未闭环矩阵；不另发阶段包。禁止 CloudAgent、machineId/用户电脑、共享 AVD、commit/push、UPX、覆盖旧归档。自测授权不等于正式验收。

设计（先核对 DESIGN §11 / 既有时钟、seek、音频、字幕结构，加法 ABI）：

- 平台音频能力写入 `ms_hw_rate_caps`；旧 API 或不支持的速率诚实 `UNSUPPORTED`，不把只改视频时钟称为音画同步倍速。
- pause/resume 冻结/恢复呈现与音频时钟；改速、seek/flush、EOS、切音轨保持连续性（改速不清零时钟；seek 保留速率并在新音频会话重套用）。
- 相对 seek：溢出安全、负值钳 0、已知时长钳位；不可 seek 直播 `NOT_SEEKABLE`；负绝对时间 `INVALID_ARG`。seek 后 flush、清字幕、等关键帧并保留 CSD；时间轴相对会话 origin。
- `ms_ff_seek` 用 `av_seek_frame`（BACKWARD 关键帧），`CONFIG_DECODERS=0` 不变。
- JNI 泵循环处理 pending seek，不阻塞 UI；有 libmsff 时 PlayPath 走 `file:`。

宿主回归（2026-11-21 20:22–20:25 CST，Asia/Shanghai，`./build.sh debug` 后直接跑已编 ASan/UBSan 二进制，gcc 14.2.0；`OPENSSL_CONF` 指向钉选 3.5.9 配置，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`）：

- `test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（含 `test_p4_rate_and_seek` 与既有 P4 切轨/系统 AAC / 中途 CSD / FAST / 受控直播入口）、`test_ff_backend`（含 `test_p4_ff_seek` 与既有 `test_p4_ff_tracks`、受控 RTMPS）、`test_net` 均 exit 0。未把 SKIP 写通过。
- `./build.sh android-hw` 非正式双 ABI 测试 APK：10,011,459 字节，SHA-256 `0cd7957223593c3a2e5ae9e45fd8585d590f8e202ffbbba2f8d07656dfb80f39`，已签名未安装。包内 v7a/v8a `libmsff.so` 4,258,944 / 5,262,872（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 80,296 / 91,792；`libmsmedia.so` 140,928 / 144,664。FFmpeg 前缀未强制重建。历史 1639–1838 与 size-opt v7a `libmsff.so` 未覆盖。HEAD 仍为 `1092bec`，未 commit/push。

未闭环矩阵写入 `docs/FEATURE_MATRIX.md`：HLS 备选 rendition、ASS/位图渲染、其余既定编码、四平台/设备验收属于承诺尚未兑现或设备未验；倒放、FFmpeg 软解、把渲染回调当做出画属于产品刻意边界。返回 `UNSUPPORTED` 不得标为功能完成。

明确未做 / 下一可继续块：其余编码（HEVC 等）的证据级识别与组帧；HLS 备选 rendition 音轨；位图/ASS 渲染；P5 AI 字幕；四平台/el7/真机硬解、出画、实听与测试 APK 系统 AAC / 倍速听感的设备验证。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 21:10 CST — P4 HEVC 实质路径（宿主自测，非正式验收）

授权：t94u「都完成」、t95u「全部都做好再给我」。本轮先做可验证的一组实质功能：HEVC 识别以外的完整 AU/CSD、中途 VPS/SPS/PPS、seek 关键帧门控、Android NDK 与 JNI 配置/提交选择及宿主 mock 证据。不把只加枚举写成支持。其余既定编码（H.263、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1，以及 AC-3/MP3/MP2/Opus）未在本轮堆未测代码。HLS 备选音轨与 ASS/位图仍为原有欠账，未标完成。不另发阶段包。禁止 CloudAgent、machineId/用户电脑、共享 AVD、commit/push、UPX、覆盖旧归档。TLS/主机名校验不变。自测授权不等于正式验收。

实现要点：

- 加法 ABI：`MS_SAMPLE_FMT_HVCC`、`ms_hevc_ps_parse` / `equal` / `pack`、`ms_hevc_au_inspect`；`MSMEDIA_ABI_VERSION` 仍为 1，既有 struct 不增长。
- 有界解析：Annex B / hvcC，成套 VPS+SPS+PPS 且 id 匹配才 `triple_complete`；不拼接新 SPS 与不匹配旧 PPS/VPS；超大 NAL 为 `OVERFLOW`；avcC 不得当 hvcC。
- `libmsff`：MP4 标 `HVCC`、TS 标 `ANNEXB`；IRAP 或 `AV_PKT_FLAG_KEY` 为关键帧；中途参数集用载荷签名，避免包装差异导致每包重发 `CODEC_CONFIG`。`ms_hevc.c` 隐藏静态编入 libmsff，不 NEEDED libmsmedia。
- 播放器与硬解：带内三联体、seek 后门控（可用 IRAP，不单靠标志）、mock 可配置/提交但标明不是设备。Android MIME `video/hevc`，csd-0 为带起始码的参数集。无硬件时诚实拒绝。AAC 系统例外不扩展。
- 自写 TS：PMT `0x24` 仍为容器标识，`ms_codec_query(HEVC)=CONTAINER_ID`。
- FFmpeg 裁剪只开 `hevc` parser 与 `hevc_mp4toannexb`，`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。真实重建宿主与 Android size-opt 前缀并对照体积；不把旧 lib 打进 APK。历史 `build/android-v7a/size/libmsff.so.1.0.0` 未覆盖。

宿主回归（2026-11-21 21:00–21:04 CST 起，Asia/Shanghai，`./build.sh test`，gcc 14.2.0，ASan/UBSan；`OPENSSL_CONF` 指向钉选 3.5.9 配置，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`）：

- `test_core`（含 `test_hevc_ps_parse_pack_bounds` 与音轨/字幕回归）、`test_ts_scan`、`test_hw_backend`（含 `test_hevc_mock_configure_submit`）、`test_hw_player`（含 `test_hevc_inband_and_seek_gate` 与 P4 切轨/倍速/seek 回归）、`test_ff_backend`（含 `test_ff_hevc_mp4_ts` 以及既有 `test_p4_ff_tracks` / `test_p4_ff_seek` / 受控 RTMPS）、`test_net` 均 exit 0。未把 SKIP 写通过。
- 这是宿主解析与 mock 提交，**不是设备硬解、出画或出声**。

体积对照（重建后，非正式）：

- 宿主 `libavcodec.a` 8,634,164 → 9,924,946；debug `libmsff.so.1.0.0` unstripped 16,169,624 → 16,792,928。
- Android size-opt `libavcodec.a` v7a 4,918,418 → 5,689,806，v8a 7,264,692 → 8,384,180。
- 内部 APK `build/android-hw/MsHwPlayer.apk` **10,163,011** 字节，SHA-256 `908db00b6f8ef4caaff9545ffdf1aa1415c72bf6f3053cfc8592dfb055692ecb`，已签名未安装。包内 v7a/v8a `libmsff.so` 4,326,832 / 5,326,712（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 84,280 / 95,856；`libmsmedia.so` 146,420 / 150,520。

历史 1639–1838 与 size-opt 对照产物未覆盖。HEAD 仍 `1092bec`，未 commit/push。

明确未做 / 下一可继续块：HLS 备选 rendition 音轨；ASS/位图渲染；其余既定编码（H.263、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1，以及 AC-3/MP3/MP2/Opus）的证据级 AU/CSD/硬解查询或诚实拒绝；自写 TS 的 HEVC AU 装配；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 21:29 CST — P4 HLS 备选 AUDIO rendition（宿主自测，非正式验收）

授权：t94u「都完成」、t95u「全部都做好再给我」。本轮补齐既定多音轨欠账中的 HLS 备选 rendition 音轨。复用官方 FFmpeg 9.1 HLS 的 AVProgram/AVStream/rendition，不平行写一套 master 清单调度。不扩展自写 `ms_hls`（master 仍明确拒绝）。`CONFIG_DECODERS=0` 未改，无 FFmpeg 软解/转码，未一刀切开模块，未重建 FFmpeg 前缀。系统 AAC 例外未扩展到其它音频。无 CloudAgent、machineId、共享 AVD、commit/push、UPX、新阶段归档。TLS/主机名校验不变。

实现要点：

- 跨流唯一 `track_uid`：HLS 把 `st->id` 设成 playlist 下标，会与 `index+1` 冲突；枚举/选择/事件用同一套稳定 id。
- 元数据：语言走 `language`；NAME 兼容 FFmpeg 写入的 `comment`；DEFAULT/disposition 用于默认选择。
- 选择：`ms_ff_select_track` 对音轨只武装 pending，读到新轨包后才提交（停旧输出、丢旧槽、重发 CSD、抬 `selection_epoch`，不抬 `timeline_epoch`、不重开视频）。失败撤回 pending，不拆会话，list 仍显示旧轨。关闭（on=0）立即提交。本调用不做网络 I/O。
- `AVStream->discard` 交给官方 `recheck_discard_flags` 启停 playlist。诚实限制：FFmpeg 只在首次读包时停止不再需要的 playlist，之后本层保证停止向外输出，旧 rendition 仍可能被底层读取。
- MPEG-TS AAC 常无 extradata：从 ADTS 头拼 2 字节 AudioSpecificConfig（与 `ms_adts` 子集一致），切换后强制重发。
- VOD HLS：`pb` 无字节 seek 时，若 demuxer 为 `hls` 且有正时长，则 `ms_ff_is_seekable` 为真，走既有 `av_seek_frame`。
- JNI：工作线程在跑时 `nativeSelectTrack` 只入队，worker 在 `pull` 前应用并在 feed 前把已提交音轨同步到播放器；网络仍在 worker。

测试（本地受控 HTTP + file HLS，不是只断言 API）：

- `test_ff_hls_alt_audio`：master + 外链英/中 AAC（48 kHz 立体声 vs 44.1 kHz 单声，CSD 不同）、相对 URI、查询串、默认选择、来回切换、关闭再启、非法 id 不改会话、VOD seek 后仍为新轨、畸形 GROUP-ID 不虚构音轨、缺失非默认 playlist 时本次 header 仍打开（tracks=2）、有界队列 LIMIT、cancel。
- `test_ff_hls_alt_audio_player`：file HLS 喂入宿主 mock 播放器，切轨后 `audio_switch_count` 增加、视频 `epoch` 不变、可 pause/resume/改倍速。
- 官方 `./build.sh test`（gcc 14.2.0 ASan/UBSan）exit 0，含 core/net/ts、HEVC、字幕、倍速/seek 回归。非正式 `./build.sh android-hw` 双 ABI 重链（未改 FFmpeg 前缀，未覆盖历史 size 产物）。

内部 APK `build/android-hw/MsHwPlayer.apk` **10,163,011** 字节，SHA-256 `f8fb3f7485e5ba10cc83a6eacc8522c206bb5a51570401945876f41f4a7dd2df`，已签名未安装。包内 v7a/v8a `libmsff.so` 4,328,516 / 5,328,272（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 85,020 / 96,728；`libmsmedia.so` 146,420 / 150,520。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：ASS/位图字幕渲染；HLS 字幕或视频 rendition；其余既定编码（H.263、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1，以及 AC-3/MP3/MP2/Opus）；自写 TS HEVC AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 21:49 CST — P4 ASS/SSA 文本字幕（解析与布局模型，宿主落地 / 设备未验）

授权：t94u「都完成」、t95u「全部都做好再给我」。本轮补既定字幕欠账中的 ASS/SSA 文本路径。复用 `ms_sub` 统一时间轴/轨道/seek/倍速，不平行重写。加法 ABI，`MSMEDIA_ABI_VERSION` 仍为 1，旧 `struct_size` 只填到 `text_size`。未引入 libass，未改顶层许可证。`CONFIG_DECODERS=0` / `CONFIG_GPL=0` / `CONFIG_NONFREE=0` 未改。无 CloudAgent、machineId、共享 AVD、commit/push、UPX、新阶段归档。TLS/主机名校验不变。不向用户发送阶段包。

实现要点：

- 公共模型：`ms_sub_cue` 追加 style/cap/layer/对齐/字号/颜色/边距/位置/PlayRes/有界 span；`MS_SUB_FORMAT_ASS/SSA`；`ms_sub_doc_set_ass_header`、`ms_sub_ass_parse_event`、`ms_sub_doc_clear_cues`（seek/关轨保留样式表）。
- 解析：Format/Style/Dialogue、转义与 `\N`/`\n`/`\h`、常见覆盖标签、SSA 对齐映射。事件栏 Margin=0 沿用 Style。忽略 `[Fonts]`/`[Graphics]`，不执行脚本、命令或附件。畸形时间跳过，超长字段与非法 UTF-8 有界。
- 未支持特效（`\move`/`\t`/`\fad`/`\clip`/karaoke/绘图/Effect 栏等）置 `cap_flags`+`DEGRADED`，可见文本去标签，不称为完整渲染。
- 播放路径：ASS 可选择；位图仍 `UNSUPPORTED`。`libmsff` 在首包前发出 ASS `CODEC_CONFIG`（extradata 头），事件保持原始载荷供播放器再解析。本裁剪未开 matroska/ass demuxer。
- Android：轻薄 JNI `nativeFillCues` + 多层 TextView 应用颜色/字号/粗斜体/对齐/边距/`\pos` 比例与 span；不再把样式标签原样显示。此处只有编译+逻辑，**不是视觉验收**。

测试：

- `test_ass_ssa_parse`、`test_p4_ass_inband`；回归既有 SRT/音轨/倍速/seek/HLS/HEVC。
- 官方 `./build.sh test`（gcc 14.2.0 ASan/UBSan）exit 0。非正式 `./build.sh android-hw` 双 ABI 重链（未改 FFmpeg 前缀，未覆盖历史 size 产物）。

内部 APK `build/android-hw/MsHwPlayer.apk` **10,236,739** 字节，SHA-256 `497196215dedd97e027fe060ba4dc652c2c6f790319ea0285c4d13ea78425531`，已签名未安装（约 2026-11-21 21:48:58 CST）。包内 v7a/v8a `libmsff.so` 4,346,244 / 5,344,800（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 85,740 / 97,496；`libmsmedia.so` 164,760 / 167,808。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：完整 ASS 渲染（字体/动画/绘图/合成）与位图字幕；若要实开 MKV 内嵌 ASS 需最小追加 matroska demuxer；HLS 字幕或视频 rendition；其余既定编码（H.263、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1，以及 AC-3/MP3/MP2/Opus）；自写 TS HEVC AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 22:16 CST — P4 MPEG-2 Video 与 MPEG-4 Part 2（宿主解析/mock，设备未验）

在已授权 P4「其余编码」块先完成 MPEG-2 Video 与 MPEG-4 Part 2 的实质可验切片：通用 `ms_video_*` 配置/AU/关键帧模型，libmsff 完整压缩 AU + 真实 CSD/PTS-DTS（含 B 帧）、seek 后关键帧门控、中途配置变化，统一 player 与 Android NDK/JNI 查询-配置-提交（`video/mpeg2`、`video/mp4v-es`）。mock 拒绝软件 codec。产品 FFmpeg 仍 `CONFIG_DECODERS=0`，仅追加 `mpegvideo` 与 `mpeg4video` parser（校正组件名），宿主与 Android 两 ABI 前缀已重建。自写 TS 查询级别仍为 `CONTAINER_ID`。H.263 未做。未覆盖历史 size 产物，无阶段包，无 commit/push。

测试：

- `test_vbit_mpeg2_mpeg4_parse`、`test_vbit_mock_configure_submit`、`test_vbit_inband_and_seek_gate`、`test_ff_mpeg2_mpeg4`（MP4/TS：mpeg2/mpeg4 均为 cfg=2 au=20 key=6 B=12 pts_ne_dts=8，mid-cfg events=10，seek 后首 AU 为关键帧）。回归音轨/ASS/倍速 seek/HLS 外音轨/HEVC 及 core/net/ts。
- 官方 `./build.sh test`（gcc 14.2.0 ASan/UBSan）exit 0。非正式 `./build.sh android-hw` 双 ABI（已重建 FFmpeg 前缀，未覆盖历史 size 产物）。

内部 APK `build/android-hw/MsHwPlayer.apk` **10,359,619** 字节，SHA-256 `ad5490c050e361339b3143a694a6f01e629007e3617d27b7aa97bdc4a198931c`，已签名未安装（约 2026-11-21 22:16:27 CST）。包内 v7a/v8a `libmsff.so` 4,398,172 / 5,397,696（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 87,764 / 99,608；`libmsmedia.so` 169,520 / 172,608。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：H.263；AVS+、AV1 与 AC-3/MP3/MP2/Opus；完整 ASS 渲染与位图字幕；若要实开 MKV 内嵌 ASS 需最小追加 matroska demuxer；HLS 字幕或视频 rendition；自写 TS HEVC/MPEG-2/MPEG-4 AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 22:34 CST — P4 H.263（宿主解析/mock，设备未验）

在已授权 P4「其余编码」块，于 MPEG-2/4 之后完成 H.263 实质可验切片：复用 `ms_video_*`，加法 `MS_SAMPLE_FMT_H263`。配置来自短视频头 PSC/PTYPE，合法无 CSD，不捏造 csd-0。libmsff 对已开 3GP/mov 输出完整 AU、PTS、关键帧；seek 后首帧门控；player 中途 QCIF→CIF；Android NDK/JNI `video/3gpp` 空 CSD 配置/提交路径与 mock 拒绝软件 codec。只追加 `h263` parser，`CONFIG_H263_DECODER=0`、`CONFIG_DECODERS=0`。已覆盖基线标准尺寸 I/P；PLUSPTYPE/自定义尺寸与 `FLV1` 未覆盖。自写 `ms_codec_query(H263)` 仍为 `NONE`。未开 avi。fixture 编码不等于产品解码。无阶段包，无 commit/push，未覆盖历史 size 产物。

测试：

- `test_vbit_h263_parse`（QCIF/CIF/SQCIF 宽高、csd_size=0、PLUSPTYPE/截断/畸形、超长载荷不复制为 CSD、旧 ABI）、`test_vbit_mock_configure_submit`（空 CSD 配置与提交、拒绝捏造 CSD、拒绝 `mock.h263.sw`）、`test_vbit_h263_inband_and_seek_gate`、`test_ff_h263`（3GP：cfg=0 au=20 key=2 pts=20 pts_ne_dts=0 inband_cfg=20，seek 后首 AU 为关键帧，截断有界）。回归 HLS 切音轨、ASS/文本字幕、倍速 seek、HEVC/MPEG-2/4 及 core/net/ts。
- 官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）在重建宿主前缀（`CONFIG_H263_PARSER=1`）后 exit 0。非正式 `./build.sh android-hw` 双 ABI（已重建 FFmpeg 前缀，未覆盖历史 size 产物）。

宿主 `libavcodec.a` 13,463,652 → 13,548,772 字节；Android size-opt `libavcodec.a` v7a 7,745,322 → 7,803,002，v8a 11,317,754 → 11,401,434。内部 APK `build/android-hw/MsHwPlayer.apk` **10,363,715** 字节，SHA-256 `2615604a6e537fb61c256484d8912a767647fc0ba609d09d90e00f86c26ee7f1`，已签名未安装（约 2026-11-21 22:34:22 CST，Asia/Shanghai）。包内 v7a/v8a `libmsff.so` 4,399,244 / 5,398,864（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 88,168 / 100,064；`libmsmedia.so` 170,132 / 173,296。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：AV1 可验证切片；AVS+、VP9 与 AC-3/MP3/MP2/Opus；完整 ASS 渲染与位图字幕；若要实开 MKV 内嵌 ASS 需最小追加 matroska demuxer；HLS 字幕或视频 rendition；自写 TS HEVC/MPEG-2/MPEG-4/H.263 AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 22:48 CST — P4 AV1 可验证切片（宿主解析/mock，设备未验，不广称全部码流）

在 H.263 宿主与双 ABI 前缀验证之后，完成 AV1 实质可验切片：复用 `ms_video_*`，加法 `MS_SAMPLE_FMT_AV1`。合法 OBU/av1C、sequence header 宽高、TU/FU 边界、KEY/SWITCH、中途 SH 变化，以及合法无 CSD 路径证据（不捏造 av1C）。libmsff 对已开 mov/MP4 输出完整 AU/PTS/关键帧；seek 后首帧门控；Android NDK/JNI `video/av01` 与 mock 拒绝软件 codec。只追加 `av1` parser，无 dav1d，`CONFIG_AV1_DECODER=0`、`CONFIG_DECODERS=0`。自写 `ms_codec_query(AV1)` 仍为 `NONE`。未开 matroska。fixture 编码不等于产品解码。无阶段包，无 commit/push，未覆盖历史 size 产物。

测试：

- `test_vbit_av1_parse`（av1C 64/32、帧内 SH 无 CSD、key/inter、截断/畸形/leb 与 CSD 溢出、旧 ABI）、`test_vbit_mock_configure_submit`（av1C 与合法无 CSD 配置提交、拒绝 `mock.av1.sw`）、`test_vbit_av1_inband_and_seek_gate`、`test_ff_av1`（MP4：cfg=6 au=20 key=5 pts=20 inspect=20 av1c_bytes=96，seek 后首 AU 为关键帧，截断有界）。回归 HLS 切音轨、ASS/文本字幕、倍速 seek、HEVC/MPEG-2/4/H.263 及 core/net/ts。
- 官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）在重建宿主前缀（`CONFIG_AV1_PARSER=1`）后 exit 0。非正式 `./build.sh android-hw` 双 ABI（已重建 FFmpeg 前缀，未覆盖历史 size 产物）。

宿主 `libavcodec.a` 在 H.263 切片 13,548,772 之上变为 14,628,742 字节；Android size-opt `libavcodec.a` v7a 7,803,002 → 8,442,924，v8a 11,401,434 → 12,375,756。内部 APK `build/android-hw/MsHwPlayer.apk` **10,535,747** 字节，SHA-256 `a1191524964acef5972cdc1e8458c57618edaa6c7ebe0fe96c7266faa8a2e049`，已签名未安装（约 2026-11-21 22:48:47 CST，Asia/Shanghai）。包内 v7a/v8a `libmsff.so` 4,485,720 / 5,480,048（NEEDED 仅 libz/libm/libdl/libc）；`libmshw.so` 88,540 / 100,296；`libmsmedia.so` 172,072 / 175,040。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：AVS+、VP9 与 AC-3/MP3/MP2/Opus；完整 ASS 渲染与位图字幕；若要实开 MKV 内嵌 ASS 或更多 AV1 容器需最小追加 matroska demuxer；HLS 字幕或视频 rendition；自写 TS HEVC/MPEG-2/MPEG-4/H.263/AV1 AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。


## 2026-11-21 23:11 CST — P4 AC-3 可验证切片（宿主解析/mock，设备未验）；公共 ms_audio 已铺好

授权仍是 t94u「都完成」、t95u「全部都做好再给我」，不交阶段包。在 AV1 之后进入 AC-3 / MP3 / MP2 / Opus 块：先复用公共音频配置/完整帧检查/签名与既有切轨生命周期，而不是每编码平行复制。加法 ABI：`MS_SAMPLE_FMT_AC3`/`MPEG_AUDIO`/`OPUS`，`ms_audio_*`，`ms_hw_audio_config` 尾部 `samples_per_frame`/`pre_skip`/`codec_delay` 按 `struct_size` 读取。AC-3 按 ATSC A/52 校验帧长与 1536 样点，合法无 CSD，不捏造 AAC ASC，不把容器 extradata 当下发 CSD。接到统一 player 与 Android NDK/JNI `audio/ac3` 查询-配置-提交/AudioTrack 时钟；切轨/seek/倍速联动。硬解 only 不变：系统软件 AAC 仅显式例外，不以系统软件 AC-3 冒充硬解。无新 FFmpeg parser/decoder（既有 ac3 parser/demuxer + 已开 mpegts/mov 足够）；`CONFIG_DECODERS=0` 保持。自写 `ms_codec_query(AC3)` 仍为 `NONE`。fixture 编码不等于产品软解。mock 不是出声。无阶段包，无 commit/push，未覆盖历史 size 产物，本切片未再打非正式 APK。

测试：

- `test_abit_parse`（AC-3 48k/44.1k 帧长与合法无 CSD、截断/坏同步、拒绝把 ASC 当 AC-3 CSD、旧 ABI；并锁定 MP2/MP3 层区分与真实 OpusHead/dOps 装箱，尚不声称后三者容器切片完成）、`test_abit_mock_configure_submit`（空 CSD、1536@48k 时钟、拒绝软件 AC-3 即使打开系统 AAC 例外、拒绝捏造 ASC、旧 ABI）、`test_abit_ac3_inband_switch_and_clock`（未选中过滤、切轨清时钟、44.1k 重建、seek/倍速）、`test_ff_ac3`（MPEG-TS cfg=0 au=32 pts=32 inspect=32；MP4 同计数；seek 后仍有 AU；截断有界）。未测 raw `.ac3`：格式白名单不含 ac3 裸流 demuxer，不以白名单外路径凑数。回归 H.263/AV1、HLS 切音轨、ASS/文本字幕、倍速 seek、HEVC/MPEG-2/4 及 core/net/ts。
- 官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）exit 0。未重建 FFmpeg 前缀。非正式双 ABI `./build.sh android-hw` **本切片未跑**；当前内部 APK 仍是 AV1 切片产物。

宿主 debug 未剥离 `libmsff.so.1.0.0` 19,012,104 → 19,032,368 字节（隐藏链入 `ms_abit`/`ms_avcc`，前缀未变）。`libavcodec.a` 仍为 14,628,742。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：MP3/MP2（MPEG-TS 多帧 PES 需最小追加 `mpegaudio` parser 后再计真实 AU 边界；再官方回归）与 Opus（真实 OpusHead/pre-skip/preroll，必要时最小 ogg，实际容器测试）；随后非正式双 ABI 构建；AVS+、VP9；完整 ASS 渲染与位图字幕；若要实开 MKV 内嵌 ASS 或更多 AV1 容器需最小追加 matroska demuxer；HLS 字幕或视频 rendition；自写 TS HEVC/MPEG-2/MPEG-4/H.263/AV1/AC-3 AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。


## 2026-11-21 23:31 CST — P4 MP3/MP2/Opus 容器 AU 边界与平台接入；双 ABI 新 APK（宿主已落地，设备未验）

接续同一授权（t94u「都完成」、t95u「全部都做好再给我」），补完上轮未完成的 MP3/MP2/Opus 容器 AU 边界与统一 player/Android 接入，并做新的非正式双 ABI `./build.sh android-hw`。不把公共解析单独算完成，不交阶段包，不覆盖历史归档，无 commit/push。`CONFIG_DECODERS=0` 维持；系统 AAC 例外不扩至 MP3/MP2/Opus/AC-3。无该音频硬件须诚实 `UNSUPPORTED`，不能悄悄软件 fallback。时钟复用统一 `ms_audio` / player 生命周期；旧 `struct_size` 不加读尾部字段。完成一项先测再下一项。

MP3/MP2：最小追加 `--enable-parser=mpegaudio` 并重建宿主前缀。官方回归证实 MPEG-TS 由多帧 PES 变为按帧 AU（`frame_bytes==data_size`），Layer III/II 不混淆，采样率/PTS 递进，已开 MP4 路径仍对齐。Opus：追加 `--enable-parser=opus` 与 `--enable-demuxer=ogg`（许可/体积已核，未开 matroska），白名单加入 `ogg`；验证真实 OpusHead（含 dOps 装箱）、packet framing、TOC 帧样点、pre-skip=312 / codec delay / preroll，seek 与切轨后重置且不重复扣 pre-skip，提交/排队时钟不冒充扬声器时钟。**尾部 discard padding 未实现**，诚实列出。上轮 AC-3 一并编入本内部 APK，不得再把 22:48 AV1 APK 当作当前实现。

测试：

- `test_ff_mp3`（MPEG-TS cfg=0 au=40 pts=40 inspect=40 aligned=40 layer=40；MP4 同计数；seek 后仍分帧；cancel/截断有界）、`test_ff_mp2`（MPEG-TS cfg=0 au=42 pts=42 inspect=42 aligned=42 layer=42；MP4 同路径）、`test_abit_mpeg_mock_configure_submit`、`test_abit_mpeg_inband_switch_and_clock`。
- `test_ff_opus`（MP4 cfg=2 au=51 pts=51 inspect=51 aligned=51 pre_skip=312；Ogg 同计数；seek/cancel/截断；错误 Head 与 TOC code3 超限拒绝）、`test_abit_opus_mock_configure_submit`（dOps/Head、提交时钟不为 0 前不得推进扬声器时钟、960-312 只扣一次、flush 后重扣一次、拒绝软件与捏造 ASC、旧 ABI）、`test_abit_opus_preroll_switch_and_clock`。回归 AC-3/H.263/AV1、HLS 切音轨、ASS/文本字幕、倍速 seek、HEVC/MPEG-2/4 及 core/net/ts。
- 官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）在重建宿主前缀（`CONFIG_MPEGAUDIO_PARSER=1`、`CONFIG_OPUS_PARSER=1`、`CONFIG_OGG_DEMUXER=1`，`CONFIG_OPUS_DECODER=0`、`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`）后于 2026-11-21 23:26:52 CST exit 0。随后 `ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 重建双 ABI 前缀（不覆盖历史 size 产物），再 `./build.sh android-hw`。

体积（Asia/Shanghai）：宿主 `libavcodec.a` 在 AV1 切片 14,628,742 之上，先因 mpegaudio 至 14,758,360，再因 opus parser 至 15,080,200；`libavformat.a` 12,750,778 → 13,806,232（ogg demux 及 oggparse* 辅助，非 vorbis/flac 解码器）。宿主 debug 未剥离 `libmsff.so.1.0.0` 19,536,792 字节。Android size-opt `libavcodec.a` v7a 8,442,924 → 8,750,762，v8a 12,375,756 → 12,808,022；`libavformat.a` v7a 8,242,610 → 8,944,708，v8a 12,090,482 → 13,118,904。内部 APK `build/android-hw/MsHwPlayer.apk` **10,642,243** 字节，SHA-256 `b643061c37e828a1ee77311707460297747a4ce16e4f6aaca17081f1a1f1fd68`，已签名未安装（约 2026-11-21 23:30:25 CST）。包内 v7a/v8a `libmsff.so` 4,530,824 / 5,526,120（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia；unstripped 符号含 `ff_mpegaudio_parser`、`ff_opus_parser`、`ff_ogg_demuxer`，无对应 decoder）；`libmshw.so` 90,940 / 102,968；`libmsmedia.so` 176,380 / 179,752。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 12:17:30 CST）。HEAD 仍 `1092bec`。

明确未做 / 下一可继续块：AVS+、VP9；Opus 尾部 discard padding；E-AC-3；raw `.ac3` 白名单；完整 ASS 渲染与位图字幕；若要实开 MKV 内嵌 ASS 或更多 AV1/Opus 容器需最小追加 matroska demuxer；HLS 字幕或视频 rendition；自写 TS HEVC/MPEG-2/MPEG-4/H.263/AV1/AC-3/MP3 AU 装配；自写 `ms_hls` master；P5；四平台/el7/真机硬解、出画、实听。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-21 23:51 CST — Opus 尾部 discard padding 与 pre-skip / seek preroll 分项收口（宿主已落地，设备未验）

授权：继续「全部都做好再给我」统一交付内的音频正确性，先收口 Opus 尾部 padding 与 preroll，不开新编码。全部原有规则/禁止项不变。无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包。产品仍 `CONFIG_DECODERS=0`，系统 AAC 例外不扩展。

独立复核（RFC 7845、本树 FFmpeg 9.1 源码与受控探测、Android MediaCodec/MediaFormat 公开契约及 AOSP SoftOpus）：上轮「preroll 只扣一次，seek/切轨重置再扣一次」**混淆了三件事**，不能当作合同。正确分项见 `docs/FEATURE_MATRIX.md` 的 Opus 行：流开始 pre-skip ≠ 中途 seek preroll ≠ 负 PTS/平台 codec delay。Android 只设 csd-0/1/2，有 csd-1 时不设 encoder-delay，本层不与 codec 自动 trim 双扣 start/warmup，但必须在 PCM 进 AudioTrack 之前扣包尾 padding。时间轴不以压缩提交样点伪装播放头。

已接线：`fill_audio_skip` 消费 `AV_PKT_DATA_SKIP_SAMPLES`；MP4 仅在无 side data 时用 TOC−duration 补末包 end；加法 ABI（`ms_event.skip_samples_*`、`ms_hw_audio_au` / `submit_au` / `note_seek` / `seek_preroll`），`MSMEDIA_ABI_VERSION` 仍为 1，不借用 dts。宿主 mock 在写出前修剪 PCM 长度；Android trim FIFO + leftover（未排空不取新输出）。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-21 23:51:22 CST** exit 0。回归全部既有音视频/字幕/HLS/倍速 seek。本切片证据：

- `test_ff_opus`：1s MP4/Ogg cfg=2 au=51 pts=51 inspect=51 aligned=51 pre_skip=312 skip0=312 end=648 audible=48000，signed 首 PTS 为负；50ms Ogg/MP4 audible=2400（end=168）；25ms Ogg audible=1201（end=407）；25ms MP4 au=2 start=312 end=1 audible=1607（FFmpeg-cut limit，不断言 1200）；MP4 中途 seek 首包 skip_start=0。
- `test_abit_opus_mock_configure_submit`：pre-skip 0/312、包元数据与 fallback 不双扣、多包 start skip、末包 end=648、flush 不恢复 pre-skip、seek(0) 再扣 312、中途 seek 3840 warmup、溢出 skip、部分写。
- `test_abit_opus_preroll_switch_and_clock`：seek(0) 扣 pre-skip；中途 seek 第一包被 3840 预热吞掉；切轨不残留。

非正式 `./build.sh android-hw` 于 **2026-11-21 23:51:51 CST** exit 0（非正式，不是四平台/el7/真机验收）。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,642,243** 字节，SHA-256 `ab34373d88babcf388fa31459cac4e51fccc3ccfa93ab88346b32548c7f36c15`。包内 v7a/v8a `libmsff.so` 4,531,528 / 5,526,712（NEEDED 仅 libz/libm/libdl/libc；unstripped 仍有 `ff_mpegaudio_parser`/`ff_opus_parser`/`ff_ogg_demuxer`，无对应 decoder）；`libmshw.so` 92,180 / 104,184（导出 `ms_hw_audio_submit_au` / `ms_hw_audio_note_seek`）；`libmsmedia.so` 176,396 / 179,800。历史 size 产物未覆盖（仍 4,235,076 / `7a757095…`，mtime 12:17:30 CST）。HEAD 仍 `1092bec`。

明确未做 / 仍需设备验证：Android csd-1/csd-2 与系统 Opus 解码器自动 trim、AudioTrack 实听、尾部 padding 在真机上的听感与时钟；1:1 输入/输出假设若某设备一次输出多帧需再核；Ogg 中途 seek 页粒度；25ms 级 MP4 末包 FFmpeg 限制。不开新编码。AVS+、VP9、E-AC-3、raw `.ac3` 白名单、完整 ASS 渲染与位图、matroska、HLS 字幕/视频 rendition、自写 TS 组帧、P5、四平台/el7/真机硬解出画出声仍未做。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 00:10 CST — P4 AVS+（广电广播类 / cavs）宿主解析与统一接入（宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内的既定编码，本块为原始明确目标 **AVS+**（广电，FFmpeg 名 `cavs`，不是 AVS2/AVS3）。未把 VP9 / E-AC-3 扩为新承诺。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS 校验不关。

复用已有 `ms_video_*` / player / Android 查询-配置-提交，不平行重写。加法 `MS_SAMPLE_FMT_CAVS`，旧 `struct_size` 兼容，`MSMEDIA_ABI_VERSION` 仍为 1。只把 GY/T 257.2 广播类 profile `0x48` 标为 `MS_CODEC_AVS_PLUS`；`0x20` 基准类保持 `AVS_FAMILY` 并 `UNSUPPORTED`；TS `0x42` 只是族标识；AVS2/AVS3 映射 `UNKNOWN`。完整压缩 AU 走已开 mpegts + 最小追加的 `cavsvideo` parser（LGPL，只编 `cavs_parser.o`；`CONFIG_CAVSVIDEO_PARSER=1`，`CONFIG_CAVS_DECODER=0`）。官方 `cavs` 解码器只认 `0x20`，禁止启用，不得用软件 cavs 冒充 AVS+。parser 与解码器目标文件未绑在一起，许可允许最小追加。

本箱无 `/ms/build/ffmpeg`、无合法 AVS+ 编码器；不用系统 ffmpeg / libxavs / libxavs2 / AVS2 样本充数。受控 fixture 按 GY/T 257.2 表 2 手工打包序列头与 `0xB3`/`0xB6` 起始码，来源可追，并标明「手工打包 ≠ 工具编码 ≠ 产品解码」。

Android：查阅 AOSP `MediaFormat` 与官方 Supported media formats，**无** AVS/CAVS MIME。`ms_hw_video_mime` 对 AVS+ 返回 NULL；JNI/NDK 在 MediaCodecList / `createByName` / format 创建之前诚实 `UNSUPPORTED`，不回落到 `video/avc`，不把 OpenHarmony `"video/cavs"` 写成普遍 MediaCodec type。平台设备常无 AVS+ 硬件；无硬件不得软解回退。宿主 mock 可配置/提交，不是设备硬解或出画。自写 `ms_codec_query(AVS_PLUS)` 仍为 `NONE`。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin FFMPEG_REBUILD=1 ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 00:07:28 CST** exit 0。全矩阵回归含 Opus trim / HLS / ASS / HEVC / MPEG-2/4 / H.263 / AV1 / AC-3 / MP3/MP2 等既有项。本切片证据：

- `test_vbit_avsplus_parse`：广播类 16×16 / 32×32 完整配置且 CSD 即真实序列头；基准类 `0x20` 为 `UNSUPPORTED` 且不升级；非法 profile 为 `MALFORMED`；I 关键、P 非关键；截断/空输入有界。
- `test_vbit_mock_configure_submit`：拒绝软件 codec；mock 硬解配置并提交 I+P；拒绝 AVCC 与缺 CSD；旧 ABI `struct_size` 仍可配置。
- `test_ff_avsplus`：手工复用的 MPEG-TS（`stream_type 0x42`）`cfg=2 au=2 key=1 pts=2 inspect=2 plus=4 family=1`（打开时族标识可先发 FAMILY，见广播类头后升 PLUS）；基准类 TS `plus==0`；seek 后 codec 仍为 PLUS；截断 TS 有界。
- `test_vbit_avsplus_inband_and_seek_gate`：带内序列头+I 配置，P 不换 epoch，32×32 序列头 epoch++，seek 丢非关键，关键帧后方可再提交。

随后 `ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 重建双 ABI 前缀（不覆盖历史 size 产物），再 `./build.sh android-hw`。宿主 `libavcodec.a` 在 Opus 切片 15,080,200 之上变为 15,177,428 字节；`libavformat.a` 仍约 13,806,240（parser 在 avcodec）。Android size-opt `libavcodec.a` v7a 8,750,762 → 8,812,690，v8a 12,808,022 → 12,898,674。符号表有 `ff_cavsvideo_parser`，无 `ff_cavs_decoder`；前缀仅 `cavs_parser.o`。`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。

非正式 `./build.sh android-hw` 于 **2026-11-22 00:10:37 CST** exit 0（非正式，不是四平台/el7/真机验收）。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,650,435** 字节，SHA-256 `67ebe4370d1fb7f85aa793a7755210c98d37c18439a8831e93b31460b598b441`。包内 v7a/v8a `libmsff.so` 4,534,008 / 5,529,320（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia；unstripped 含 `ff_cavsvideo_parser`，无 `ff_cavs_decoder`）；`libmshw.so` 92,388 / 104,424；`libmsmedia.so` 177,660 / 181,000。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把 23:51 CST Opus APK 当作本切片实现。

明确未做 / 仍需设备验证：AVS+ 真机硬解与出画；平台常无该硬件，无 MIME 时设备路径保持诚实 `UNSUPPORTED`。自写 TS AVS+ 组帧未做。VP9 / E-AC-3 未做（未扩承诺）。完整 ASS 渲染、matroska、HLS 字幕/视频 rendition、既有编码的自写 TS 组帧、P5、四平台/el7/真机硬解出画出声仍未做。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 00:44 CST — P4 位图字幕（DVB / PGS / VobSub）实质实现（宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内既定字幕工作，本块为位图字幕实质实现，不得再只枚举后选择播放返回 `UNSUPPORTED` 就算完成。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS 校验不关。未把 VP9 / E-AC-3 扩为新承诺。

复用统一 `ms_sub` / player / ff / Android 叠加 API，不平行重写。加法 ABI（`MS_CODEC_SUB_DVB`/`PGS`/`VOBSUB`，cue 位图字段，`ms_sub_doc_set_bitmap_header` / `feed_bitmap` / `parse_vobsub`），旧 `struct_size` 兼容，`MSMEDIA_ABI_VERSION` 仍为 1。自写 DVB/PGS/VobSub 有状态解析到有界 RGBA，**不启用** FFmpeg `dvbsub`/`pgssub`/`dvdsub` 解码器；本轮无需追加 demuxer/parser（已开 mpegts 即可交出 PES）。不引入 libass/FreeType。完整 ASS 仍为欠账，去标签纯文本不是完整渲染。通用位图族/XSUB/图文仍 `UNSUPPORTED`。VobSub 无 idx 调色板不臆造颜色。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 00:43:36 CST** exit 0。全矩阵回归含 AVS+ / Opus trim / HLS / ASS / HEVC / MPEG-2/4 / H.263 / AV1 / AC-3 / MP3/MP2 等既有项。本切片证据：

- `test_sub_bitmap_parse`：DVB 16×16 红色位置 (10,20)、开放 cue 被下一显示集关闭、PGS 16×16 蓝色画布 1920×1080、`.sup` AUTO、缺调色板/截断/超幅可控、1024×1024 后再追加触发 4MiB `OVERFLOW` 且保留已有条目、旧 ABI 尾部未写、VobSub idx+8×2 SPU 与 `MSVB` AUTO、无调色板 `UNSUPPORTED`。
- `test_p4_bitmap_inband`：通用 `SUB_BITMAP` 仍不可选；DVB/PGS 可选中、喂入完整 cue、不把二进制当文字、截断不致命、seek/flush 清理后再喂入恢复。字幕轨互斥。宿主 mock，不是设备出画。
- `test_ff_bitmap_subs`：MPEG-TS `0x90`+PMT `HDMV` 识别 PGS 并解析 cue；`0x06`+`0x59` 识别 DVB，紧凑 4×4 一套 PES 得到 RGBA cue；flush 清空。多包 16×16 DVB PES 在探测下可能被拆包，测试改用可装进单 PES 的受控 fixture，不因此宣称任意超 184 字节 PES 已在 lavf 路径验完。

非正式 `./build.sh android-hw` 于 **2026-11-22 00:44:03 CST** exit 0（非正式，不是四平台/el7/真机验收）。未重建 FFmpeg 前缀。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,724,163** 字节，SHA-256 `360b18b8889bdbcb741823fbdf03ad1c7dce094d55a59d219ee694087cb9eaf2`。包内 v7a/v8a `libmsff.so` 4,550,712 / 5,545,656（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia）；`libmshw.so` 92,988 / 104,952；`libmsmedia.so` 193,624 / 196,936。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把 00:10 CST AVS+ APK 当作本切片实现。

明确未做 / 仍需设备验证：位图字幕真机叠加与视觉验收；XSUB/DVB 图文；PGS 跨 PES 续包重组；超单 TS 包的 DVB PES 在 lavf 路径的组包鲁棒性未作为合同。完整 ASS 渲染未做。AVS+ 真机硬解与出画仍未验。VP9 / E-AC-3 未做（未扩承诺）。matroska、HLS 字幕/视频 rendition、既有编码的自写 TS 组帧、P5、四平台/el7/真机硬解出画出声仍未做。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 00:59 CST — 最小追加 matroska demuxer（宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内既有欠账，本块为最小追加 matroska demuxer，真实打开 MKV 内嵌字幕与已支持编码。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS 校验不关。未把 VP9 / E-AC-3 扩为新承诺。完整 ASS 渲染未拉 libass/字体依赖假装完成。

只打开 `--enable-demuxer=matroska` 及 configure 拉入的必要 LGPL 辅助对象（zlib 已开，未开 bzlib/muxer/decoder）。白名单加入 `matroska,webm`（同一 demuxer）。加法 `MS_FORMAT_MATROSKA=11`、`MS_FORMAT_WEBM=12`。复用既有轨道/字幕/音轨/seek 路径。FFmpeg 9.1 对本类 MKV 常把 `start_time` 记为 0，会话 origin 纳入 `codecpar->initial_padding`，以免 AAC priming 负 PTS 在首个视频 AU 之后被标 `OVERFLOW`。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 00:58:38 CST** exit 0。全矩阵回归含位图字幕 / AVS+ / Opus trim / HLS / ASS 布局 / HEVC / MPEG-2/4 / H.263 / AV1 / AC-3 / MP3/MP2 等既有项。本切片 `test_ff_matroska` 证据（fixture 编码不等于产品软解）：

- 受控 MKV：H.264 + 双 AAC + SRT + ASS，枚举/选择、视频与 AAC AU、内嵌字幕包与时间轴 cue（按事件 PTS 查询，因 origin 含 priming 不能只用相对 0）、seek 后再出包、切第二路 AAC、取消有界、关闭清理。
- HEVC-in-MKV AU/CSD；WebVTT-in-MKV；超大 ASS 文本 cue 截断到 `MS_SUB_CUE_TEXT_MAX`；截断文件打开/拉包有界。
- PGS `.sup` 以 `-c copy` 放入 MKV 后，走现有位图路径得到 16×16 RGBA cue（不是新的 PGS 解码器）。
- WebM 仅 Opus：URL `.webm` 映射 `MS_FORMAT_WEBM`，并拉到 Opus AU。不宣传 VP8/VP9/Vorbis，也不把 mk3d/mka/mks 写成已测。

已真实重建宿主与 Android 双 ABI 前缀并做体积对照。宿主 `libavformat.a` 13,806,240 → 14,236,440（+430,200）；`libavcodec.a` 基本不变。Android size-opt `libavformat.a` v7a 8,944,720 → 9,243,416，v8a 13,118,912 → 13,546,824。`CONFIG_MATROSKA_DEMUXER=1`、`CONFIG_MATROSKA_MUXER=0`、`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_BZLIB=0`。

非正式 `./build.sh android-hw` 于 **2026-11-22 00:59:30 CST** exit 0（非正式，不是四平台/el7/真机验收）。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,814,275** 字节，SHA-256 `f5967a755217017568277be2a9782690a714351426bce534de8f90844467d997`。包内 v7a/v8a `libmsff.so` 4,596,124 / 5,592,808（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia）；`libmshw.so` 92,988 / 104,952；`libmsmedia.so` 193,664 / 196,952。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把 00:44 CST 位图 APK 当作本切片实现。

明确未做 / 仍需设备验证：MKV/WebM 真机打开、切轨、seek、内嵌字幕叠加与出画出声；完整 ASS 渲染（字体栅格/动画）；VP9 / VP8 / Vorbis / E-AC-3（未扩承诺）；未测的 mk3d/mka/mks；HLS 字幕/视频 rendition；既有编码的自写 TS 组帧；P5；四平台/el7/真机硬解出画出声。位图字幕与 AVS+ 真机仍未验。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 01:30 CST — HLS 字幕 rendition（独立 WebVTT 外链；宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内既有欠账，本块为 HLS 字幕 rendition 的实质实现，对标已落地的 HLS 备选 AUDIO。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS 校验不关。未把 VP9 / E-AC-3 扩为新承诺。完整 ASS 渲染未拉 libass。HLS 视频 rendition（多码率切视频）本轮明确列为下一欠账，不把 `UNSUPPORTED` 写成完成。

复用官方 FFmpeg HLS 的 AVProgram/AVStream/rendition，不另写清单调度。只按需打开 `--enable-demuxer=webvtt`，format 白名单同步加入 `webvtt`（嵌套打开会拷贝父白名单）。选择模型对标 AUDIO：`pending_sub_si` / `committed_sub_si`，第一包才提交并清空旧内嵌 cue；失败撤回且不把新轨标已选，字幕外链失败返回 `MS_AGAIN` 而不拆掉视频/音频会话。`DEFAULT=YES` 且可播则打开时自动选中并置 `user_sub_sel`，否则 `FORCED`；单独 `AUTOSELECT` 不自动打开。切字幕不重开视频、不毁掉音频会话。JNI `nativeSelectTrack` 仍入队到 worker，网络仍在 `ms_ff_pull`。

官方 lavf 契约（已核实，不是产品另写的路径）：`EXT-X-MEDIA TYPE=SUBTITLES` 外链在 header 用假 `WEBVTT\n` 建流，真正读包时 `init_subtitle_context()` 强制 `av_find_input_format("webvtt")` 打开**当前分片 URL**。本轮实证是独立 WebVTT playlist（相对 URI、查询串）。**MPEG-TS 封装的 WebVTT 作为 TYPE=SUBTITLES 外链、以及主视频 TS 带内 WebVTT，本轮未验，不得写成已完成。** 清单中的 ASS/SRT 外链不宣传为已验收。

打开后 `find_stream_info` 可能把短 VTT 整段读完：含字幕流的 HLS 在默认选择后以 `av_seek_frame(..., 0, AVSEEK_FLAG_ANY|AVSEEK_FLAG_BACKWARD)` 回绕；直播不可 seek 时不否决打开。另有最小本地补丁 `patches/ffmpeg-9.1-hls-sub-rendition-switch.patch`（构建脚本幂等应用）：新启用字幕 playlist 时不按 `cur_timestamp` 丢掉本段已开始的 cue，并在再次 needed 时关掉残留 webvtt 上下文，否则关闭再启会立刻 EOF。用户 seek 仍走官方 `hls_read_seek`。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 01:30:26 CST** exit 0。全矩阵回归含 matroska / 位图 / AVS+ / Opus trim / HLS 备选 AUDIO / ASS 布局 / HEVC / MPEG-2/4 / H.263 / AV1 / AC-3 / MP3/MP2 等既有项。本切片 `test_ff_hls_subs` 证据（本地受控 HTTP master + 多外链 WebVTT；fixture 编码不等于产品软解）：默认 English 自动选中并出 HelloEng cue（文本与媒体时间对齐）；来回切换到 Chinese / 切回 English，旧 cue 已清，`timeline_epoch` 不变、`selection_epoch` 增加，视频与音频连续；关闭后再启用 Chinese；非法 id 不改选择；畸形 `SUBTITLES="missing"` 仍打开视频且不伪造字幕轨；缺失 playlist 诚实失败、不伪报已选；查询串相对 URI；取消有界；seek 到 2s 后仍为 Chinese 轨并再出对齐 cue。不只断言 API 返回值。

已真实重建宿主与 Android 双 ABI 前缀并做体积对照（相对 matroska 切片后、本轮打开 webvtt 之前）。宿主 `libavformat.a` 14,236,440 → 14,423,582（+187,142）；`libavcodec.a` 仍为 15,177,444。Android size-opt `libavformat.a` v7a 9,243,416 → 9,377,550，v8a 13,546,824 → 13,741,614。`CONFIG_WEBVTT_DEMUXER=1`、`CONFIG_WEBVTT_DECODER=0`、`CONFIG_WEBVTT_MUXER=0`、`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_LIBASS=0`。

非正式 `./build.sh android-hw` 于 **2026-11-22 01:30:49 CST** exit 0（非正式，不是四平台/el7/真机验收）。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,826,563** 字节，SHA-256 `d4e1940b469dddc4549634c0a649963d5dc05724d6f4f34e39a490fb7c83a942`。包内 v7a/v8a `libmsff.so` 4,602,116 / 5,599,240（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia）；`libmshw.so` 92,988 / 104,952；`libmsmedia.so` 193,664 / 196,952。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把 00:59 CST 的 matroska APK（10,814,275，`f5967a755217017568277be2a9782690a714351426bce534de8f90844467d997`）当作本切片实现。

明确未做 / 仍需设备验证：HLS 字幕 rendition 真机枚举/切换/叠加出画；HLS 视频 rendition（多码率切视频）；MPEG-TS 封装或主 TS 带内 WebVTT 作为字幕 rendition；HLS 清单一侧的 ASS/SRT 外链；完整 ASS 渲染；直播不可 seek 时探测耗尽后的片头 cue；用户 seek 点之前已开始的 cue 补发；自写 TS 组帧；自写 `ms_hls` master；P5；四平台/el7/真机硬解出画出声。matroska / 位图 / AVS+ 等既有项真机仍未验。VP9 / E-AC-3 未扩承诺。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 01:55 CST — HLS 视频变体手动选择与切换（宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内既有功能，本块为 HLS 视频 variant/rendition **手动**选择与切换闭环。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；历史归档与 `build/android-v7a/size/libmsff.so.1.0.0` 未覆盖；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS/主机名校验不改。只按既定 HLS 功能做手动变体选择，**不额外承诺自动 ABR**。未把跨 codec 的 `UNSUPPORTED` 写成全部切换完成。

实现：复用官方 FFmpeg HLS AVProgram/AVStream 与已落地音/字幕 pending-commit，不平行写 master 调度。变体枚举以实际已知元数据为准（打开时名称常见仅带宽；宽高以切换后真实 SPS 为准）。播放中来回切：等新轨关键帧提交、重发 CSD、不复用旧参数、时间轴连续、资源有界。关联外链音/字幕组保持用户选择；muxed 音轨明确回退。失败/EOF/取消/队列上限不伪报成功、不无限等待。UI/JNI 只入队。尝试把字幕补丁的残留 ctx 关闭扩到视频会空指针，已撤回；字幕补丁保持，字幕回归仍通过。Android 前缀未重编，只重链。

证据：受控本地 HTTP 两档分辨率/带宽 master（及 muxed 第二份 master），`test_ff_hls_video_variants` 实测新旧 AU 来源、CSD/关键帧/PTS、来回切、非法 id、缺失变体 404、跨 codec 拒绝、seek、外链音/字幕连续性、EOS、取消、队列上限。`test_ff_hls_video_variant_player` 验证暂停/倍速/seek 与 mock 重配。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` ASan/UBSan 于 **2026-11-22 01:55:24 CST** exit 0。非正式 `./build.sh android-hw` 于 **2026-11-22 01:55:40 CST** exit 0。内部 APK 10,830,659 字节，SHA-256 `92ee83fad6c54d32cd29527774f1bb8d253c4aeac7d4003931aae86eb9872df5`；v7a/v8a libmsff 4,604,292 / 5,600,920，NEEDED 仅 libz/libm/libdl/libc。未向用户发阶段文件，未 commit/push，未启动 AVD。

明确未做 / 仍需闭环：HLS 视频变体真机切换与出画；自动 ABR（未承诺）；跨 codec 变体切换；直播不可 seek 时短 VTT 探测耗尽后的片头 cue；用户 seek 点之前已开始的 cue 补发；PGS/DVB 组包鲁棒性；MPEG-TS 封装或主 TS 带内 WebVTT；HLS 清单一侧的 ASS/SRT 外链；完整 ASS 渲染；自写 TS 组帧；自写 `ms_hls` master；P5；四平台/el7/真机硬解出画出声。matroska / 位图 / AVS+ / HLS 字幕等既有项真机仍未验。VP9 / E-AC-3 未扩承诺。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 02:14 CST — PGS 续包 / DVB 超单 TS 包 PES 在 lavf 路径的组包鲁棒性（宿主已落地，设备未验）

授权：继续 t94u「都完成」、t95u「全部都做好再给我」内字幕完整性；先修已有字幕真实缺口，不开新功能。全部原有规则/Bot 本机例外不变：无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包；历史归档与冻结 `build/android-v7a/size/libmsff.so.1.0.0` 未覆盖；`CONFIG_DECODERS=0`；视频硬解 only；系统 AAC 例外不拓展；TLS/主机名校验不改。禁止把测试 fixture 缩到 4×4 单包来代替普通 16×16 以上真实多包需求。本轮只完成位图组包，HLS 用户 seek 补发与直播短 VTT 片头明确留作下一项，未堆未测 FFmpeg/HLS 补丁（视频 playlist ctx 不得再被字幕补丁误关）。

实现：分析官方 mpegts「一 PES 一 AVPacket」契约后，复用有界 acc 与统一时间轴。END 单独成包时先并入 acc；DVB 续 PES 剥除 `0x20 0x00`，独立显示集须以 page composition 开头以免丢掉未完成页；PGS ODS first/continuation/last 收齐 RLE 再解码。到来顺序/缺片/截断/过长/切轨 seek flush 有界，不串帧、不泄漏旧字幕。旧 ABI 与 RGBA 生命周期不变；超限诚实返回，不错误复用旧画面。测试夹具末包改用 adaptation stuffing，避免 `0xFF` 被当成 PES 载荷。未改产品 FFmpeg 补丁，前缀未重编。

证据：`test_ff_bitmap_subs` lavf DVB 改为 16×16 多 TS 包 PES 并独立校验像素/位置/时间；`test_ff_bitmap_reassembly` 用真实多 TS/多 PES/拆分 segment 的受控 fixture 经 libmsff 到 RGBA，覆盖 DVB 头部/载荷断点、缺中片、截断、seek/flush，以及 PGS 每段一 PES、ODS 两段/三段与原始字节流断点。`test_core` 同步覆盖增量喂入。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` ASan/UBSan 于 **2026-11-22 02:14:27 CST** exit 0（含 HLS 变体/字幕/音轨、MKV、ASS、Opus 及其它编码）。非正式 `./build.sh android-hw` 于 **2026-11-22 02:14:54 CST** exit 0。内部 APK 10,834,755 字节，SHA-256 `67d2846d04ac3bdf34e4a8e7592ac77f282ca6a28ae9cc97438f9c04802dccd6`；v7a/v8a libmsff 4,605,892 / 5,602,312，NEEDED 仅 libz/libm/libdl/libc。未向用户发阶段文件，未 commit/push，未启动 AVD。编译/mock 不是设备视觉验收。

明确未做 / 仍需闭环：HLS 用户 seek 不补发 start 早于 seek 点但 end 仍覆盖该点的 cue；直播不可 seek 时短 VTT 探测耗尽后的片头 cue；位图字幕真机叠加与时间对齐；完整 ASS 渲染；自写 TS 组帧；自写 `ms_hls` master；MPEG-TS 封装或主 TS 带内 WebVTT；HLS 清单一侧的 ASS/SRT 外链；XSUB/图文；自动 ABR（未承诺）；跨 codec 视频变体；P5；四平台/el7/真机硬解出画出声。matroska / AVS+ / HLS 字幕 / HLS 视频变体等既有项真机仍未验。VP9 / E-AC-3 未扩承诺。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 02:38 CST — HLS 字幕时间轴：seek 覆盖进行中 cue 与直播短 VTT 片头（宿主已落地，设备未验）

继续用户已授权「全部做好」内原有字幕时间轴正确性，只修两项已知缺口。研究官方 FFmpeg 9.1 HLS/WebVTT 与既有切轨补丁、统一 `ms_sub` 时间轴后，做字幕域有限重放/覆盖查找：`hls_read_packet` 对字幕保留 end>seek 的进行中 cue（end 开区间），已过期不复活；直播首次启用从当前滑动窗口起点重开，不改 seekable、不缓存全部历史；`webvttdec` 解析 `X-TIMESTAMP-MAP` 并保持 1/1000 以免与 HLS stub 主流转时基冲突；`ms_sub` 相同 cue 去重。不改音视频 seek/catch-up，不关闭视频 ctx。两项均完成，不以 `UNSUPPORTED` 充数。

证据：`test_ff_hls_sub_timeline` 本地 HTTP VOD seek(2s) 覆盖 0.5–4s 长 cue、边界 end 不显示、跨段去重、切轨旧 cue 消失、非零 timestamp map；无 ENDLIST 动态 live 片头 cue 进入时间轴，重复拉清单/滑动窗口不重复累计，cancel/失效/限额。真实 demux，不只返回值。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` ASan/UBSan 于 **2026-11-22 02:38:05 CST** exit 0（含 video/audio/sub HLS、MKV、位图、Opus 与编码回归）。因 FFmpeg 源码变更已重建宿主与双 ABI 前缀；非正式 `./build.sh android-hw` 于 **2026-11-22 02:41:13 CST** exit 0。内部 APK 10,834,755 字节，SHA-256 `e587cb15d8f3a52abb065541e7d7abac1b10cb67d6bbb66cb255f3b7308e2c1c`；v7a/v8a libmsff 4,607,236 / 5,603,336，NEEDED 仅 libz/libm/libdl/libc。冻结 v7a size 制品未覆盖。未向用户发阶段文件，未 commit/push，未启动 AVD。编译/mock 不是设备视觉验收。

明确未做 / 仍需闭环：位图字幕真机叠加与时间对齐；完整 ASS 渲染；自写 TS 组帧；自写 `ms_hls` master；MPEG-TS 封装或主 TS 带内 WebVTT；HLS 清单一侧的 ASS/SRT 外链；XSUB/图文；自动 ABR（未承诺）；跨 codec 视频变体；P5；四平台/el7/真机硬解出画出声。matroska / AVS+ / HLS 字幕（含本时间轴）/ HLS 视频变体 / 位图组包等既有项真机仍未验。VP9 / E-AC-3 未扩承诺。用户要最终统一交付，本阶段只保留内部结果。

## 2026-11-22 03:03 CST — 累积正确性审查与稳定 C ABI 修复（不扩功能，不向用户发阶段包）

发现并修复：已交付 1838 头的 `ms_event` 为 176 字节且无 `struct_size`，随后在尾部追加的 `skip_samples_*` 使当前库 `ms_session_pull` / `ms_ff_pull` 按 184 字节整结构写入旧客户端栈对象（本机 gcc 实测）。未抬 `MSMEDIA_ABI_VERSION`（仍为 1），未删旧符号。恢复 1838 布局；修剪改走 `ms_event_trim` 查询与 `ms_hw_player_note_event_trim`。JNI worker 在 feed 前查询并 note。专项 `test_abi_old_header` 用冻结的 1838 公共头（源包 SHA-256 `7303150d5f0745d1642fc8ece3251805e4b77bd3c27e15581a5caa82e4a3c7a6`）编译独立客户端，动态链接当前 ASan `.so`：`sizeof(ms_event)=176`，session/ff pull 金丝雀完好，旧 `struct_size` stats、当前头 `list_tracks`/`cues_at` 步进以及极小 cue `struct_size` 金丝雀通过，`abi=1`。

顺带实修（非杜撰）：`fill_cue` 开头字段改为按调用方 `struct_size` 闸门写入；`clear_cues`/reset 释放位图 acc 缓冲（上限 64KiB，与注释一致）；删除 `ms_ff_select_track` 外挂字幕分支里误缩进的 `timeline_epoch=1`。抽查后未把 JNI 生命周期、切轨 pending 回滚、seek/暂停/倍速时钟写成新缺陷。Android 音频 trim 仍按输入/输出 1:1 弹出队列，分裂/合并风险记下，本轮不先写必失败反例以免阻塞矩阵。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan，含新旧头动态兼容专项）于 **2026-11-22 03:03:28 CST** exit 0。未改 FFmpeg 源码，未重建前缀。非正式 `./build.sh android-hw` 于 **2026-11-22 03:03:40 CST** exit 0（仅重链）。未启动共享 AVD，未安装 APK。内部 `build/android-hw/MsHwPlayer.apk` **10,834,755** 字节，SHA-256 `5ed9079bbd8d66275a1c77a4ebecd6ca9c95f399b991e1937220ae195af3986e`。包内 v7a/v8a `libmsff.so` 4,607,364 / 5,603,736（NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia）；`libmsmedia.so` 196,192 / 199,352；`libmshw.so` 93,996 / 105,784。历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 仍 2026-11-21 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把 02:41 CST 时间轴 APK 当作本切片实现。未向用户发阶段文件。

## 2026-11-22 03:25 CST — 关闭 Android trim FIFO 1:1 假设（宿主已落地，设备未验；不向用户发阶段包）

先写反例再修：1 个压缩 AU → 多块 PCM（960 拆 400+560、start 312，1:1 会在第一块上错扣）、多 AU → 一块 PCM（两包合成 1920、start 312 与 end 648，1:1 会把 end 打到整块或丢掉第二包 end）、空输出不得 pop 元数据、延迟输出、EOS 排空、AudioTrack 部分写。抽出 `ms_hw_pcm_trim`，Android 与 mock 真正复用。按输出样点/PTS 消耗 skip/end；流级 312 pre-skip 与 3840 warmup 计入当前 AU expected；`platform_trims_delay` 不双扣 start，且不用压缩 AU 长度当已修剪输出长度。有界队列、flush 丢弃 hold、时钟仍为播放头/已写出可听样点（不是提交样点）。递增样点核对 312/3840/648、多包 skip、交错 metadata、0 样点、拆合、背压、overflow、切轨/seek flush、EOS。1838 `ms_event` 布局与 sidecar 保持，`test_abi_old_header` 与 `test_ff_opus`（真实 Ogg/MP4 fixture demux 到 sidecar）一并回归。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 03:25:12 CST** exit 0。非正式 `./build.sh android-hw` 于 **2026-11-22 03:25:43 CST** exit 0（仅重链，未重建 FFmpeg 前缀）。内部 APK **10,842,947** 字节，mtime 2026-11-22 03:25:46 CST，SHA-256 `eef92bd2b9afa5b9176a0148c0976470f7ec256437fb2053b28f680131b3ab42`。包内 v7a/v8a `libmsff.so` 仍 4,607,364 / 5,603,736（NEEDED 仅 libz/libm/libdl/libc）；`libmsmedia.so` 196,192 / 199,352；`libmshw.so` 98,116 / 109,968。冻结 v7a size 产物仍为 4,235,076 / `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 2026-11-21 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把宿主模型写成真机通过。未向用户发阶段文件。


## 2026-11-22 03:51 CST — HLS EVENT/滑动窗口定位与时移边界（宿主已落地，设备未验；不向用户发阶段包）

按原始设计与 FFmpeg 9.1 HLS/DASH seek 契约补缺口，不平行重写清单、不另建 DVR、不无限本地缓存。滑动 live 不再因 `duration<=0` 被误报 `NOT_SEEKABLE`；EVENT/滑动的 `ms_ff_duration_90k` 为 -1，不得把窗口长度当 VOD 时长。加法 `ms_ff_get_seek_range` / `ms_ff_seek_live_edge` 与播放器 `note_seek_range` / `seek_live_edge`：查询 start/end/live_edge/safety（90 kHz，相对会话 origin）；绝对/相对定位；越过 live_edge 或窗口滑动后过期目标钳位到 start/live_edge 并返回 OK。live_edge 减去一个 target duration，不跳未完成末片。UDP/RTP/RTMP/RTSP 与 DASH 直播诚实 `NOT_SEEKABLE`。最小幂等补丁 `patches/ffmpeg-9.1-hls-live-window.patch` 写入窗口元数据并同步 `s->start_time`，不改 finished duration、不关字幕 ctx。JNI 异步入队（含 Live），seek 后复用既有 flush / 音频 trim / 字幕清空 / 关键帧门控 / CSD，保留用户轨选择。`MSMEDIA_ABI_VERSION` 仍为 1。

宿主测试：`test_ff_hls_live_window` 用本地受控动态 HTTP 真实 demux EVENT（非零 MEDIA-SEQUENCE、窗口增长+DISCONTINUITY、长 cue、切轨保留、前后/越界钳位、live_edge、取消）与滑动窗口（起点推进、过期钳位、缺失分片不挂死）；`test_ff_hls_sub_timeline` 直播首条 cue 仍过；`test_p4_ff_seek` VOD range；`test_hw_player` 窗口钳位并保留暂停/倍速；UDP/RTP/RTMP 维持 `NOT_SEEKABLE`。只证明 AU/CSD/PTS，不是出画。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan；此前 `FFMPEG_REBUILD=1` 已真实重建宿主前缀，`CONFIG_DECODERS=0`，`msmedia_refresh_hls_window` 已编入 `libavformat.a`）于 **2026-11-22 03:51:48 CST** exit 0，含 `test_ff_hls_live_window`、`test_ff_hls_sub_timeline`、`test_abi_old_header`（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。UDP-FAST mid-GOP 本轮有视频 AU，未 flake。非正式 `ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 已重建 v7a/v8a size-opt 前缀，`./build.sh android-hw` 于 **2026-11-22 03:55:16 CST** exit 0。内部 APK **10,859,331** 字节，mtime 2026-11-22 03:55:16 CST，SHA-256 `0029b791802bf5250a13c0e70e1b9bc42ea681fd3ad8e99a8ccb37abf8c8da19`。包内 v7a/v8a `libmsff.so` 4,610,244 / 5,606,384（NEEDED 仅 libz/libm/libdl/libc）；`libmsmedia.so` 196,192 / 199,352；`libmshw.so` 100,440 / 111,960。冻结 v7a size 产物仍为 4,235,076 / `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`（mtime 2026-11-21 12:17:30 CST）。HEAD 仍 `1092bec`，未 commit/push。不得把宿主模型写成真机通过。未向用户发阶段文件。

## 39. 可选完整 ASS 渲染（隔离 libmsass；2026-11-22 04:16 CST，Asia/Shanghai）

继续「都完成 / 全部都做好再给我」内的字幕原始承诺：不再把去标签/有限 span 当作完整渲染。许可与体积核实后，采用成熟 libass 作为**可选隔离**模块，不塞入 `libmsmedia`/`libmsff`，不改变未选定的顶层许可证。轻量纯文本路径保留。字幕栅格不是视频软解，不读回视频 Surface。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 04:16:21 CST** exit 0，含 `test_ass_render` 多时间点像素证据与 `test_abi_old_header`（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。非正式 `ANDROID_SIZE_FFMPEG_ONLY=1 ./build.sh android-hw` 于 **2026-11-22 04:19:35 CST** exit 0，APK **已打入**双 ABI 可选 `libmsass.so` 并接线 ImageView。未启动共享 AVD，未安装 APK。不得把宿主渲染当作设备视觉验收，也不承诺 100% 全部 ASS。

宿主 debug 未 strip（本轮官方矩阵产物）：

| 产物 | 字节 | NEEDED |
| --- | ---: | --- |
| `libmsmedia.so.1.0.0` | 687,744 | 仅 libc |
| `libmsff.so.1.0.0` | 19,927,432 | libxml2/libz/libm/libc/ld-linux；无 libssl，无 libmsass |
| `libmshw.so.1.0.0` | 234,088 | libmsmedia、libc（dlopen 可选 libmsass，不 NEEDED） |
| 可选 `libmsass.so.1.0.0` | 2,569,024 | 仅 libz/libm/libc；无 libstdc++、无共享 libass |
| 静态依赖（host prefix） | libass.a 409,794；libfreetype.a 1,010,710；libharfbuzz.a 1,945,646；libfribidi.a 159,176 | 不打进默认核心 |

内部 APK `/workspace/MsMedia/build/android-hw/MsHwPlayer.apk` 14,316,499 字节，SHA-256 `6a74fe6e53eaa5383ebbfaced5be1ee77284c7f6034943c89c1752bc46d703bf`，mtime 2026-11-22 04:19:35 CST。已签名，**未安装**。相对 §38 的 10,859,331（`0029b791…`）增加约 3.46 MiB，与双 ABI `libmsass.so`（1,478,856 + 1,966,168）相符，不把增量藏进核心。

| 产物 | v7a / v8a 字节 | 说明 |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so` | 196,192 / 199,352 | 与 §38 相同；NEEDED 仅 libc/libm/libdl |
| 默认核心 `libmsff.so` | 4,610,244 / 5,606,384 | 与 §38 相同；NEEDED 仅 libz/libm/libdl/libc |
| 默认核心 `libmshw.so` | 105,592 / 118,072 | 相对 §38 的 100,440 / 111,960，增加 bind/JNI；仍不 NEEDED libmsass |
| 可选 `libmsass.so` | 1,478,856 / 1,966,168 | 静态链入钉选依赖；NEEDED 仅 libz/libm/libdl/libc，无 libc++_shared |
| 默认核心合计（media+ff+hw） | 4,912,028 / 5,923,808 | 不含可选 renderer |
| 可选模块增量 | 1,478,856 / 1,966,168 | 须单独报告 |

未覆盖冻结 v7a size 产物 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076 字节，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST）。

本切片不是设备硬解、出画、出声或字幕上屏验收。当时仍未做：真机 ASS 叠加与时间对齐；100% 全部 ASS 特效；DASH 直播时移；XSUB/图文与 P5；四平台/el7 与统一交付。

## 40. 可选 AI 字幕插件框架（隔离 libmsai；2026-11-22 04:50 CST，Asia/Shanghai）

授权：继续用户原始 P5 可选 AI 字幕转写与翻译承诺（全部做好统一交付）。先读 DESIGN §12 / ROADMAP P5。不自选付费供应商、不调用第三方服务、不上传真实私有音频、不从 vault 取凭据、默认禁用、不安装大型模型凑数。供应商尚未指定是实接验收阻碍；本切片完成供应商无关的真实插件框架、音频接入、时间轴、翻译回传与受控测试，**不可把 mock 说成真实 AI 识别完成**。既有规则/Bot 本机编译例外不变。无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包。`CONFIG_DECODERS=0`，视频硬解 only，AAC 系统例外不扩，TLS 校验不变，稳定 ABI 加法。

实现：可选 `libmsai`（`include/msmedia/ms_ai_sub.h`），不链入默认核心；`libmshw` 仅 `dlopen`。修剪后可听 PCM 经 `ms_hw_audio_set_pcm_tap` 非阻塞入有界队列（满则丢最旧），异步工作线程只在显式启用且已注册插件时创建。PTS 来自内部 `ms_hw_pcm_trim_last_out`（AU 起点 + 第一可听样点下标），不是臆造时间戳。转单声/线性重采样为公共可验证辅助，不是软解。partial/final/翻译归并到 `MS_TRACK_ID_AI_SUB`，`ms_sub_cue` 尾部加法字段与 `MS_SUB_CAP_AI_*` 经 `struct_size` 门控；选中 AI 轨不取消原字幕。seek/flush 与切音轨 bump epoch 并取消在途。禁用时 tap 为空操作。JNI `nativeAiStatus` / `Configure` / `SetEnabled` 与测试按钮文案为「AI sim (not real)」，状态不含商业供应商名。

受控假供应商 `ms-ai-fake-local` 把 PCM 首末样点与 pts 编进明确 `SIM-` 文本。`test_ai_sub` 走 mock 已解码 Opus→trim→tap，断言 pcm0=312、pts_us=6500，覆盖修订/final/翻译、乱序不降级、迟到 epoch 丢弃、flush、错误、超长、队列上限、pause/倍速媒体时间、禁用不抓。不以 `add_cue` 绕过音频链路。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0，ASan/UBSan）于 2026-11-22 04:50:14 CST 退出 0，含 `test_ai_sub: PASS` 与 `test_abi_old_header`（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。UDP-FAST mid-GOP 本轮一次通过。宿主 debug 未 strip：

| 产物 | 字节 | NEEDED |
| --- | ---: | --- |
| `libmsmedia.so.1.0.0` | 688,216 | 仅 libc（相对 §39 的 687,744，加法 cue 字段） |
| `libmsff.so.1.0.0` | 19,927,912 | libxml2/libz/libm/libc/ld-linux；无 libssl，无 libmsass/libmsai |
| `libmshw.so.1.0.0` | 255,760 | libmsmedia、libc（dlopen 可选 libmsai/libmsass，不 NEEDED） |
| 可选 `libmsass.so.1.0.0` | 2,569,024 | 与 §39 相同 |
| 可选 `libmsai.so.1.0.0` | 616,208 | libmsmedia、libc，以及本 debug 的 libasan/libubsan；无模型 |

非正式 `ANDROID_SIZE_FFMPEG_ONLY=1 ./build.sh android-hw` 于 2026-11-22 04:51:46 CST 退出 0。内部 APK `/workspace/MsMedia/build/android-hw/MsHwPlayer.apk` 14,378,081 字节，SHA-256 `c1b4cbcdfa4e24dfe6a7f92e54099d6b8af9c2f3292144b9a7c839ff5b6fb03c`，mtime 2026-11-22 04:51 CST。已签名，**未安装、未启动共享 AVD**。不得把 §39 的 14,316,499（`6a74fe6e…`）当作本实现。冻结 v7a size 产物未覆盖（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`）。

| 产物 | v7a / v8a 字节 | 说明 |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so` | 196,448 / 199,608 | 相对 §39 的 196,192 / 199,352；NEEDED 仅 libc/libm/libdl |
| 默认核心 `libmsff.so` | 4,610,244 / 5,606,384 | 与 §39 相同；NEEDED 仅 libz/libm/libdl/libc |
| 默认核心 `libmshw.so` | 112,600 / 125,464 | 相对 §39 的 105,592 / 118,072，增加 AI bind/JNI/tap；仍不 NEEDED libmsai/libmsass |
| 可选 `libmsass.so` | 1,478,856 / 1,966,168 | 与 §39 相同 |
| 可选 `libmsai.so` | 20,000 / 20,608 | 无模型；NEEDED 仅 libmsmedia/libc/libdl |
| 默认核心合计（media+ff+hw） | 4,919,292 / 5,931,696 | 不含可选模块 |
| 可选 AI 增量 | 20,000 / 20,608 | 须单独报告 |

本切片不是设备硬解、出画、出声或真实 AI 识别验收。供应商、凭据、上云授权与真机效果须用户后续决定。仍未做：真实供应商实接；预生成/缓存转写；DASH 直播时移；XSUB/图文；四平台/el7 与统一交付。

## 41. Linux VAAPI 码流映射与 DRM 探测（2026-11-22 05:17 CST，Asia/Shanghai）

授权：继续用户原定 Android/Windows/Linux/iOS 四平台目标与「全部都做好再给我」，不是擅自开新平台。本轮先审现有硬解适配、设计、构建脚本与本机工具链，完成一块本机可验证的跨平台前置实现，不为绕设备阻碍扩大格式范围。既有规则/Bot 本机编译例外不变。无 CloudAgent / machineId / 用户电脑 / 共享 AVD / commit / push / UPX / 阶段包。`CONFIG_DECODERS=0`，硬解 only，AAC 显式例外与 TLS 正常，旧头 ABI 动态回归。不得用其它用户电脑或服务伪造 Mac/Windows 验收。P5 供应商实接仍未做（不能自选供应商、不能上传私有音频）。

本机只读核查：无 `/dev/dri`；系统有 `libva.so.2` / `libva-drm.so.2`（2.22.0-3）但无 `libva-dev`/`vainfo`；无 mingw、无 Apple SDK；无 docker/podman/可用 el7 镜像与 `/ms`；`/dev/kvm` 节点存在（`root:pulse-access`）但当前用户无权限，且不得启动共享 AVD。因此只实施 Linux VAAPI 映射与探测，不堆 Windows/iOS 未编译代码。

实现：`src/hw/ms_hw_vaapi.c` 运行时 `dlopen` libva（MIT，最小 ABI 子集见 `ms_hw_vaapi_libva.h`），`libmshw` 不 NEEDED libva。`ms_hw_vaapi_parse_stream` 按码流字段映射 `VAProfile`，禁止仅 MIME 声称支持；AVS+ 可解析但 libva 无对应 profile，保持未映射。`ms_hw_vaapi_probe_device` 打开 DRM 并查询 profile/VLD。默认 query 与 `mock_mode=0` 的 configure 走该路径，无设备或提交未实现时 `is_hardware=0` 且返回 `UNSUPPORTED`；mock 状态机保持分离。slice 提交与呈现明确未做，见 `docs/HW_LINUX.md`。

官方 `./build.sh test`（gcc 14.2.0，ASan/UBSan）于 **2026-11-22 05:17:57 CST** 退出 0，含 `test_hw_vaapi ok`、`test_hw_backend`、`test_ai_sub: PASS` 与 `test_abi_old_header`（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。本机构建 `./build.sh release` 在未改动的 `src/codec/ms_abit.c` 触发 gcc 14 `-Werror=array-bounds`（`-O2`），本轮未把它当正式发版，也未借此改无关音频解析。

宿主 debug 未 strip（ASan/UBSan 构建）：

| 产物 | 字节 | NEEDED |
| --- | ---: | --- |
| `libmsmedia.so.1.0.0` | 688,216 | 仅 libc（与 §40 相同） |
| `libmsff.so.1.0.0` | 19,927,912 | 与 §40 相同；无 libssl，无 libva |
| `libmshw.so.1.0.0` | 301,464 | 仅 libmsmedia、libc（相对 §40 的 255,760；dlopen 可选 libva/libmsai/libmsass，不 NEEDED） |

非正式 `./build.sh android-hw` 于 **2026-11-22 05:18:49 CST** 退出 0。内部 APK `/workspace/MsMedia/build/android-hw/MsHwPlayer.apk` 14,394,465 字节，SHA-256 `780873f61c23ec7a5046538ce6dc9b91c48fd4ff3b8169f0af92406f2fe833af`，mtime 2026-11-22 05:18:49 CST。已签名，**未安装、未启动共享 AVD**。不得把 §40 的 14,378,081（`c1b4cbcd…`）当作本实现。冻结 v7a size 产物未覆盖（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST）。HEAD 未 commit/push。

| 产物 | v7a / v8a 字节 | 说明 |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so` | 196,448 / 199,608 | 与 §40 相同；NEEDED 仅 libc/libm/libdl |
| 默认核心 `libmsff.so` | 4,610,500 / 5,606,624 | 仅重链，NEEDED 仍仅 libz/libm/libdl/libc |
| 默认核心 `libmshw.so` | 119,728 / 132,712 | 相对 §40 的 112,600 / 125,464，增加 VAAPI 解析/探测（Android 上探测为未构建路径）；仍不 NEEDED libva/libmsai/libmsass |
| 可选 `libmsass.so` | 1,478,856 / 1,966,168 | 与 §40 相同 |
| 可选 `libmsai.so` | 20,000 / 20,608 | 与 §40 相同 |

本切片不是设备硬解、出画、出声或真实 AI 识别验收。仍未做：VAAPI 提交/呈现与有 GPU 的 Linux 实机；Windows MF/D3D11、iOS VideoToolbox 的编译与验收；el7 容器；P5 真实供应商实接；DASH 直播时移；XSUB/图文；统一交付。未向用户发阶段文件。

## 42. gcc 14 release `-Werror=array-bounds` 收口（2026-11-22 05:45 CST，Asia/Shanghai）

授权：上轮发现的当前 release 构建失败属于交付正确性，不是无关音频改动。既有规则/本机编译例外/禁止项不变；统一交付、不发阶段文件。无 CloudAgent / machineId / 共享 AVD / commit / push / UPX。`CONFIG_DECODERS=0`，硬解 only，AAC 显式例外与 TLS 校验保持。默认 `build/release` 为冻结历史，不得覆盖。P5 供应商实接仍未做。

精确错误（复现于 gcc 14.2.0，`./build.sh release` 同类 `-O2 -Werror`）：`src/codec/ms_abit.c:222` `parse_mpega` / ISRA 克隆 `parse_mpega.isra`，

`br = (ver_id == 1 ? k_mp_br_v1 : k_mp_br_v2)[layer_n - 1][bridx] * 1000;`

告警 `array subscript -16` / `offset -64 into object k_mp_br_v1/k_mp_br_v2 of size 192`。`-O3` 同样失败；`-Os`/`-O0`（官方 debug ASan）不告警。未关闭 `-Werror`/`-Warray-bounds`，未改全局优化级别，未用 noinline/volatile 遮掩，未在未证明时断言编译器 bug。

根因证据：源码在表查找前已拒绝 `ver∈{0,1}`、`layer==0`、`bridx∈{0,15}`、`sridx==3`。之后 `layer_n` 只可能是 1/2/3，`layer_n-1` 只可能是 0/1/2，`bridx` 为 1..14，没有任何源码路径产生下标 -16（即 `layer_n==0 && bridx==0` 的 `(-1)*16+0`）。缩减复现表明：两个调用点（`ms_audio_cfg_parse` / `ms_audio_au_inspect`）加上 signed `layer_n-1` 与三元表指针，会让 gcc 14 `-O2` 的诊断推到该越界式；把层映射写成穷尽分支并在查表前显式核行/列后，同一编译选项通过。这是边界约束不够显式，不是已证明的运行时越界。

修复：`mpega_read_header` 复用公共 4 字节头字段读取与长度校验；保留位（含 MPEG-2.5）穷尽拒绝；ISO layer 显式映射到表行 0..2，其它值 `MALFORMED`；`mpega_bitrate_kbps` / `mpega_sample_rate` 先核 `ver_id`、行、列再索引（同 AC-3 查表风格）。未删历史代码或产物。测试旧前缀 `ms_sub_cue` / `ms_hw_player_stats` / `ms_hw_rate_caps` 改为完整对象 + 显式 `struct_size` + 尾部金丝雀，避免把短于类型的缓冲强转完整结构（同属 `-O2 -Warray-bounds`，正式 release 矩阵编测试时才会出现）。`build.sh` 新增向后兼容 `--out-dir` / `MS_BUILD_OUT`（必须落在本树 `build/` 子目录）、`--test`、`--opt=O3`（禁止写入默认 `build/release`）。测试临时 CA 改为显式 `basicConstraints=CA:TRUE`，签发时清掉钉选 `OPENSSL_CONF`，生成后 `openssl verify`；不改变产品 TLS 校验。默认 `./build.sh release` 仍是 `-O2` 写入 `build/release`。

官方矩阵（均走 `./build.sh`，非临时 gcc 验收）：

- debug ASan/UBSan 全矩阵：`./build.sh test`，**2026-11-22 05:30 CST** 退出 0，含 `test_abit_parse`（合法 MP3/MP2 + 保留位/自由格式/MPEG-2.5/Layer I 误用 + MPEG-2 L3 24 kHz/576 样点 + padding/mono + 104/1440 极限帧）、`test_hw_vaapi`、`test_ai_sub: PASS`、`test_abi_old_header`（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。
- 未消毒 release `-O2`：`./build.sh release --out-dir=build/release-audit-20261122 --test`，**2026-11-22 05:39 CST** 退出 0，含 1838 旧头与 `test_net` HTTPS（校验证书，校验保持开启）。
- 未消毒 release `-O3`：`./build.sh release --opt=O3 --out-dir=build/release-audit-20261122-o3 --test`，**2026-11-22 05:43 CST** 退出 0，同样含 1838。
- release-size `-Os`：`./build.sh release-size --out-dir=build/release-size-audit-20261122 --test` 在 `test_ff_matroska` 对 host 新 mux 的 21,438 字节 mkv 上 `ms_ff_open` 返回 `MALFORMED`（debug/O2/O3 通过；host `ffprobe` 可识别该文件）。未重编 size FFmpeg 前缀，不把该失败记成本次音频查表修复的回归。同目录已编产物上，后续 `test_hw_player`（含已生成 TS/FLV）、`test_ass_render: PASS`、`test_ai_sub: PASS`、`test_abi_old_header`（1838，`abi=1`）退出 0。`test_core` / `test_abit_parse` 在该 size 构建中已通过。

冻结 `build/release/libmsmedia.so.1.0.0` 仍为 166,168 字节，mtime **2026-11-21 11:57:55 CST**，本轮未覆盖。宿主 audit 不是 el7 正式发版。

未消毒 release `-O2` audit（strip，2026-11-22 05:37 CST）：

| 产物 | 字节 | NEEDED |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so.1.0.0` | 264,608 | 仅 libc；SONAME `libmsmedia.so.1`，版本节点 `MSMEDIA_1.0`；SHA-256 `816c85c03b2928829b8ffc4e3362b8f849330ed7f7adf4bcc69679a2dffe8da1` |
| 可选 `libmshw.so.1.0.0` | 113,568 | 仅 libmsmedia、libc（不 NEEDED libva） |
| 可选 `libmsff.so.1.0.0` | 8,680,568 | libxml2、libz、libm、libc；无 libssl/libcrypto |
| 可选 `libmsnet.so.1.0.0` | 35,432 | libmsmedia、libssl、libcrypto、libc（对照网络库，不链进核心） |
| 可选 `libmsass.so.1.0.0` | 2,559,576 | 仅 libz、libm、libc；不 NEEDED libmsmedia |
| 可选 `libmsai.so.1.0.0` | 34,984 | libmsmedia、libc；假供应商 only |

同核心 `libmsmedia.so.1.0.0`：`-O3` audit 330,144 字节；`-Os` size audit 199,056 字节。`-O3`/`-Os` 可选 ASS/AI：2,555,360 / 34,952 与 2,555,512 / 31,080。

非正式 `./build.sh android-hw` 于 **2026-11-22 05:45:00 CST** 退出 0，未重编 FFmpeg 前缀。内部 APK `/workspace/MsMedia/build/android-hw/MsHwPlayer.apk` 14,394,465 字节，SHA-256 `4b103436ca487a0cadb9da9a895140294d56aea4565db9e9fcd9e63d278f96a4`。已签名，**未安装、未启动共享 AVD**。不得把 §41 的 `780873f6…` 当作本修复后的 APK。冻结 v7a size 产物未覆盖（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST）。HEAD 未 commit/push。

| 产物 | v7a / v8a 字节 | 说明 |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so` | 196,528 / 199,656 | 公共 `ms_abit` 重链；NEEDED 仅 libc/libm/libdl |
| 默认核心 `libmsff.so` | 4,610,564 / 5,606,704 | 仅重链，未改前缀 |
| 默认核心 `libmshw.so` | 119,728 / 132,712 | 与 §41 相同量级 |
| 可选 `libmsass.so` | 1,478,856 / 1,966,168 | 与 §41 相同 |
| 可选 `libmsai.so` | 20,000 / 20,608 | 与 §41 相同 |

后续平台（只研究、未开新大任务）：Linux 缺 GPU 不等于不能写真实 VAAPI submit（picture/slice 缓冲、表面池、无设备时 fail-closed 并保持 `is_hardware=0`），但不得堆未验证伪实现。Windows：本机当前无 mingw；Debian trixie 有开源 `gcc-mingw-w64-x86-64` 14.2.0-17+27 等包，本机 `sudo -n` 可用，但本轮未安装。最小交叉编译可行的是核心 C ABI，不拉 Apple SDK，不操作用户电脑；MF/D3D11 头文件是否完整需安装后另作清点，不能预先声称 Windows 硬解可编。iOS 仍无 Apple SDK，不能称已编译。

本切片不是 el7/四平台或设备验收。仍未做：VAAPI 真实提交/呈现；Windows/iOS 编译与验收；el7 容器；P5 真实供应商实接；release-size 路径上 `test_ff_matroska` 的 mkv open；DASH 直播时移；XSUB/图文；统一交付。未向用户发阶段文件。

## 43. 收口 release-size `test_ff_matroska` MALFORMED（落后 size 前缀；2026-11-22 06:01 CST，Asia/Shanghai）

授权：构建功能一致性与关联回归，收口上轮 release-size 打开已承诺 MKV 失败。不是授权新平台工具安装；未装 mingw，未开其它平台或供应商任务。既有规则不变。无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包。`CONFIG_DECODERS=0`，硬解 only，AAC 例外与 TLS 保持。默认 `build/release` 与冻结 `build/android-v7a/size/libmsff.so.1.0.0` 未覆盖。

失败夹具与日志已保留在 `build/release-size-audit-20261122-repro/`：`h264_aac_subs.mkv` 21,438 字节，SHA-256 `b5124cdc3e6848ec1fd80ec20a4651e465b6c499ab94ef688929401e291117a0`（mtime 2026-11-22 05:43 CST），以及当时官方 size 矩阵 / 重试日志。

被测事实（不要预定成优化级别或编译器问题）：

- 失败构建：`build/release-size-audit-20261122/test_ff_backend` 静态链入当时的 `build/ffmpeg-9.1-size/prefix`。该 prefix 的 `libavformat.a` mtime **2026-11-21 12:05:01 CST**（matroska 切片之前）。`config_components.h` 为 `CONFIG_MATROSKA_DEMUXER=0`、`CONFIG_OGG_DEMUXER=0`、`CONFIG_WEBVTT_DEMUXER=0`；档案中无 `ff_matroska_demuxer`。`CONFIG_SMALL=1`、`CONFIG_DECODERS=0`。
- 对照 prefix `build/ffmpeg-9.1`（mtime **2026-11-22 03:46:16 CST**）已有 matroska/ogg/webvtt demuxer。
- 同一份字节输入探测三次：落后 size 前缀均为 `avformat_open_input failed` → `MS_ERR_MALFORMED`；同一文件在 debug ASan、release `-O2`、release `-O3` 下三次均为 `OK`，5 条轨。不是偶发。
- 根因是 size 前缀落后于当前 `scripts/ffmpeg9_build.sh` 的 `--enable-demuxer`（2026-11-22 00:59 起已含 matroska，随后还有 ogg/webvtt）。`ffmpeg9_build.sh` 旧逻辑只要 `libavformat.a` 存在且 OpenSSL pin 未变就跳过重建，却仍把**当前** configure 行写入 `configure-flags.txt`（该文件 05:42 被改写，看起来已开 matroska，实际库没有）。这不是 `-Os`/`--enable-small` 误编译，也不是 `ms_ff` 适配器把合法 MKV 判畸形；`MS_ERR_MALFORMED` 来自 `AVERROR_INVALIDDATA`（白名单含 matroska 但 demuxer 未编入）。

修复：

- 在新隔离前缀 `build/ffmpeg-9.1-size-audit-20261122` 经 `./build.sh` 重建（`--enable-small` / `-Os`，复用既有 `openssl-3.5.9-size`）。建成后 `CONFIG_MATROSKA_DEMUXER=1`、`CONFIG_OGG_DEMUXER=1`、`CONFIG_WEBVTT_DEMUXER=1`、`CONFIG_SMALL=1`、`CONFIG_DECODERS=0`。`libavformat.a` 13,772,046 字节，mtime **2026-11-22 05:53:12 CST**。落后的 `build/ffmpeg-9.1-size` 原样保留作对照，未覆盖。
- `scripts/ffmpeg9_build.sh`：当前请求写入 `configure-requested.txt`；真正建成后才写 `configure-flags.txt` 与 `configure.stamp`（含 pin、补丁哈希、CFG_ARGS）；跳过重建必须 stamp 一致且 `config_components.h` 覆盖本趟 demuxer/parser/bsf。当时还允许「缺 stamp 但组件已对齐则补记 stamp、不重建」（曾对 `build/ffmpeg-9.1` 走过该路径，`libavformat.a` mtime 仍为 03:46:16 CST）。该正确性边界在 §44 已收紧：不再凭组件清单签发指纹。过时前缀会明确说明并只重建该 FFmpeg 前缀。未全局强制重编 OpenSSL/ASS/默认 O2 前缀。
- 未降优化、未关告警、未跳过或缩窄 MKV 支持。未改产品 TLS。

官方矩阵（均走 `./build.sh`）：

- 完整 release-size：`FFMPEG_BUILD_ROOT=build/ffmpeg-9.1-size-audit-20261122` `./build.sh release-size --out-dir=build/release-size-audit-20261122-fresh --test`，**2026-11-22 05:56 CST** 退出 0，含 `test_ff_matroska ok`、`test_abit_parse`、`test_net`、`test_hw_vaapi`、`test_ass_render: PASS`、`test_ai_sub: PASS`、1838 旧头（`sizeof(ms_event)=176`，`abi=1`）。同一失败夹具用新 size 产物打开三次均为 `OK`、5 轨。
- release `-O2` 关联回归：`./build.sh release --out-dir=build/release-audit-20261122-mkvreg --test`，**2026-11-22 05:58 CST** 退出 0，含 matroska 与 1838。
- debug ASan/UBSan：`./build.sh test`，**2026-11-22 06:01 CST** 退出 0，含 matroska 与 1838。

未消毒 release-size 新 audit（strip，非 el7 发版）：

| 产物 | 字节 | NEEDED |
| --- | ---: | --- |
| 默认核心 `libmsmedia.so.1.0.0` | 199,056 | 仅 libc；SHA-256 `161948a061d7ae62f92d20c10334ab3cf6a5f72df73716e8504d4beb174fb90e` |
| 可选 `libmshw.so.1.0.0` | 84,776 | 仅 libmsmedia、libc |
| 可选 `libmsff.so.1.0.0` | 6,100,312 | libxml2、libz、libm、libc；无 libssl；SHA-256 `71d04c9c4ab53248145ccefbd1e416cefa44af1649103afa55887244446ec929` |
| 可选 `libmsass.so.1.0.0` | 2,555,512 | 仅 libz、libm、libc |
| 可选 `libmsai.so.1.0.0` | 31,080 | libmsmedia、libc |

Android 双 ABI 前缀与 `android-hw` 的 `libmsff` 已含 `ff_matroska_demuxer`（前缀 mtime 2026-11-22 03:52–03:54 CST），不受本机落后宿主 size 前缀影响，本轮未重编、未重签、未安装。冻结 v7a size `libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST。冻结 `build/release/libmsmedia.so.1.0.0` 仍为 166,168 字节，mtime 2026-11-21 11:57:55 CST。HEAD 未 commit/push。未向用户发阶段文件。

仍未做：把默认可写的 `build/ffmpeg-9.1-size` 原地换成新前缀（有意保留落后对照；此后若直接 `./build.sh release-size` 而不设 `FFMPEG_BUILD_ROOT`，脚本会检出过时并只重建该宿主 size 前缀）；VAAPI 真实提交/呈现；Windows/iOS 编译与验收（本轮明确不装 mingw）；el7 容器；P5 真实供应商实接；DASH 直播时移；XSUB/图文；统一交付。


## 44. 收紧 FFmpeg 前缀 stamp 签发（不得凭组件清单认证；2026-11-22 06:22 CST，Asia/Shanghai）

授权：仍在同一构建功能一致性修复内做最后代码审查与可重复测试，不新增平台或功能。既有规则不变。无 CloudAgent / machineId / 共享 AVD / commit / push / UPX / 阶段包 / 新 APK。`CONFIG_DECODERS=0`，硬解 only，AAC 例外与 TLS 保持。冻结历史制品与归档未改、未确认删除未执行。

审查（先核对代码与证据，再决定）：`scripts/ffmpeg9_build.sh` 先前第二条跳过分支在 stamp 缺失或与期望不一致时，只要 OpenSSL pin 与 `ms_ff_components_match`（`config_components.h` 的 demuxer/parser/bsf 宏，外加 `CONFIG_DECODERS!=1` 和 size 时的 `CONFIG_SMALL`）成立，就把期望指纹 `mv` 成 `configure.stamp` 并跳过重建。组件齐全不能证明归档已应用当前 HLS 补丁、extra-cflags、tarball 版本或（除 pin 文件对比外的）OpenSSL 配置。该分支在 stamp 已存在但哈希/参数不同、而 demuxer 清单未变时，也会覆盖旧 stamp 并跳过，把不确定来源的旧库认证成当前配置。这是真实正确性缺口，不是文档笔误。

`strings` 对注释中的 `MsMedia:` 标记不可靠（默认 / 落后 size / 新 size 三份 `libavformat.a` 均无该注释字符串）。可作已构建证明的是补丁引入的字面量：默认前缀与 `build/ffmpeg-9.1-size-audit-20261122` 的 `libavformat.a` 含 `msmedia.hls.playlist` / `window_start` / `window_end` / `start_seq` / `target_duration` 与 `X-TIMESTAMP-MAP=`；落后对照 `build/ffmpeg-9.1-size`（mtime 2026-11-21 12:05:01 CST，Asia/Shanghai）没有这些字符串。默认前缀 `build/config.h` 的 `FFMPEG_CONFIGURATION` 与当前 O2 `CFG_ARGS` 一致（含 matroska/ogg/webvtt 与 `-O2`），`CONFIG_DECODERS=0`，prefix 内 OpenSSL pin 文件 mtime 为 2026-11-22 03:46:17 CST。因此 05:52 由旧引导路径签发的 `build/ffmpeg-9.1/configure.stamp` 对本份归档事后可核对为内容匹配；按「无足够证明则重建、不凭组件清单签发」的规则保留该 stamp 与库，未删除、未再签发、未原地重编默认前缀。`configure-flags.txt` 仍是旧跳过路径在 05:39 写下的 “archived …” 头，新逻辑在完全匹配时不再改写它。

修复：删除「组件对齐则补记 stamp」。复用前缀的唯一充分证明是 OpenSSL pin 一致，且已有 `configure.stamp` 与本趟期望逐字节相同，再附加组件核对。缺 stamp、stamp 不一致或组件不符时，只重建该 FFmpeg 前缀；`configure.stamp` 与建成用 `configure-flags.txt` 仅在 `make install` 成功之后写入。匹配成功则跳过，不改 flags、不改 stamp。构建失败（含自测注入失败）在写 stamp 之前退出。隔离自测入口：`./build.sh ffmpeg-cache-selftest`（`tests/test_ffmpeg9_prefix_cache.sh`）；可用 `MS_FF_CACHE_CHECK_ONLY=1` 只做决策。`MS_FF_FAKE_INSTALL` 仅允许 `ffmpeg-cache-selftest` 目录，用于验证失败不写 stamp；在该根目录之外会被拒绝。未改真实补丁与共享源码，未污染冻结制品。

官方测试（均走 `./build.sh`，未重制 APK）：

- 隔离缓存自测：`./build.sh ffmpeg-cache-selftest`，六项全部 PASS（请求 flag 改为 `MS_SIZE_OPT=1`；补丁内容变化只改隔离 stamp 哈希、不改真实补丁；无 stamp 且组件齐全但档案较旧；注入构建失败不写成功 stamp；完全匹配不重建且 flags/stamp/档案不变；假安装在自测根目录外被拒绝）。
- 当前真实前缀决策：默认 `build/ffmpeg-9.1` 与 `build/ffmpeg-9.1-size-audit-20261122` 走真实 `ffmpeg9_build.sh` 均为 stamp 匹配并跳过，`libavformat.a` / `configure.stamp` / `configure-flags.txt` mtime 与 SHA-256 不变。对落后 `build/ffmpeg-9.1-size` 只做 `MS_FF_CACHE_CHECK_ONLY=1`：退出 3，`cache-decision: REBUILD`，明确拒绝「只凭组件签发 stamp」，未写入 stamp，档案与 flags 未动。
- debug ASan/UBSan：`./build.sh test`，**2026-11-22 06:16 CST** 退出 0，FFmpeg 前缀 stamp 匹配跳过，含 `test_ff_matroska ok` 与 1838（`sizeof(ms_event)=176`，`abi=1`）。
- release `-O2` 关联回归：`./build.sh release --out-dir=build/release-audit-20261122-mkvreg --test`，**2026-11-22 06:19 CST** 退出 0，同样跳过前缀并含 matroska 与 1838。
- release-size 关联回归：`FFMPEG_BUILD_ROOT=build/ffmpeg-9.1-size-audit-20261122` `OPENSSL_BUILD_ROOT=build/openssl-3.5.9-size` `./build.sh release-size --out-dir=build/release-size-audit-20261122-fresh --test`，**2026-11-22 06:22 CST** 退出 0，fresh size 前缀 stamp 匹配跳过，含 matroska 与 1838。未对默认可写的落后 `build/ffmpeg-9.1-size` 做原地重建。

冻结对照仍完整：`build/release/libmsmedia.so.1.0.0` 166,168 字节，mtime 2026-11-21 11:57:55 CST；`build/android-v7a/size/libmsff.so.1.0.0` 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST；失败夹具 `build/release-size-audit-20261122-repro/h264_aac_subs.mkv` SHA-256 `b5124cdc3e6848ec1fd80ec20a4651e465b6c499ab94ef688929401e291117a0` 未改。三份 HLS 补丁哈希未变。HEAD 未 commit/push。未向用户发阶段文件。

仍未做（本轮不自行开始）：把默认可写的 `build/ffmpeg-9.1-size` 原地换成新前缀（有意保留落后对照；直接 `./build.sh release-size` 而不设 `FFMPEG_BUILD_ROOT` 会因无 stamp 只重建该宿主 size 前缀）；默认 O2 前缀的 `configure-flags.txt` 仍带旧 “archived” 头，不是本趟建成记录；VAAPI 真实提交/呈现；Windows/iOS 编译与验收（不装 mingw）；el7 容器；P5 真实供应商实接；DASH 直播时移；XSUB/图文；统一交付。

## 45. Android 播放生命周期：Surface 重建与音频焦点（2026-11-22 15:57 CST，Asia/Shanghai）

授权：用户「继续」，并纠正主线「Android为主」。未做 Linux VAAPI 提交/呈现，未装 mingw，未开 Windows/iOS。既有规则不变。无 CloudAgent / machineId / 用户电脑 / 共享 AVD / commit / push / UPX / 阶段包。`CONFIG_DECODERS=0`，视频硬解 only，库默认音频硬解 only（仅既有显式 AAC 系统例外），TLS 保持。ABI=1，1838 布局保持。

先核对文档与代码再选题。AndroidCAStore 后台导出+PEM 缓存已在；首帧计时已区分 `mark_request` / `first_output` / `note_display`，无把 codec 输出写成出画的回归。API 16–20 视频 JNI 仍仅有 API 21 工具链编译校验，音频仍走 AMediaCodec（API 21+），本轮不伪称旧系统 ABI 已闭环。MediaCodec 真机拆合/空输出/delay 与 skip_end 双扣仍待设备。本轮选择对当前测试 APK（minSdk 21）最有实质价值、且本机可验证的缺口：**Surface 销毁重建与音频焦点生命周期**。

发现并修复的真实缺陷：每次 `surfaceCreated` 重开 Intent 会话；JNI 表面置空调用会关掉音频的 `release_decoder`；无音频焦点，且 `onPause` 若误用 `nativePause` 会把生命周期暂停当成用户暂停。实现见 `docs/HW_ANDROID.md` §42。宿主 `test_surface_loss_keeps_audio_and_focus` 覆盖：同指针不拆、丢失表面后音频继续而视频不提交、非关键帧丢弃、新表面关键帧重提、焦点 LOSS 暂停、GAIN 恢复、GAIN 不清除用户暂停、用户暂停跨越表面重建、`CAN_DUCK` 按暂停。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 于 **2026-11-22 15:56 CST** 退出 0，含该测试、`test_ff_matroska` 与 1838。因 JNI/Java/APK 路径变动，非正式双 ABI 经 `./build.sh android-hw` 重链（FFmpeg 前缀未重建，stamp 逻辑未改），内部 APK 14,394,465 字节，SHA-256 `1c64286eda65443a8fff1f6021f99535c42599d6795f9e7a859f1a242ac8fe04`，已签名未安装。冻结历史制品未覆盖。HEAD 未 commit/push。未向用户发阶段文件。

仍欠（本轮不自行开始）：真机/隔离 AVD 上的 Surface 重建、前后台与音频焦点、出画/出声与 1 秒首帧；AndroidCAStore 真机观察；API 16–20 音频 JNI 与旧系统 ABI 验收（NDK r29 不能以 API 16 为 target）；MediaCodec/AudioTrack 真机契约（拆合/空输出/平台 delay 与 skip_end、部分 write）；字幕/ASS/位图真机叠加视觉；P5 真实供应商；el7；Windows/iOS（不装 mingw）；VAAPI 真实提交；统一交付。

## 46. Android 宿主：platform_trims / leftover / 表面丢失换 CSD 与焦点下 seek（2026-11-22 16:12 CST，Asia/Shanghai）

授权：用户主线仍是「Android为主」且「全部都做好再给我」，不发阶段包。不重复上一轮 Surface/焦点切片，不把该项写成真机已验收。未做 Linux VAAPI 提交/呈现，未装 mingw，未开 Windows/iOS。既有规则不变。

选题证据：leftover 与 trim 在 `platform_trims_delay=0` 下已有拆合/空输出/配额用例，但 Android 开音频时设置的 `platform_trims_delay=1` 分支（不双扣 start、expected=0、仍扣 skip_end）没有会话级宿主证明；既有 partial+flush 用例先写完 leftover 再 flush。播放器上一轮覆盖了表面丢失与焦点，但未覆盖无表面时的中途 CSD 变更，也未覆盖焦点暂停中的 seek。未选择只能等设备的 AndroidCAStore 观察、API 16–20 旧系统 ABI、字幕视觉或真机 MediaCodec 契约本身。

本轮未发现需要改产品修剪公式的缺陷：flush 中途 leftover、暂停续写、表面丢失后收下新 CSD 并在重绑时 configure、焦点下 seek 保持暂停，在补测后均按既有代码成立。发现并修复的测试空洞：`sub_toc` 在带 skip 的 `submit_au` 未写 `pts_us`；`expected=0` 的 AU 边界依赖 PTS，缺 PTS 时后一包 skip_end 不会生效（首次反例 n=1608 而非 960）。新增宿主调试钩子与 mock leading 修剪，只用于证明 Android 旗标路径。详见 `docs/HW_ANDROID.md` §43。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 于 **2026-11-22 16:12 CST** 退出 0，含 `test_hw_audio_trim_via_mock`（leftover-flush/pause/platform_trims）、`test_surface_loss_keeps_audio_and_focus`、`test_csd_change_and_seek_while_unbound_or_paused`（含 TS/FLV 矩阵内再跑）、`test_ff_matroska` 与 1838。JNI/Java/APK 未改，未重链 android-hw；FFmpeg 前缀未重建。冻结对照未改：`build/release/libmsmedia.so.1.0.0` 166,168，SHA-256 `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 未 commit/push。未向用户发阶段文件，未自行另开新平台任务。

仍欠（本轮不自行开始）：真机/隔离 AVD 上的 Surface 重建、前后台与音频焦点、出画/出声与 1 秒首帧；AndroidCAStore 真机观察；API 16–20 音频 JNI 与旧系统 ABI 验收（NDK r29 不能以 API 16 为 target）；MediaCodec/AudioTrack 真机契约（拆合/空输出、平台 delay 与 skip_end、部分 write、playback head）；`expected=0` 在缺 PTS 或时间戳相同时尚依赖 EOS/PTS 切 AU，真机若出现无 PTS 包仍须观察；字幕/ASS/位图真机叠加视觉；P5 真实供应商；el7；Windows/iOS（不装 mingw）；VAAPI 真实提交；统一交付。

## 47. 修复 platform_trims 在缺 PTS / 相同 PTS 下 skip_end 不生效（2026-11-22 16:21 CST，Asia/Shanghai）

授权：用户主线仍是「Android为主」且「全部都做好再给我」。不重复上一轮生命周期与既有 leftover/platform_trims 证明。先核查上一轮暴露的 `expected=0` 靠 PTS 切 AU 的产品空洞，不只改测试。

核查：JNI 本身只是把 `ms_ff_pull` / `ms_session_pull` 的事件喂给播放器。`ms_ff` 在 `AV_NOPTS_VALUE` 时不置 `has_pts`；会话层复制帧标志；TS PES 可以无 PTS。播放器在无 PTS 时提交 `pts_us=0`，修剪层还把缺 PTS 写成 `has_pts=1`。因此缺 PTS 与时间戳相同都会让后一包 `skip_end` 失效，不是测试助手独有的问题。乱序 PTS 已是 `STATE`。seek 后整包 leading 还会留下粘连 meta。

修复见 `docs/HW_ANDROID.md` §44：按平台 leading 计算 expected，整包 leading 不入队，加法 `has_pts` 诚实下发。反例 `via_platform_trims_no_or_same_pts`。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 于 **2026-11-22 16:21 CST** 退出 0，含既有 trim/生命周期/`test_ff_matroska`/1838（`sizeof(ms_event)=176`，abi=1）。JNI/Java/APK 未改，未重链 android-hw（非正式 APK 尚未含本修复）。FFmpeg 前缀未重建。冻结对照未改：`build/release/libmsmedia.so.1.0.0` 166,168，SHA-256 `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。HEAD 未 commit/push。未交包、未安装、未另开新平台任务。不是真机出声/出画或 1 秒首帧验收。

仍欠：真机/隔离 AVD 上的 Surface 重建、前后台与焦点、出画/出声与 1 秒首帧；AndroidCAStore 真机观察；API 16–20 音频 JNI 与旧系统 ABI；真机 MediaCodec/AudioTrack 契约（拆合/空输出、平台实际 delay 与 csd-1/csd-2 是否恰好等于本层 leading 计数、部分 write、playback head）；帧长未知时 `expected=0` 的 PTS/EOS 回退在真机上的表现；字幕视觉；P5 供应商；el7；Windows/iOS（不装 mingw）；VAAPI 提交；统一交付；下一次改到 JNI/Java/APK 或授权重链时再把本修复打进非正式 APK。

## 48. 非正式 android-hw 重链纳入缺 PTS skip_end 修复（2026-11-22 16:23 CST，Asia/Shanghai）

授权：用户主线仍是「Android为主」且「全部都做好再给我」，不向用户发阶段包、不安装、不启动共享 AVD。用现有 `./build.sh android-hw` 把上一轮公共路径修复重链进内部非正式双 ABI APK。JNI/Java 未改。FFmpeg 前缀未重建，只从既有 Android size-opt 前缀重链 `libmsff.so`。未覆盖冻结产物，未 commit/push，未删除历史归档。

`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh android-hw` 于 2026-11-22 16:22 CST 退出 0，`apksigner verify --min-sdk-version 21` 通过。新包 `build/android-hw/MsHwPlayer.apk` 14,394,465 字节，SHA-256 `d7c2149e9e650c07d9885b9057bf4d5de5fa5df2c331350f128cb3266ec16d67`，mtime 2026-11-22 16:22:50 CST（Asia/Shanghai）。包内 so 与 NEEDED 见 `docs/HW_ANDROID.md` §45。不得把上一轮同一路径 APK（SHA-256 `1c64286eda65443a8fff1f6021f99535c42599d6795f9e7a859f1a242ac8fe04`，mtime 15:57:42 CST，`libmshw.so` 120,708 / 133,984）当作本修复实现；本轮 `libmshw.so` 为 120,868 / 134,048。字节数碰巧与上一轮相同，哈希不同。

重链后诚实审查：不再重复已完成的 Surface/焦点、leftover/delay、缺 PTS 修剪。本机已没有可证明的新实质缺陷值得再改一刀。剩余项都需要真机/隔离 AVD 或其它外部条件（出画/出声与 1 秒首帧、AndroidCAStore 真机观察、API 16–20 旧系统 ABI、真机 MediaCodec/AudioTrack 契约含平台实际 delay 是否恰好等于 leading 计数、字幕视觉、P5、el7、Windows/iOS、VAAPI 提交、统一交付）。未再堆无增量 mock 用例，未改产品源码，因此未再跑官方 ASan/UBSan 全矩阵。HEAD 未 commit/push。未向用户发阶段文件，未自行另开新平台任务。

## 2026-11-23 12:43（北京时间，UTC+8）剩余验收中途记录

非正式模拟器验收（CJZB_API34 TCG，非真机硬解，非 el7）。被中断后从现场续做。冻结产物哈希未变。

### 已落地的修复（源码已在树内，官方 test 已过）

- FORTIFY / CodecLooper：根因是 `AMediaCodec_stop` 后再 `delete`，looper 在已销毁互斥量上加锁。最终关闭顺序为：`closing=1` → 摘 OnFrameRendered 回调 → 若已 start 则 `flush` 归还 buffer → **只** `AMediaCodec_delete`（内部 release+join）→ 指针置空。音频路径同样不单独 stop，先 pause AudioTrack 再 flush+delete。`mshw_frame_dump` 在 c2.android.avc.decoder 上 `DUMP_EXIT=0` 且日志无 FORTIFY（`build/avd-accept-20261123/dump/run-final-c2_android_avc_decoder.log`）。goldfish dump 一次 180s 超时未写出日志，待补。
- 测试 Activity：`com.msmedia.DEBUG_CMD` 广播（seek/next_audio/next_sub/sub_off/pause/resume/stats/pos），`singleTop` + `configChanges`。
- 累加字段：`audio_track` write_frames/write_bytes/head_frames，`av_sync` video_pts 与 audio_clock 差。
- x86_64 `ass_deps` 原先误用 aarch64 工具链，已在 `scripts/ass_deps_android_build.sh` 补 `x86_64-linux-android` 映射；测试 APK 含 libmsass。

### 官方 test 与 APK（本轮最终包，未再改产品源码则不再重链）

- 官方 `PATH=/usr/bin:/bin ./build.sh test`：2026-11-23 12:32:40 Asia/Shanghai，EXIT=0，`sizeof(ms_event)=176`，abi=1。
- 默认双 ABI（含 libmsass）：`build/android-hw/MsHwPlayer.apk` 14,414,945 字节，mtime 2026-11-23 12:33:00 Asia/Shanghai，SHA-256 `34dd1d8fe2704d8b3400266f2da5f10fc29e113b4b49affcc9de9c8484de22b0`。
- x86_64 测试包（含 libmsass）：`build/android-hw-x86_64/MsHwPlayer.apk` 8,770,278 字节，mtime 2026-11-23 12:33:08 Asia/Shanghai，SHA-256 `efd4ddb4b74575db48dbda5af1c0ce8593e8b8764ebdc3965021bc1dbe53fc69`。已安装到 CJZB_API34。

### 已取证（续做前）

- 项 4：默认不带 `force_decoder`，App Surface 出画。截图 `build/avd-accept-20261123/shots/30-goldfish-default-surface.png`（彩条可见；当时叠了 Digital Wellbeing ANR 框，不是出画失败）。session 走 `c2.goldfish.h264.decoder`（模拟器虚拟/软件解码，`is_hardware` 名称分类不得写成真机硬解）。`first_output_to_surface=10907.240` ms。stats：`build/avd-accept-20261123/stats/goldfish-default.txt`。同段已见 `audio_track write_frames` 从 0 增到 9216，`av_sync have=1 delta_us=71021`。
- 项 1 部分：dump 关闭无 FORTIFY；App `force-stop` 后 teardown 摘录见 `stats/goldfish-teardown.txt`（待与 long_multi 退出一并再核）。
- 中断时 `long_multi.mkv` 已在播：pid 15127，同样默认 goldfish，`first_output_to_surface=25660.037` ms，`timeline_epoch=1`，`audio_track write_frames=75776 head_frames=75776`，`av_sync delta_us=-396666`，`tracks audio_id=2 sub_id=4`。交互/旋转/前后台/ASS 叠加尚未截完。

### 仍待本切片完成

seek / next_audio / next_sub / sub_off / pause / resume 的 adb 广播 + 截图和 stats；中途旋转与前后台/Surface 重建（同一 timeline_epoch）；ASS 或位图字幕叠加截图；goldfish dump 关闭补测。不得写成真机硬解或扬声器听感（AVD `-no-audio`）。

## 2026-11-23 12:52（北京时间，UTC+8）剩余验收结果

续 12:43 记录。未再改产品源码，未再跑官方 test，未再重链 APK。模拟器结果不是真机硬解，也不是扬声器听感（AVD `-no-audio`）。

### 项 1 FORTIFY

- 关闭顺序已修：摘回调 → 已 start 则 flush → 只 `AMediaCodec_delete`，禁止先 `stop` 再 `delete`。
- dump `c2.android.avc.decoder`：`DUMP_EXIT=0`，日志无 FORTIFY。证据：`build/avd-accept-20261123/dump/run-final-c2_android_avc_decoder.log`、`frame-final-c2_android_avc_decoder.png`。
- App 两次 `force-stop`（12:40:25 / 12:51:51 WIN DEATH）在 `logcat-remain-accept.txt` 中未见 CodecLooper FORTIFY / Fatal signal。
- goldfish dump 关闭补测：180s 超时日志为空，未写出 ppm；同一 close 路径已在 c2.android dump 验证。不把该项写成已用 goldfish dump 复验。

### 项 4 默认 goldfish App Surface 出画

- 无 `force_decoder`。组件 `c2.goldfish.h264.decoder`（模拟器 goldfish/软件，名称分类 `hw=1` 不得写成真机硬解）。
- 截图 `shots/30-goldfish-default-surface.png`：彩条出画（当时叠 Digital Wellbeing ANR）。
- `first_output_to_surface=10907.240` ms。stats：`stats/goldfish-default.txt`。

### 项 5 AudioTrack / 音画同步粗测

- 同一 p4_seek 会话：`write_frames` 0→9216，`head_frames=9216`，`av_sync have=1 video_pts_us=280000 audio_clock_us=208979 delta_us=71021`。
- `long_multi.mkv` 默认 goldfish：`write_frames=75776 head_frames=75776 clock_us=1578666`，`av_sync have=1 video_pts_us=1182000 delta_us=-396666`。随后因 TCG / `-no-audio` / 系统 ANR，位置和写入帧停在 `pos_90k=163980`，不能当成持续实时播放。不是扬声器听感。

### 项 6 ASS 叠加

- x86_64 测试 APK 已含 libmsass。`long_multi.mkv` 画面可见黄/红 “ASS-ENG color”。
- 证据：`shots/31-long-playing-ass.png`、`31b-long-ass-no-anr.png`（仍可能叠系统 ANR 框）、旋转后 `38-long-rotated.png`。未再验独立位图字幕（PGS 等）。

### 项 2 adb 指令式交互

广播均送达（`debug_cmd=` 已入日志）：
- pause/resume：`user_paused` / `paused` 0→1→0，`timeline_epoch` 仍为 1。截图 32/33。因时间轴已冻结，写入帧未再增减。
- next_audio：列出 5 轨后 `select id=3 on=1 queued`（fra AAC）。stats 的 `audio_id` 仍为 2，`audio_switch` 仍 0——切换是异步排队，worker 当时卡住，完成态未在 stats 里观察到。
- next_sub：`select id=5 on=1 queued`（fra SRT）。
- sub_off：`select id=4294901761` 与 `id=4 on=0 queued`。截图 36 仍可见 ASS（排队未及时生效）。
- seek 20s：`seek 1800000 queued`。截图 37 拍摄时画面仍是片头彩条+ASS。稍后 worker 跑起来，`pos_90k=1800000`，`timeline_epoch=2`（seek 换 epoch，不是新 Activity）。证据在 `logcat-remain-accept.txt` 与 `stats/long-09-home.txt`。

样例：`samples/long_multi.mkv`（60s，H.264 + 双 AAC + ASS/SRT）。

### 项 3 旋转与前后台

- `settings put system accelerometer_rotation 0` 且 `user_rotation 1` 成功（`stats/rotation-user_rotation.txt` 为 1）。
- 截图 `38-long-rotated.png` 为 480×800，彩条+ASS 仍在，旋转期间 `timeline_epoch=1`，同一进程 15127，未重开会话。
- HOME：`40-long-home.png`（桌面），`surface_bound=0`，焦点丢失，`paused=1`。这是放弃音频焦点，不是用户 pause。
- 返回：`am start` singleTop，`set_surface st=OK win=yes`，同一 pid，位置仍 1800000。截图 `41-long-fg.png` 画面为黑；抓 stats 时仍 `focus=2` `paused=1`，`write_frames` 未再升，`av_sync have=0`。**不能写成本次已证明“回来后继续出画、音频不断”。** 会话对象未销毁，但出画恢复未拍到。

### APK（最终，未再改源码）

- 默认双 ABI+libmsass：`build/android-hw/MsHwPlayer.apk` 14,414,945 B，2026-11-23 12:33:00 Asia/Shanghai，SHA-256 `34dd1d8fe2704d8b3400266f2da5f10fc29e113b4b49affcc9de9c8484de22b0`。
- x86_64 测试+libmsass：`build/android-hw-x86_64/MsHwPlayer.apk` 8,770,278 B，2026-11-23 12:33:08 Asia/Shanghai，SHA-256 `efd4ddb4b74575db48dbda5af1c0ce8593e8b8764ebdc3965021bc1dbe53fc69`。
- 冻结产物未动：`build/release/libmsmedia.so.1.0.0` `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。
- 官方 test：2026-11-23 12:32:40 Asia/Shanghai，EXIT=0，`sizeof(ms_event)=176`，abi=1。

### 仍未验及原因

- 真机硬解 / 扬声器听感 / 1 秒首帧设备验收；API 16–20；P5 真厂商；el7；Windows/iOS；VAAPI 真提交；HDR（设计仍为待评估纳入）。
- goldfish dump 销毁补测（超时）。
- 音轨/字幕切换与 sub_off 的 **完成态**（命令已排队，stats/画面未跟上）。
- 前后台返回后继续出画与音频写入继续爬升（41 为黑帧）。
- 独立位图字幕轨（本轮只验了 ASS）。

## 2026-11-23 13:03 Asia/Shanghai — 回前台黑帧与命令队列缺陷（进行中）

产品缺陷（不要先归因 ANR）正在修：

1. Surface 拆/重绑后 `need_keyframe_resync`，可 seek 源由 JNI 对当前位置内部 seek（向关键帧）；直播只等 IDR。暂停时 `drive` 仍抽视频并尽快出一帧，不推进音频。`onResume` 非用户暂停时始终重新申请焦点并通知 GAIN。
2. JNI `feed_retry` 背压等待中让出 pending；选轨队列 8 槽，UI 线程立即 `select_track`/`clear_subtitles`；`sub_off` 立刻隐藏 ASS。宿主反例测试已写入 `tests/test_hw_player.c`。
3. 随后跑官方 `PATH=/usr/bin:/bin ./build.sh test`，重链两个 APK，模拟器复验，goldfish dump 加长超时。

本段为过程记录，不是验收结论。模拟器 ≠ 真机硬解。


## 2026-11-23 13:33 Asia/Shanghai — 回前台黑帧 / 命令队列 / goldfish dump

本段为过程与模拟器复验记录，不是 el7，也不是真机硬解或扬声器听感。HDR 仍未实现（设计仍为待评估纳入）。

### 查清的产品原因（不要先归因 ANR）

1. **回前台黑帧**
   - `set_surface(NULL)` 只拆视频解码器并 `waiting_key=1`，重绑后若只等下一个 IDR，长 GOP 会长时间黑屏。
   - `ms_hw_player_drive` 在 `paused` 时曾经直接返回，重绑后即使喂入关键帧也不吐帧。
   - `onResume` 仅在 `requestAudioFocus` 成功时才通知 GAIN；失败则 `focus_paused` 一直挡住出画。
   - 模拟器上还有一层后果：UI 轮询在 worker 持有 `op`（goldfish `AMediaCodec_configure/start` 可达约 20s）时阻塞，触发 `FocusEvent` ANR 后进程被 cached app freezer 冻结，后续 DEBUG 广播只入队不执行。这是锁持有时间过长，不是“先怪 ANR”。

2. **命令已排队未完成**
   - JNI `feed_retry` 在 `MS_LIMIT/MS_AGAIN` 背压循环里从不调用 pending seek/select，worker 回不到 pump 顶部，命令一直停在单槽 pending。
   - 单槽会被后一条（如 `next_sub`）覆盖前一条（`next_audio`）。
   - `nativeSelectTrack` 在 worker 存活时只入队、不立即改播放器，stats 的 `audio_id` 与 ASS 绑定要等 pump；`sub_off` 只清 Java `cueView`，libass 栅格仍被 poll 画回去。

### 修复（加法 ABI，仍为 1，未改 `ms_event`）

- 重绑后 `need_keyframe_resync`；可 seek 源 `take_surface_resync` 给出当前位置，JNI 内部 seek 向关键帧；直播 `seekable==0` 只等 IDR。
- 暂停时仍 drive 视频并尽快出一帧，不推进音频。
- `onResume`：非用户暂停时始终重新申请焦点并通知 GAIN。
- JNI：背压等待中让出并消费 pending；选轨 8 槽队列；UI 线程立即 `select_track` / `clear_subtitles`；`sub_off` 立刻隐藏 `assView`。
- UI 对 `op` 使用短超时；`set_surface` 在 UI 取 `ANativeWindow` 后入队，worker 再绑，避免 configure 期间卡死主线程。
- `mshw_frame_dump` 将 stdout/stderr 设为无缓冲，避免超时被杀后日志为空。

### 宿主反例测试

`tests/test_hw_player.c`：
- `test_surface_rebind_resync_and_paused_present`：可 seek 重绑给出 resync 位置；暂停态喂关键帧 + drive 出一帧且音频钟不前进；直播 take 返回 0。
- `test_select_seek_and_sub_off_apply_without_feed`：选轨/seek 不依赖下一次成功 feed 即改 stats；`clear_subtitles` 立即清 cues。

### 官方 test

`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`，2026-11-23 约 13:06 Asia/Shanghai，EXIT=0。新宿主测试两次均 ok。`sizeof(ms_event)=176`，abi=1。测试在 `build/debug`。此后仅改了 Android JNI/Java 的 UI 锁与 Surface 入队（官方宿主路径不编此文件）。

### goldfish dump 销毁补测

更长等待 + nohup + 无缓冲日志。证据：
- `build/avd-accept-20261123/dump/run-goldfish-retest.log`：`configured name=c2.goldfish.h264.decoder`，`got frame 160x120`，`done got=1`，`EXIT=0`。
- 帧：`dump/frame-goldfish-retest.ppm`（57615 B）与 `dump/frame-goldfish-retest.png`。像素非空（mean≈126.8）。
- `dump/logcat-goldfish-retest-fortify.txt` 无本次 dump 的 FORTIFY / Fatal signal。关闭顺序仍是 flush + `AMediaCodec_delete`、不单独 stop。
- 组件名分类 `is_hardware=1` 不得写成真机硬解；这是模拟器 goldfish 虚拟/软件路径。

### 模拟器 long_multi.mkv 复验（CJZB_API34 TCG，x86_64 测试 APK，默认无 force_decoder）

证据目录：`build/avd-accept-20261123/retest2/`。解码器仍是模拟器 goldfish/c2 路径，不是真机硬解。AVD `-no-audio`，音频只看 `write_frames` / head，不是听感。

- **切音轨：已验。** `01-next-audio.txt`：`select id=3 on=1 player=OK queued-demux`，随后 `audio_id=3 audio_switch=3 sel_epoch=4`（由 2 切到 fra/id=3）。截图 `shots/61-next-audio.png`。
- **关字幕：已验（stats + 立即 clear）。** `clear_subtitles -> OK`，`sub_id=0 sel_epoch=5`，截图 `63-sub-off.png`（71098 B，小于关之前的 `62-next-sub.png` 75426 B）。粗 8×8 指纹仍以彩条为主，不能单独当成字形级“ASS 已消失”的像素证明。
- **切字幕到 fra/id=5：未在 stats 完成态看到。** `select id=5 on=1 queued (player busy)`，随后 `sub_id` 仍为 4，直到 `sub_off`。worker 当时仍可能卡在 goldfish 背压（`au_in=43 submitted=43`，`pos_90k` 常停在 163980 / `write_frames=75776` ≈ 1.58s 音频）。
- **seek 到 20s：未证画面/位置变化。** 广播超时（rc=124），日志无 `seek queued/applied`，`timeline_epoch` 未因这次 seek 增加。`65-seek20.png` 偏暗（mean≈15.8），不能写成 20s 画面。
- **回前台出画：有出画，不是黑帧。** `68-fg.png` / `69-fg-later.png` 彩条可见（中心 mean≈132 / 131，dark=0）。同 pid 20975。但 `am start --activity-single-top` 时带了 path，13:30:41 起 `sel_epoch=1 audio_switch=0`，更像新开会话，**不能写成同一会话从 HOME 前 `write_frames` 连续爬升**。HOME 前已出现 `surface_bound=0 focus=2`。新会话同样会在约 1.58s 停在 `write_frames=75776`（goldfish 背压仍在）。

### APK（含上述修复与 libmsass，非正式包）

- 默认双 ABI+libmsass：`build/android-hw/MsHwPlayer.apk` 14,419,041 B，2026-11-23 13:22:44 Asia/Shanghai，SHA-256 `04764d99330adca581476fd8734b86cc1737edf50bfffd6add69f782fe716744`。v7a/v8a 均含 libmsai/libmsass/libmsff/libmshw/libmsmedia。
- x86_64 测试+libmsass：`build/android-hw-x86_64/MsHwPlayer.apk` 8,774,374 B，2026-11-23 13:22:14 Asia/Shanghai，SHA-256 `ee31a108145ac524a7bd888dca19e47e526a9abd1ca180130cf95717d8505f5d`。
- 冻结产物未动：`build/release/libmsmedia.so.1.0.0` `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

### 仍未验及原因

- 真机硬解 / 扬声器听感 / 1 秒首帧设备验收；API 16–20；P5 真厂商；el7；Windows/iOS；VAAPI 真提交；HDR。
- 同一会话 HOME→回前台后 `write_frames` 连续爬升（本次 FG 带 path，像新会话）。
- seek 后位置与画面内容变化（广播超时，未见 seek applied）。
- 切到非默认字幕轨 id=5 的完成态。
- goldfish 上 `au_in=43` / `write_frames=75776` 附近的持续背压（出画后输入停住）；`sub_off` 仍会向 ff 入队 `0xFFFF0001`，ff 回 INVALID_ARG（播放器侧已 clear，属噪声）。
- 独立位图字幕轨。


## 2026-11-23 13:40 Asia/Shanghai — 1.58s 停播死锁（进行中）

初步结论：视频 `releaseOutputBuffer` 在 dequeue 时立即发生，vpend 只做统计。更可能的互相阻塞是：JNI 泵在一条视频 AU 的 LIMIT 上循环调用 `drive`，而 `ms_hw_audio_android_drive` 使用阻塞式 `AudioTrack.write([BII)I`。AVD `-no-audio` 下轨道缓冲写满后 write 不返回，视频 drive 进不去，解码器输入满，解复用不再取音频，时钟也不走。即使 goldfish 慢，阻塞 write 也会让整条管道停死。

计划：非阻塞 write；sink 停转时丢弃 PCM 并计入写入、用单调时钟兜底；选轨在 worker 补做 player select；`sub_off` 不再向 ff 入队 `0xFFFF0001`。宿主反例：音频写配额耗尽（时钟不走）时视频仍可提交/出帧。随后官方 test、重链、模拟器连播 30s 以上。

本段为过程记录，不是验收结论。



## 2026-11-23 14:25 Asia/Shanghai — 1.58s 停播修复（过程）

本段为过程记录，不是 el7 / 真机硬解 / 扬声器听感。HDR 仍未实现。

### 原因

视频 `releaseOutputBuffer` 在 dequeue 时立即发生，vpend 只做统计，不是“等音频钟不 release”。真正互相阻塞是：

1. 共享 `drive` 里 `AudioTrack.write` 在 AVD `-no-audio` 下会堵住（即便声明 `WRITE_NON_BLOCKING`，goldfish 仍可能阻塞）。输入满 → 解复用停 → 音频也断粮，整条泵停在约 `write_frames=75776`（48 kHz 约 1.58s）、`au_in=43`。
2. 音频钟若只跟 playback head，head 不动则钟不走。
3. 即便 write 不再阻塞，leftover PCM 若不丢弃会挡住音频输出取回，解码器槽满；视频若 8 个 pending 且 goldfish 长时间无输出，同样把解复用卡住。

### 已落地修复（加法 ABI 仍为 1）

- 非阻塞意图的 `write([BIII)I`，并放到独立写线程，drive 最多等 2ms，避免泵被 write 卡住。
- sink 200ms 不消费则丢弃 leftover PCM 并计入 `write_frames`（测试写配额武装时不丢、钟不兜底）。
- `get_clock_us`：头/钟 200ms 不动则用单调时钟兜底（暂停或配额武装时不用）。
- 视频：已有首帧且 pending≥8 且 2s 无新输出时 flush 并等待关键帧，避免解码器卡死。
- 宿主反例 `test_audio_clock_stall_does_not_deadlock_video`：写配额 0 时视频仍继续提交。
- JNI：`0xFFFF0001` / AI 哨兵不入 ff 队列；worker 补做 player select / `clear_subtitles`。Java `sub_off` 不再 `nativeSelectTrack(0xFFFF0001)`。

### 官方 test

`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`：一次因本地 RTMP 端口拒绝失败（`test_ff_backend.c:3416`，与本次改动无关）；立即重跑 2026-11-23 约 14:18–14:21 Asia/Shanghai EXIT=0。`test_audio_clock_stall_does_not_deadlock_video` ok，`sizeof(ms_event)=176`，abi=1。测试在 `build/debug`。

### APK（14:21 Asia/Shanghai 重链，含上述修复与 libmsass）

- 默认双 ABI：`build/android-hw/MsHwPlayer.apk` 14,427,233 B，SHA-256 `0faa91298d37d99aa00c36ac96ca077dafdc3ebe87b4d039b67b4b8371517385`。
- x86_64 测试：`build/android-hw-x86_64/MsHwPlayer.apk` 8,778,470 B，SHA-256 `441d0e39b5b71462fb07afbc6dcd17c22a517b81fc2dec08d6d82e6feb05cfb7`。
- 冻结产物未动：`libmsmedia.so.1.0.0` `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`libmsff.so.1.0.0`（v7a size）`7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

### 模拟器证据（进行中）

- `stallfix` / `stallfix2` 是写线程落地前/仅写线程、尚未做 leftover 强丢与视频 2s flush 的复验：连播 30s 仍停在 ~76800/43；但 seek 20s 位置+画面已变，`sub_id=5` 完成，无 path 的同一会话 HOME→FG 后 `write_frames` 增长且非黑帧，`sub_off` 无 `0xFFFF0001`。证据：`build/avd-accept-20261123/stallfix2/`。
- `stallfix3` 已装入 14:21 x86_64 APK（Streamed Install）。pid 30885。14:23:55 Asia/Shanghai 已有 first_output，当时 `write_frames=30720 submitted=20`。14:24:47 已到 `write_frames=398336 submitted=179 plat_copy=147 pos_90k=754650`，已越过旧的 75776/43 停滞并仍在增长。正在同一会话上采满 30s 并做 seek / id=5 / HOME→FG / sub_off。解码器是模拟器 goldfish/c2，不是真机硬解。AVD `-no-audio`。



## 2026-11-23 14:35 Asia/Shanghai — 1.58s 停播已在模拟器上证连续增长；收尾项已验

本段为模拟器验收记录，不是 el7，也不是真机硬解或扬声器听感。HDR 仍未实现（设计仍为待评估纳入）。解码器是 CJZB_API34 TCG 上的 goldfish/c2 路径。AVD `-no-audio`，音频只看 `write_frames` / `head_frames` / `clock_us`。

### 连播 30s+（最优先，已证）

同一会话 pid 30885，x86_64 测试 APK（14:21 重链）。证据：`build/avd-accept-20261123/stallfix3/`（`run.log`、`stats/summary-continue.txt`、截图 `shots/`）。

- 14:23:55 Asia/Shanghai first_output：`write_frames=30720` `submitted=20` `plat_copy=20` `pos_90k=65340`。
- 14:24:47：`write_frames=398336` `submitted=179` `plat_copy=147` `pos_90k=754650`（已越过旧停滞点 75776 / au_in=43）。
- 14:26:29–14:28:55 连续 6 点（约 2.5 分钟墙钟，且接在 first_output 之后已超过 30s）：
  - `write_frames` 822272 → 1006592 → 1129472 → 1254400 → 1459200 → 1586176
  - `submitted` 362 → 433 → 484 → 533 → 619 → 674
  - `plat_copy` 294 → 353 → 388 → 429 → 499 → 546
  - `pos_90k` 1557180 → 2978010（约 17.3s → 33.1s），`video_pts_us` 与 `clock_us` / `head_frames` 同步上升
- 脚本判定 `ok30=True`，`same_pid_play=True`。截图 `play-cont-00.png`、`play-cont-03.png`。

### 收尾

- **seek 20s：** `pre_pos=3223710`（约 35.8s）→ `seek_pos=1891980`（约 21.0s，目标 1_800_000），`timeline_epoch` 1→2，`video_pts_us` 35102000→20822000，画面指纹变化（`10-pre-seek.png` / `11-seek20.png`）。
- **字幕 id=5：** 一次 `next_sub` 后 `sub_id=5 sel_epoch=2`，完成态，不是失败停在 busy。日志里的 busy 字样来自更早的 “busy (worker…)”，stats 已是 id=5。截图 `12-sub-id5.png`。
- **同一会话 HOME→回前台（不带 path）：** `am start --activity-single-top` 无 path，系统提示 “Activity not started, its current task has been brought to the front”。pid 始终 30885，`sel_epoch` 保持 2（不是新会话）。`timeline_epoch` 2→3 是 Surface 重绑 resync。`write_frames` 2456576→2860032，`submitted` 1041→1210，fg-early 到 fg-later 仍在涨。`23-fg-later.png` 中心 mean≈117，dark≈0.022，`surface_bound=1`，不是黑帧。
- **sub_off：** `sub_id=0 sel_epoch=3`，`sub-off-logcat.txt` 无 `0xFFFF0001` / `4294901761`，未向 ff 误入队哨兵。截图 `24-sub-off.png`。关字幕后播放仍在走（`write_frames=3161088` `submitted=1333`）。

### 官方 test 与 APK

改源后的官方 `PATH=/usr/bin:/bin ./build.sh test` 于约 14:18–14:21 Asia/Shanghai EXIT=0（中间一次 RTMP 本地端口拒绝与本次改动无关，立即重跑通过）。此后未再改产品源码。两个 APK 已在该官方 test 之后重链（14:21:24 / 14:21:39 Asia/Shanghai），均含 libmsass。SHA-256 见上一段 14:25 过程记录，冻结产物未动。

### 仍未验及原因

- 真机硬解 / 扬声器听感 / 1 秒首帧设备验收；API 16–20；P5 真厂商；el7；Windows/iOS；VAAPI 真提交；HDR。
- 独立位图字幕轨（本次只验了内封轨切到 id=5 与 sub_off）。
- 时钟在 head 重新走动时可能从兜底值回到 head（曾见 `clock_us` 短暂回落）；连播增长已证，但未单独做唇形同步验收（也不应把模拟器写成听感同步）。



## 2026-11-23 14:40 Asia/Shanghai — Android HDR10/HLG 识别与透传（开始）

用户已授权在 Android 主线实现 HDR10 / HLG 的识别与 MediaCodec 透传。本段为过程记录，不是 HDR 显示效果验收，也不是 el7 / 真机硬解。

### 现有架构（动手前）

- `ms_event` / `ms_track_info` 不能加字段（1838 动态测试 `sizeof(ms_event)=176`，ABI 仍为 1）。HDR 信息用加法、带 `struct_size` 的新结构与查询函数。
- `ms_hevc_ps_parse` 只收集 VPS/SPS/PPS 并解析 id；已有 RBSP 去仿真防止字节与 `skip_profile_tier_level`，但未读 VUI/SEI。
- `ms_hw_config`、`ms_hw_player_stats` 已是 size-prefixed，可在末尾追加。
- Android `ms_hw_android_configure` 已设 mime / width / height / csd-0/1，未设 color / hdr-static-info / HEVC profile。
- MP4/MKV 容器色度与 mastering 走 `libmsff` 的 `codecpar` / side data，不在核心里链 FFmpeg。
- DESIGN 原先把 HDR 列为「待评估纳入」；本轮按授权实现识别与透传，**不宣称显示器 HDR 效果已验**。Dolby Vision 只识别并报告不支持，回落基础层。

### 计划

核心 `ms_hdr_*`：HEVC VUI（primaries=9、transfer PQ=16/HLG=18、matrix 9/10、range）与 SEI 137/144；打包 Android 25 字节 `hdr-static-info`（ST2086+CLL 小端，主色 RGB 序）。`ms_ff_get_hdr_info` 合并 codecpar 与 mastering/CLL side data。播放器在配置前合并码流与容器信息，configure（API24+）写入 color 键与静态元数据，Surface 不做色调映射。宿主单测用手工比特流；官方 test 后重链两个 APK；模拟器只证字段被设置且能出画。



## 2026-11-23 14:53 Asia/Shanghai — Android HDR10/HLG：实现已入库，开始官方 test

加法实现已写入工作树，尚未宣称官方 test / APK / 模拟器通过。冻结产物未改。本轮用手工 HEVC 比特流覆盖 VUI/SEI 与 25 字节 hdr-static-info（本机预编译已通过）；未使用 x265 样本生成器。不传 HDR10+ 动态元数据，DV 只识别并回落基础层。AV1/VP9 仅透传 ff 已给的 color，码流未验证。接下来跑官方 `./build.sh test`。



## 2026-11-23 15:31 Asia/Shanghai — Android HDR10/HLG：识别与透传已收口（模拟器非真 HDR 显示）

本段为宿主测试 + CJZB_API34（TCG）模拟器记录，不是 el7，不是真机硬解，也**不是 HDR 显示效果验收**。AVD `Display.getHdrCapabilities` 为 none、maxLuma=500，模拟器不能当作真实 HDR 显示器。

### 实现（加法，作者 Long <mycjzb@gmail.com>）

- 新模块 `src/codec/ms_hdr.c` + `include/msmedia/msmedia.h` 中带 `struct_size` 的 `ms_hdr_info` 与查询/打包 API（`ms_hdr_info_default` / `kind_name` / `merge` / `classify` / `parse_hevc` / `pack_static_info` / Android color 与 HEVC profile/level 映射）。不改 `ms_event` / `ms_track_info`，ABI 仍为 1，LP64 `sizeof(ms_event)=176`。
- 识别来源：HEVC VUI（primaries=9、transfer PQ=16 / HLG=18、matrix 9/10、range）与 SEI 137（mastering，内部 GBR）/ 144（CLL）；`ms_ff_get_hdr_info` 合并 codecpar `color_*`、mastering/CLL side data（ffmpeg RGB→内部 GBR）、DOVI_CONF / DV 标签、DYNAMIC_HDR10_PLUS。MP4/MKV 容器信息走 ff。AV1/VP9 仅透传 ff 已给的 color，**码流解析未验证**。
- 播放器：`ms_hw_config` / `ms_hw_player_stats` 末尾追加 `ms_hdr_info hdr`；`ms_hw_player_set/get_hdr_info`；从 ff、HEVC CSD / 带内 AU 合并；configure 前补 width/height（HEVC 的 `ms_video_cfg_parse` 仍返回 UNSUPPORTED）。
- Android configure（API24+，NDK 路径）：写入 `color-standard` / `color-transfer` / `color-range` 与 25 字节小端 `hdr-static-info`（ST2086+CLL，主色 RGB 序）；HEVC 10bit / HDR10 / HLG 使用 Main10 profile=2（不用 4096 Main10HDR10，goldfish 会拒）；Surface 不做色调映射。**不传** `KEY_HDR10_PLUS_INFO`（检出只标注未验证）。Dolby Vision 不配置，识别后报告不支持并回落基础层。
- goldfish/ranchu：计算并记录全部拟写入键（含 `static_hex`），**不写入 MediaFormat**。原因是这些键没有可靠 c2 映射，写入后即使 `AMediaCodec_configure` 返回 OK，随后也会无输出。真机路径仍写入。
- JNI/Java：打开成功后 `ms_ff_get_hdr_info` + `set_hdr_info`；stats 增加 `hdr=hdr10|hlg|sdr|unknown` 与 `display_hdr=`（`Display.getHdrCapabilities`），并写明不是显示效果声明。
- 宿主 `tests/test_hdr.c`：手工比特流覆盖 Annex B / 长度前缀 / hvcC 的 VUI/SEI 与 25 字节布局。本机 libx265 另生成了 320x240 的 10bit PQ/HLG 小样本（`build/dev-samples/hdr/`）。

### 官方 test 与 APK

最后一次改产品源码后已跑：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`，日志 `/tmp/msmedia-official-test-hdr4.log`，约 15:15 Asia/Shanghai **EXIT=0**。`test_hdr: all passed`，`sizeof(ms_event)=176`，`abi=1`。此前 hdr / hdr2 / hdr3 同样 EXIT=0（中间修过 `log_fn` 参数与 `MS_ERR_STATE`）。本收口之后未再改产品源码，故未重跑官方 test、也未再次重链。

随后已重链（均含 libmsass），冻结产物未动：

- 默认双 ABI：`build/android-hw/MsHwPlayer.apk` 14,480,481 B，mtime 2026-11-23 15:15:33 CST / Asia/Shanghai，SHA-256 `abc9af5eae991a5b690d299a0aba01f9ff779636b583e61ef2a05a586aa36a46`（含 v7a/v8a `libmsass.so`）。
- x86_64 测试：`build/android-hw-x86_64/MsHwPlayer.apk` 8,803,046 B，mtime 2026-11-23 15:15:25 CST / Asia/Shanghai，SHA-256 `d0291fb43e21bb3453cf54aac95c29c066ae619653c657a18e36edfb85d2ac4f`（含 `lib/x86_64/libmsass.so`）。已 Streamed Install 到 CJZB_API34。
- 冻结：`libmsmedia.so.1.0.0` `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；v7a size `libmsff.so.1.0.0` `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

### 模拟器证据

目录：`build/avd-accept-20261123/hdr/`（过程：`fix2/` `fix3/`；收口：`final/` 与 `final/matrix/`）。解码器是模拟器 goldfish / `c2.android` / OMX，**不是真机硬解**。

已证：

- **识别**：HDR10 样本 `hdr=hdr10 prim=9 trc=16 matrix=9 range=1 bit=10 mastering=1 cll=1 src=vui:1,sei:1,container:1`；HLG 干净样本 `hdr=hlg prim=9 trc=18 matrix=9 range=1 bit=10`（vui+container）。stats 另有 `display_hdr=none maxLuma=500.0`。
- **MediaFormat 键被设置**（写入时）：`final/mediaformat-keys-applied.txt` 与早期 `hdr/logcat-hdr10.txt` 的 NdkMediaCodec 格式转储：`color-standard=6`、`color-transfer=6`、`color-range=2`，`hdr-static-info` 25 字节 `01 48 8a 08 39 34 21 aa 9b 96 19 fc 08 13 3d 42 40 e8 03 32 00 e8 03 90 01`（与打包函数一致：type=1，D65，1000 nits，min=50×0.0001，CLL=1000，FALL=400）。该次转储仍带当时的 `profile=4096`（已改为 Main10=2）且因缺宽高/`4096` 曾 EINVAL；修宽高与 profile 后，`c2.android.hevc.decoder` 上日志为 `static=1 profile=2`，`coded.pl.profile` 从 24576(Main) 变为 24577(Main10)，`static_hex` 对干净样本为 `01d084803ec233c4864c1db80b133d4240e8033200e8039001`（Rx=34000,Ry=16000,Gx=13250,Gy=34500,Bx=7500,By=3000,Wx=15635,Wy=16450,max=1000,min=50,CLL=1000,FALL=400，与 x265 mastering 一致）。
- **能出画（8bit HEVC）**：默认 goldfish、无 force_decoder，`hevc8_sdr_320x240.mp4`：`first_output_to_surface=2349.328` `submitted=72` `err=OK`，截图 `final/matrix/hevc8-goldfish.png` 为播放器 Surface（非桌面）。这证明 HDR 改动未破坏 HEVC 出画路径。画面呈中灰而非 lavfi 标注蓝，只说明本 AVD/goldfish 色表示有限，**不得写成 HDR 显示已验**。
- goldfish 对拟写入键的日志策略已生效：`keys computed, not applied on goldfish`。

未能在本 AVD 上得到 10bit/HDR 出画（模拟器解码器限制，不是识别失败）：

- goldfish 将 HEVC 配成 Main 8bit（`coded.pl.profile=24576`），10bit SDR / HDR10 / HLG 均 `first_output=-0.001`。
- `c2.android.hevc.decoder` 能接受 Main10（24577）并写入 color / static-info，但 `C2SoftHevcDec: Fatal error in decoder 0x43dd`，随后 `MediaCodec err 0xe/14`，干净 10bit SDR、干净 HDR10、干净 HLG 同样无输出。
- `OMX.google.hevc.decoder` 播放 10bit SDR 亦无输出（`err=OVERFLOW`）。
- 因此：**不得宣称本模拟器已验证 HDR 显示效果，也不得把 10bit HDR 出画写成已验。** 8bit HEVC 出画 + 日志/转储中的 color 与 25 字节布局是本 AVD 能提供的证据上限。

### 仍未验及原因

- 真机硬解、真机 HDR 显示/色调映射效果、设备 1 秒首帧；本 AVD 的 10bit Main10 出画（goldfish / c2.android / OMX 均失败）。
- HDR10+ 动态元数据透传（仅检出、不传，未验证）；Dolby Vision 配置（明确不支持，只回落基础层）。
- AV1/VP9 码流级 color/VUI 解析（仅 ff color 透传）。
- API 16–20 旧 JNI configure 路径未改。
- P5 真厂商、el7、Windows/iOS、VAAPI 真提交、扬声器听感、唇形同步。



## 2026-11-23 15:33 Asia/Shanghai — 位图字幕 / 长稳 / API16–20（开始）

用户确认 HDR 收口后，按顺序做 Android 主线本机可验剩余项：独立位图字幕轨、同一会话连播 5 分钟以上、API 16–20 旧路径。约束同前（ABI 1、ms_event 176B、视频仅硬解、不 commit/push、不删文件、冻结产物不动）。本段为过程记录，不是 el7 / 真机硬解。

已有 `ms_sub_bitmap` 解析 PGS/DVB/VobSub，`ms_ff` 与 `ms_hw_player` 已喂入这三种；宿主 fixture 与短 MKV 复用测试已覆盖解析。本轮用本机 ffmpeg 工具生成带独立 PGS 位图轨 + 文字/ASS 轨的较长 MKV，在宿主做切轨/关闭，并在 CJZB_API34 上验证叠加显示。`long_multi.mkv` 本身只有 60s，5 分钟连播将用其循环加长副本，不覆盖原文件。时钟兜底回落若在代码或实测中出现，再修为单调并加宿主反例。


## 2026-11-23 15:58 Asia/Shanghai — 剩余项（位图轨 / 时钟单调 / API16-20）推进

- 项 1 模拟器（仍用 15:15 x86_64 APK，CJZB_API34 TCG）：
  - 证据目录 `build/avd-accept-20261123/remain/bitmap/`。
  - `10-pgs-ready.png`：默认 PGS 红条可见，red=0.135，sub_id=3，first_output_to_surface=25183.249 ms（出画墙钟，非媒体时基）。
  - 切轨命令生效：SRT sub_id=4 epoch=2、ASS sub_id=5 epoch=3、sub_off sub_id=0 epoch=4、再切回 PGS sub_id=3 epoch=5 且 `14-pgs-again.png` red=0.135。
  - `11-srt.png` / `12-ass.png` 画面无文字：原样本 `h264_aac_pgs_srt_ass.mkv` 中 SRT/ASS 轨 **0 个数据包**（ffprobe DURATION=0）。已另存 120s 样本 `build/dev-samples/subs/h264_aac_pgs_srt_ass_120.mkv`（未覆盖 19s 文件）：PGS 120 包 / SRT 1 包 0–120s / ASS 1 包 0–120s（居中黄字）。准备重跑互切出画。
- 项 2 时钟：15:15 APK 在本段再次录到回退（switch2 日志）：`clock_us` 8483574→7792166、10432283→8992333；开播 head=-1 时兜底 386233，first_output 后 head 起步跳回 298666。产品侧已加 `clock_emitted_us` 保持单调；去掉临时 `MS_CLOCK_DEBUG`。宿主反例改为调试钩子 `debug_backdate_clock_change` + `debug_force_emitted_clock`（不依赖 mock 墙钟是否走进兜底），随后跑官方 test。
- 项 3：`/home/box/.local/android-sdk/system-images` 目前只有 android-34；sdkmanager 可用。交付 APK minSdkVersion=21，不能装到 API 19。待项 1–2 与官方 test / 重链完成后再决定是否拉 API 21 镜像（且 API 21 AVD 走的是 NDK AMediaCodec，不是 16–20 JNI 旧路径）。
- 冻结产物未动。无 commit/push。下一步：官方 `./build.sh test`，通过后重链两个 APK，用新 APK 做 5 分钟 long_multi_6m 连播。

## 2026-11-23 16:36 Asia/Shanghai — 官方 test / 重链 / 位图互切已过；5 分钟连播进行中；项 3 改为 API 18 兼容

本段接 15:58 剩余项。中断后先完成改源后的官方 test 与两个 APK 重链，再用新包做模拟器项 1 视觉互切；项 2 的 `long_multi_6m.mkv` 5 分钟以上连播于 16:34 CST 开跑（同一会话，未打断）。项 3 按新要求改为：**交付 APK minSdk 降到 18，并加 API 18–20 兼容实现**，不再只做静态说明。冻结产物未动。无 commit/push。不是 el7，不是真机硬解/出声验收。

### 官方 test 与 APK

最后一次产品源改动（`ms_ff.c`：同一容器内可播字幕缓存、HLS/DASH/RTSP/RTP/RTMP 外链未选中字幕不 demux）之后：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`，日志 `/tmp/msmedia-official-test-remain17.log`，约 16:22–16:24 Asia/Shanghai，**EXIT=0**。`test_ff_backend ok`，`test_abi_old_header: sizeof(ms_event)=176`（1838 头 + 当前 .so，canary 完好，abi=1）。未再改产品源码后重链：

- 默认双 ABI：`build/android-hw/MsHwPlayer.apk` 14,480,481 B，mtime 2026-11-23 16:26 Asia/Shanghai，SHA-256 `6ed60bc69a3a6eccc7dbecb871b5f6e1fed7d82b8cdbecb38a0cfc04742bc715`。不得把 15:15 同路径包（`abc9af5eae991a5b690d299a0aba01f9ff779636b583e61ef2a05a586aa36a46`）当作本实现。字节数碰巧相同，哈希不同。`libmshw-legacy.a` 于 16:25 随 android-hw 用 API 21 工具链 `-DMS_HW_LEGACY_JNI` 编过（仍只是编译校验，当时未打进 APK）。
- x86_64 测试：`build/android-hw-x86_64/MsHwPlayer.apk` 8,803,046 B，mtime 2026-11-23 16:26 Asia/Shanghai，SHA-256 `9ac4fbbbe7905f7948dfe54a44bc3f12f59640cf3b21d97802c2bde712f03394`（相对 15:15 的 `d0291fb43e21bb3453cf54aac95c29c066ae619653c657a18e36edfb85d2ac4f` 哈希已变）。已 `adb install -r` 到 CJZB_API34（TCG，`-no-window -no-audio`）。

冻结对照未改：`build/release/libmsmedia.so.1.0.0` 166,168 B，mtime 2026-11-21 11:57 Asia/Shanghai，SHA-256 `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` 4,235,076 B，mtime 2026-11-21 12:17 Asia/Shanghai，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

### 项 1 位图字幕轨（新 APK，CJZB_API34）

样本 `build/dev-samples/subs/h264_aac_pgs_srt_ass_120.mkv`（已在设备 `/data/local/tmp/msmedia-subs/`）：PGS 120 包、SRT「TEXT-SRT」0–120s、ASS 居中黄字「ASS-TRACK」0–120s（PlayRes 320x240，Alignment 5）。证据目录 `build/avd-accept-20261123/remain/bitmap/`（30–34 段；20–24 仍是 15:15 旧包，SRT/ASS 未出字，保留对照）。

- `30-pgs-ready.png`：默认 PGS 红条，crop red=0.210、bot_r=0.735，`sub_id=3` epoch=1，`first_output_to_surface=23428.644` ms（出画墙钟，非媒体时基）。
- `31-srt.png`：切到 `sub_id=4` epoch=2，红条消失，画面底部白字 **TEXT-SRT**（uiautomator 也读到该文本）。
- `32-ass.png`：切到 `sub_id=5` epoch=3，黄字 **ASS-TRACK** 可见（栅格 ImageView，无无障碍文本；位置略偏下，不是切轨失败）。
- `33-off.png`：`sub_off` 后 `sub_id=0` epoch=4，文字/位图均消失。
- `34-pgs-again.png`：再切回 PGS，`bot_r=0.735`，红条恢复。

该会话 `switch-newapk-stats.log` 中 `clock_us` 84 个样本无回退；`submitted` 2→700。宿主 `test_hw_player` 的 PGS/SRT/ASS 缓存切轨仍是逻辑覆盖，本段是模拟器出画。

明确不支持/本轮未在模拟器出画验证的格式：`MS_CODEC_SUB_BITMAP` 未细分族（XSUB、图文电视等）不可直接 `select_track`、不可播；DVB 与 VobSub 在 `ms_sub_bitmap.c` / ff 映射中可解析为 RGBA cue，**本轮模拟器只验了 MKV 内 PGS**，未做 DVB-TS / idx+sub 出画。不是 FFmpeg 软解字幕。

### 项 2（进行中）

同一 x86_64 新包，16:34 CST 起播 `long_multi_6m.mkv`，目标 first_output 后再采 320s。进行中已见 `submitted` / `write_frames` / `head_frames` 连续增长，`t0-ready.png` 彩条秒位「2」、`t180.png` 为「3」（画面在变，非冻帧）；RSS 约 260–266 MiB，未见单边爬升。`clock_us` 单调，但在 AVD `-no-audio` + TCG 慢解码下兜底钟会按墙钟往前走、与媒体 PTS/head 拉大（例如 t≈70s 时 clock≈82s、video_pts≈9s）。这是单调修复在 head 长期落后于已发出兜底钟时的表现，不是回退；完整 5 分钟数字待本会话结束写入。证据目录 `build/avd-accept-20261123/remain/longplay/`。

### 项 3 转向（调研，尚未改 minSdk）

用户要求 Android 4.3（API 18）及以上都要支持。已查：NDK r29 `meta/platforms.json` 为 **min=21, max=35**，sysroot 与 clang 包装只有 `android21` 起，没有 `*-linux-android18-clang`。r26 起官方最低目标就是 21。本机只有 r29，sdkmanager 可下旧 NDK（如 23.2 / 21.4），但官方入口仍是 `./build.sh` + 现用 r29，不宜把交付工具链切到旧 NDK。

可行并准备落地的做法（**仍用 r29、以 API 21 为编译目标，运行时支持 18**）：

- 清单 `minSdkVersion=18`（安装允许 API 18+）。编译/链接仍 `--target=*-android21`。
- API 21+ 保持现有 NDK AMediaCodec 路径，不得回退；但当前 `libmshw.so` **NEEDED `libmediandk.so`**，API 18–20 没有该库，loadLibrary 会失败。改为对 `libmediandk.so` 做 `dlopen`/`dlsym`，API 21+ 再解析符号，18–20 走已有 JNI MediaCodec（把现在仅编译校验的 `MS_HW_LEGACY_JNI` 编进主 so，按 `ro.build.version.sdk` 运行时选择；不用 API 24 的 `android_get_device_api_level` 做门槛）。
- 缺失接口回落并在注释写清边界：AudioTrack `write([BIII)I` / `WRITE_NON_BLOCKING` 为 21+，18 用 `write([BII)I`；`getTimestamp` 为 19+，18 只用 `playbackHeadPosition`；`pthread_condattr_setclock` 为 21+，18 回落默认时钟；MediaFormat color / `hdr-static-info` 仍仅 24+；HDR 不在 18 上声称。
- 依赖库加载：现包 `libmsff.so` / `libmsass.so` / `libmsmedia.so` 不 NEEDED mediandk；`getentropy` 已是 WEAK UND（OpenSSL，API 28 才有强符号，弱引用可在 18 上加载）；`getauxval` 为 API 18 起可用。落地时再对 v7a/v8a 做符号核对，避免把 21+ libc 符号做成硬依赖。
- AVD：sdkmanager 有 `system-images;android-18;default;x86`（无 x86_64）。API 18 出画验证需要 **i686 测试 APK**，现有 x86_64 包不能装进该镜像。拉镜像/建 AVD 时不得动 CJZB_API34。若镜像或 x86 AVD 在本环境不可用，将如实说明。

下一步：等项 2 连播收口并写数字，再改源落地 minSdk 18 + 运行时路径选择；每次改源跑官方 test，最后重链两个 APK，并用 CJZB_API34 回归。

## 2026-11-23 16:41 Asia/Shanghai — 项 2：同一会话 5 分钟以上连播（CJZB_API34）

用 16:26 x86_64 新包，同一进程 pid=23276，播 `long_multi_6m.mkv`（设备 `/data/local/tmp/msmedia-subs/long_multi_6m.mkv`）。`first_output_to_surface=17946.751` ms（出画墙钟）后继续采集到 389.7 s（>5 分钟）。证据 `build/avd-accept-20261123/remain/longplay/`（`samples.csv`、`summary.json`、`longplay-stats.log`、`meminfo-end.txt`、截图 t0/t60/t180/t300/t320/t-end）。本段不是 el7，不是真机硬解/出声，AVD 为 TCG + `-no-audio`。

- 增长：`submitted` 70→947，`au_in` 77→1168，`write_frames` 142336→2236416，`head_frames` 126216→2096392，`video_pts_us` 2.923s→46.523s，全程 `err=OK`、`user_paused=0`、`backpressure=0`。`drop_nokey` 7→221（TCG 慢解、落后时丢非关键帧，不是停播）。
- 未停播：13 个采样点 `submitted`/`write_frames`/`head` 均单调上升；画面 t0 彩条「2」、t180「3」、t300「50」、t-end 仍有后续绿场+ASS「ASS-ENG color」。
- 时钟：`clock_us` 33,275,774→344,649,138，**0 次回退**（15:15 包上的兜底回落到 head 在本包未再现）。但兜底钟按墙钟走，与媒体 PTS/head 差值增大（av_sync_delta_us 约 -30s 到 -298s）：AVD `-no-audio` 下 head 慢于墙钟，已发出的兜底钟保持单调、200ms 头停后又按墙钟继续。这不是回退；未把「追上 head」改成回退。`pos=` 行本轮脚本未解析到（不影响 submitted/clock 结论）。
- 内存：同 pid；RSS 260,572–266,080 KiB（末次 dumpsys 有一次空窗，`meminfo-end.txt` TOTAL RSS=266,976 KiB，PSS=150,249，Native Heap PSS=123,392）。无单边爬升，不作泄漏判定。
- 宿主时钟反例仍在 `test_hw_audio_trim`（`debug_backdate_clock_change` / `debug_force_emitted_clock`），已包含在 16:24 官方 test。

项 1–2 在本机模拟器上已收口。接下来落地项 3：minSdk 18 + 运行时 JNI/Ndk 分流（r29 仍以 API 21 编译）。


## 2026-11-23 17:14 Asia/Shanghai — 项 3：minSdk 18 落地、JNI_OnLoad 修复、libc 弱引用、CJZB_API34 回归、API 18 AVD

本段接项 1–2 收口与「仍用 r29 API 21 编译、运行时支持 18」的方案。官方入口仍是 `./build.sh`。未提交、未推送，未改冻结产物。本段不是 el7，不是真机硬解/出声。

### 做法（已落地）

- 编译目标保持 NDK r29 API 21（`meta/platforms.json` min=21，无 `*-linux-android18-clang`）。清单 `minSdkVersion=18`，`d8 --min-api 18`，apksigner `--min-sdk-version 18`。
- `libmshw.so` 不再 NEEDED `libmediandk.so`；`ms_hw_mediandk.c` 对 `libmediandk.so` 做 dlopen/dlsym。`ms_hw_use_ndk_mediacodec()`：SDK_INT≥21 且 mediandk 可用才走 NDK 路径，否则 JNI MediaCodec。API 21+ 成功路径不回退。
- JNI I/O 编进主 so。AudioTrack 18 用 `write([BII)I`；不用 API 19 `getTimestamp`；HDR/color 键仍仅 24+。
- 首次 minSdk 18 包在 CJZB_API34 的 `JNI_OnLoad` 因错误查找 `queueInputBuffer(IIJI)V` 留下 pending `NoSuchMethodError` 而 SIGABRT。已删错误签名，只保留 `(IIIJI)V`，并对已废弃的 `getInputBuffers`/`getOutputBuffers` 做 ExceptionClear，不再作为 OnLoad 硬条件。
- 对照 NDK 21.4 的 API 18 sysroot：v7a `libmshw.so` 曾强 UND `pthread_condattr_setclock`、`pthread_mutex_timedlock`（均为 API 21）；`libmsff.so` 曾强 UND `atof@LIBC`、`signal@LIBC`（API 18 只有 strtod / sigaction）。已处理：
  - `src/hw/ms_hw_bionic18.h`：上述 pthread 符号弱引用；API 18 上 setclock 跳过（cond 默认 CLOCK_REALTIME），timedlock 用 trylock+nanosleep 轮询。API 21+ 解析到 libc 实符号。
  - `src/compat/ms_api18_libc.c` 链进 libmsff（不改编、不覆盖冻结 size-opt 前缀）：`atof`→`strtod`，`signal`→`sigaction`（sigset_t 清零，不用 API 21 才有的 sigemptyset/sigaddset，也不用 r29 64 位 sysroot 没有的 bsd_signal）。`getentropy` 保持弱 UND；`getauxval` 为 API 18 强符号，可加载。`libmsass.so` / `libmsmedia.so` 对照 API 18 libc 无额外硬缺。
- 最终 v7a 交付 so：`libmshw` 对 setclock/timedlock 为弱 UND；`libmsff` 对 atof/signal 无 UND，仅 `getentropy` 为弱；无 NEEDED libmediandk。

### 官方 test 与 APK

- `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（`/tmp/msmedia-official-test-api18d.log`）**EXIT=0**，`sizeof(ms_event)=176`，`abi=1`，1838 旧头动态测试通过。该次覆盖 libmshw 弱引用相关源改；`ms_api18_libc.c` 只进 Android 重链，宿主不编。
- 最终包（Asia/Shanghai）：
  - 交付 `build/android-hw/MsHwPlayer.apk` 14,521,441 B，mtime 2026-11-23 17:06:01，SHA-256 `5759c4a62941b80aa6e9ed21e24de1e6f69a7fd9fbd04fea46eab05ebae9a18d`，`sdkVersion:18`，含 v7a+v8a。
  - 测试 `build/android-hw-x86_64/MsHwPlayer.apk` 8,819,430 B，mtime 2026-11-23 17:05:43，SHA-256 `98ac90093fd9283476be4281633468d27221fe1aa0145eb45234e16068888531`。
- 冻结未动：`build/release/libmsmedia.so.1.0.0` SHA-256 `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`；`build/android-v7a/size/libmsff.so.1.0.0` SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

### CJZB_API34 回归（API 21+ 路径不得回退）

证据 `build/avd-accept-20261123/remain/api18-fix-api34/`。装入最终 x86_64 包（设备 libmshw SHA 与包内一致 `65c975703c…`）。`backend=amediacodec`，`err=OK`，`first_output_to_surface=18961.613` ms，`submitted` 持续增长（27→409）。默认 PGS 出画红条 `red=0.210` / `bot_r=0.735`（`50-pgs-ready.png`）；`sub_off` 后红条消失；再 `next_sub` 回到 sub_id=3，红条数值与首次相同（`54-pgs-again.png`）。轨切换 log：sub_id 3→4→5→0→3。本次最终包截图未抓到可见 SRT/ASS 像素（全帧黄/白字幕比为 0；与 UI 铬白 0.126 相同）。OnLoad 修复后、libc shim 前的包（SHA `2c63bd73…`）在同目录 `41-srt.png`/`42-ass.png` 曾抓到 SRT 底白、ASS 底黄；字幕缓存策略未随 shim 改。JNI_OnLoad 崩溃未再现。

### API 18 AVD

- sdkmanager 包 `platforms;android-18` 与 `system-images;android-18;default;x86` 已在 `/home/box/.local/android-sdk/system-images/android-18/default/x86`（32 位 x86，无 x86_64）。
- 另建 AVD **CJZB_API18**（Galaxy Nexus，512 MB），未改 CJZB_API34。
- 模拟器 37.1.11 对 32 位 x86：默认/KVM 不可用时会报 `x86 emulation currently requires hardware acceleration`；`-accel tcg` 非法；**必须 `-accel off`**（与 CJZB_API34 相同）。命令：`emulator -avd CJZB_API18 -port 5556 -no-window -no-audio -gpu swiftshader_indirect -memory 512 -no-snapshot-load -no-snapshot-save -no-boot-anim -accel off`。约 45 s 启动完成：`ro.build.version.sdk=18`，`ro.build.version.release=4.3.1`，`ro.product.cpu.abi=x86`，无 houdini。CJZB_API34（emulator-5554）一直在线。
- 出画验证**未做成**：现有测试 APK 只有 `lib/x86_64`，交付 APK 只有 v7a+v8a，都没有 i686。API 18 的旧 `pm` 仍会把包装上（不报 NO_MATCHING_ABIS），但不会抽出 native 库。启动即 `UnsatisfiedLinkError: Couldn't load mshw ... findLibrary returned null`。证据 `build/avd-accept-20261123/remain/api18-avd/`（`install-abi.log`、`x64-install-launch.log`、`api18-after-x64-install.png` 为 4.3 主屏 + “Unfortunately, MsHwPlayer has stopped.”）。
- 要做 API 18 出画，需要另做 **i686 测试 APK**：用 r29 的 `i686-linux-android21-clang` 建 `build/android-x86/` size-opt 前缀（OpenSSL/FFmpeg/libxml2/libass，不得覆盖冻结产物），再仿 `android_hw_x86_64_build.sh` 重链并链入 `ms_api18_libc.c`。本树目前没有 i686 前缀或脚本，本轮未开编。
- Java：`registerReceiver` / AudioFocus / HDR caps 已按 SDK_INT 分支。`TextView.setElevation`（API 21）在 `applyCuesFromNative` 未守卫，有 i686 包并出字幕后还需补判断。Dalvik 对本包这些 API 21+ 方法有 VFY 警告，当前崩溃原因是缺 native 库。

### 仍未验

- API 18/19 真机，以及 API 18 AVD 上的启动出画（缺 i686 测试 APK）。
- 最终包在 CJZB_API34 上 SRT/ASS 的像素可见性（轨切换与 PGS 开/关/开已见）。
- API 18 上 Java `setElevation` 与完整字幕视图。
- el7、真机硬解/出声；AVD `-no-audio` 墙钟兜底快于媒体 PTS（项 2 已记录，不是回退）。

## 2026-11-23 17:19 Asia/Shanghai — API 18 续：Java NewApi 守卫与 i686 前缀

- lint（cmdline-tools 8.9.0）`--check NewApi,InlinedApi`，compileSdk 34，minSdk 来自清单 18，源码 + 对 android-34 编译的 classpath。修复前唯一 NewApi：`View.setElevation`（API 21，`MsHwPlayerActivity.java:1092`）。`registerReceiver(..., RECEIVER_EXPORTED)`、AudioFocusRequest、`getHdrCapabilities` 已有 SDK_INT 分支，lint 未再报。资源只有 `strings.xml`，无 v21+ 属性。
- 已守卫：`if (Build.VERSION.SDK_INT >= 21) tv.setElevation(...)`；API 18–20 仍按 addView 顺序叠字幕。修复后再跑同一 lint，issues 为空。报告：`build/avd-accept-20261123/remain/api18-java/lint-newapi-before.xml` 与 `lint-newapi.xml`。
- 官方 test 已在该 Java 改动后启动（`/tmp/msmedia-official-test-api18java.log`），宿主不编 Java，仍按「每次改源跑 test」执行。
- 新增 `scripts/android_x86_size.sh`、`scripts/android_hw_x86_build.sh`，`./build.sh android-hw-x86` 入口；`ass_deps_android_build.sh` 增加 `x86`（i686-linux-android21-clang）。不覆盖 v7a/v8a/x86_64 前缀与冻结产物。
- i686 size-opt 前缀已就绪（`build/android-x86/`）：OpenSSL 3.5.9 / FFmpeg 9.1（CONFIG_DECODERS=0, SMALL=1, GPL=0）/ libxml2 2.13.8；抽样 `.o` 为 ELF 32-bit i386。正在编测试 APK。

## 2026-11-23 18:13 Asia/Shanghai — API 18 JNI 出画修补 + 用户点播（t116u）未完播

### API 18 JNI 路径（出画仍待用新 i686 包复测）

- 根因（配置成功、submitted 停在二十多、first_output=-0.001）：JNI MediaCodec 路径没有走 NDK 已有的两处转换。`split` 给出的 H.264 CSD 是裸 NAL；AU 在 AVCC 长度前缀下原样 queueInputBuffer。`OMX.google.h264.decoder` 要的是起始码 Annex B。另：`dequeueOutputBuffer` 曾用 timeout=0，与 NDK 注释里 goldfish/软解空转不输出的问题同类。
- 修复：`ms_hw_nal_with_start_code` / `ms_hw_avcc_au_to_annexb` 放到 `src/hw/ms_hw_csd.c`（`ms_hw_internal.h` 声明）。NDK 路径改为调用同一实现；JNI `ms_hw_jni_codec_open` 把带起始码的 CSD 放在 codec 私有缓冲（`MediaFormat.setByteBuffer` 可能只持引用），feed 时按 `nal_len_size` 转 Annex B；drive 与 NDK 一样 10ms 超时并在 FORMAT/BUFFERS_CHANGED 后同一次再取。JNI configure 补 `d->sample_fmt`。`start()` 对 `getInputBuffers`/`getOutputBuffers` 清异常。
- 官方 test：改源后 `./build.sh test` 退出 0，`test_abi_old_header` ok（1838，`sizeof(ms_event)=176`，`abi=1`）。日志：`/tmp/msmedia-official-test-annexb.log`。
- 测试 APK：`./build.sh android-hw-x86`（不入默认交付）`build/android-hw-x86/MsHwPlayer.apk` 7,594,711 B，SHA-256 `da3a32b9406657e84f9a3ffd70a627e72b1a96af7e17f886d5d3a4326921eedb`（Annex B 修补后、FFmpeg 日志钩子前）。此前已有 jobject Surface 全局引用、`force_decoder` 走 JNI configure、libc shim、`setElevation` API 21 守卫。
- CJZB_API18 上 Annex B 修补后的复测被 t116u 插队打断，尚未用上述 i686 包再跑出画/seek/字幕。更早一次（修补前）已能 `backend=jni-mediacodec err=OK submitted=28 write_frames=8192`，但无 first_output。

### 用户点播 t116u（CJZB_API34，未出画）

证据目录：`build/avd-accept-20261123/user-vod/`（日志与截图已脱敏，目标写为 `1252033264.vod2.myqcloud.com/.../hJijavSjX0EA.mp4`；UI 地址栏已涂黑）。
- 包：先用当时 x86_64 测试包 SHA-256 `98ac90093fd9283476be4281633468d27221fe1aa0145eb45234e16068888531`；为拿到 FFmpeg 失败原因后重链（见下）SHA-256 `7776d9b4126a52845feff37ba82330b9d787ebb7201c901db72b921792b6855d`，8,827,622 B。模拟器 **TCG**（`-accel off`），`gpu=swiftshader_indirect`，`-no-window -no-audio`。
- 宿主探测（00-probe-moov.txt）：HTTPS Range 206，总长 1,699,383,887；盒布局 ftyp + free + mdat(1,694,396,415 @40) + **moov 在尾部** offset 1,694,396,455 大小 4,987,432。宿主 `ffprobe` 成功：H.264 Constrained Baseline 1280x688，时长 7469.625 s（2:04:29.625）。
- 首次打开：`ms_ff_open failed: UNKNOWN`，`dur_open_input≈2.1s`，`find_info` 未跑。非崩溃（log 里 “has stopped”/YouTube SIGILL 曾误报）。
- 环境：AVD 把该域名解析成 **198.18.0.1**（假 IP）。本机 libc / 连 UDP 8.8.8.8 也返回 198.18.0.1；宿主进程经 172.30.0.1 能访问，**AVD slirp 到 198.18.0.1 是 Destination Host Unreachable**。一般公网 TCP 从 AVD 可达（8.8.8.8:53、1.1.1.1:443）。DoH 得到真实 A（218.61.116.92 等），AVD 对那些地址 443 连通。`/system/etc/hosts` bind 对 netd 无效。采用 iptables OUTPUT DNAT `198.18.0.1:443 -> 218.61.116.92:443`（计数有命中）。见 04-dns-workaround.txt。
- 诊断补强（不给 libmsff 加 liblog）：`ms_ff_open` 失败带 `av_strerror`；`ms_ff_set_log` 接到 JNI `MsFf`；`apply_tls_trust_env` 增设 `SSL_CERT_FILE`（TLS 校验未关）。官方 test 退出 0（`/tmp/msmedia-official-test-fflog3.log`）。随之重链 x86_64 测试包，未覆盖冻结前缀。
- 新包 + DNAT 后仍失败：`ms_ff_open failed: UNKNOWN (avformat_open_input failed: I/O error (-5))`，`dur_open_input≈5.57s`，find_info 无。系统 CA 导出 PEM（135 张）在宿主上能校验 CDN 证书（CN=`*.vod.myqcloud.com`，DigiCert，TLS 1.2）。另试 AVD→宿主 10.0.2.2:9443→198.18.0.1:443 裸转发，得到 `SSL routines::wrong version number`（与宿主 ffprobe 不是同一条栈），已弃。
- 因此：**首帧/60s 连播/seek 30 分与 1:50/暂停恢复/后台回前台均未做到**。不是 moov 在尾本身在宿主上不可解。未关 TLS 校验。默认双 ABI 交付包尚未因本次 ms_ff/JNI 改动做最终重链。

### 仍未验

- Annex B 修补后的 CJZB_API18 出画、seek、字幕、后台回前台；API 19 最小启动。
- 最终交付包在 CJZB_API34 上的 SRT/ASS 可见像素。
- 用户点播的首帧与后续操作（受本箱 AVD DNS/假 IP 与打开 I/O 错误 -5 阻挡）。
- 封装 A / 外挂字幕 B / 歌词 C / 演示手势与倍速 D（按要求排在 API 18 收口与本点播之后，尚未开始）。
- el7、真机硬解/出声。


## 2026-11-23 18:20 Asia/Shanghai — 点播 EIO(-5) 定性：AVD 网络，非产品无法打开尾 moov

约束：全程未关 TLS 校验；日志/本段不写完整公网 URL（域名 + 文件名尾：1252033264.vod2.myqcloud.com/.../hJijavSjX0EA.mp4）。

### 宿主对照（直连公网，同一 debug libmsff）

- 工具：`/tmp/vod-diag/ms_ff_open_vod`，链接 `build/debug/libmsff` + `libmsmedia`。
- 设置与播放器一致：`MS_FF_PROBE_FAST_START`、`io_timeout_ms=15000/20000`、`--ca /etc/ssl/certs/ca-certificates.crt`、校验开启。
- 结果：`open=OK`，wall ≈ 6.8–7.2 s（open_input ≈ 6.3–6.7 s，find_info ≈ 0.51 s），`seekable=1`，`dur_90k=672267510`（约 2:04:29）。
- seek 30 分（162000000 @90k）：`st=OK`（索引已在 moov，调用本身 wall≈0）。
- 伴随警告：`Unknown error`、`stream 1, offset 0x30: partial file`、FAST probesize 下 codec 参数不全（`probe_incomplete=1`），但不挡打开。
- **结论：产品在通畅网络上可以用现有 15s 超时打开这份 moov 在尾的 HTTPS MP4。**

### AVD 内 TLS + Range（直连公网，失败）

- 探测程序：`/tmp/vod-diag/and_https_range`（NDK r29 x86_64 + 已有 `android-x86_64/openssl-3.5.9-size` 静态库，校验 PEER）。
- TCP 到 `218.61.116.92:443` 与 `198.18.0.1:443`（OUTPUT DNAT 仍指向前者）均 `tcp_ok`。
- TLS 一律 `error:0A00010B:SSL routines::wrong version number`（与此前把 AVD 443 接到宿主 198.18 的 relay 相同）。
- 宿主对同一真实 IP + SNI 的 openssl/ffprobe 为 TLS 1.2 且校验通过。
- **结论：CJZB_API34（TCG）到该 CDN 的路径不能建立有效 TLS，属 AVD/slirp 网络环境，不是 Range/尾 moov 产品缺陷本身。**

### 本机 HTTPS Range 反向代理（接下来的播放路径）

- 脚本：`/tmp/vod-diag/https_range_proxy.py`，监听 `127.0.0.1:8443`，叶证书为已有自签方案 `build/avd-accept-20261123/certs/server.crt`（SAN IP 127.0.0.1 / DNS localhost），对源站走系统 CA 且校验开启。
- 宿主 curl Range `bytes=0-15` 与 moov 头 `bytes=1694396455-1694396486` 均为 HTTP 206，总长 1699383887。
- 宿主同一 libmsff 经代理打开 `https://127.0.0.1:8443/vod.mp4` + 测试 CA：`open=OK` wall ≈ 7.465 s。
- `adb -s emulator-5554 reverse tcp:8443 tcp:8443` 已建立。测试 CA 已追加到 `/data/data/com.msmedia/cache/ms_system_cas.pem`（136 张，未关校验）。
- 证据：`build/avd-accept-20261123/user-vod/10-host-vs-avd-diag.txt`。
- 下一步：在 CJZB_API34 上播该代理 URL，测首帧、60s、seek 30 分/1:50、暂停恢复、前后台。该段为**经本机代理**，不是 AVD 直连公网。



## 2026-11-23 18:36 Asia/Shanghai — 经本机代理完成点播验证（AVD 直连公网不可用）

路径标签：

- **直连公网**：仅宿主对照（同一 libmsff，15s FAST_START，校验开）打开成功；AVD 对真实 CDN IP 的 TLS 为 wrong version number，未在 AVD 上直连播成功。
- **经本机代理**：`adb reverse tcp:8443` + 宿主 HTTPS Range 反向代理（源站走系统 CA，校验开；对 AVD 用已有自签 CA，SAN 127.0.0.1）。播放 URL 为 `https://127.0.0.1:8443/vod.mp4`。未关 TLS 校验。APK SHA-256 `7776d9b4126a52845feff37ba82330b9d787ebb7201c901db72b921792b6855d`（x86_64）。AVD：CJZB_API34 TCG。

结果（证据 `build/avd-accept-20261123/user-vod/`，PNG 已遮 URL 栏）：

- 打开成功：`dur_open_input=18129ms`，`dur_find_info=1266ms`，`seekable=1`，`dur_90k=672267510`，`backend=amediacodec` `hw=1`，AAC 系统软解。
- 首帧：`first_output_to_surface=71522.913`（约 71.5s，TCG 很慢）。`13-first-output-proxy.png` 画面区有丰富色彩，不是黑屏。
- 60s 连播：首帧后画面仍在；媒体时间停在约 15.7–15.8s，`submitted` 几乎不动（377→379）。AudioTrack `head_frames` 因 AVD `-no-audio` 冻结，音频钟走 stall 兜底。**不是从 0 起 60s 媒体时钟单调增长。**
- seek 30 分：`pos_90k=162000000` 准确；截图与首帧中区差约 7，且当时 `first_output_to_surface=-0.001`，**30:00 画面变化未确认**。
- seek 1:50（1 小时 50 分）：`pos_90k=594000000`；`16-seek1h50.png` 与片头中区差约 117，画面确认。
- 暂停：`paused=1`，画面与 1:50 冻结相同；恢复后 `video_pts_us≈6595.89s`（约 1:49:55），画面相对暂停有变化。
- 前后台：HOME 自动暂停、位置保持。回到前台后立即截到黑屏（surface 重建偏早），稍后 `18-foreground-later.png` 恢复为 1:50 附近画面，`surface_bound=1` `epoch=5` `first_output_to_surface=71224.609`。

分类结论：原先 EIO(-5) 是 AVD 到该 CDN 的 TLS/网络环境，不是产品读不了尾 moov。经本机代理后产品能打开并在 1:50 出画、暂停恢复、前后台恢复。未改 Range/超时代码。60s 媒体时间增长弱、30 分画面未确认、TCG 首帧很慢，记入未完全项，不据此改时钟（`-no-audio` 环境）。下一步：API 18 Annex B 复测（i686 测试 APK 需按当前源重链，因 17:43 包早于 18:02 的 ff 日志钩子）。



## 2026-11-23 18:45 Asia/Shanghai — CJZB_API18 出画复测（Annex B 后的 i686 包）

测试 APK：`./build.sh android-hw-x86`（不覆盖默认交付、不动冻结前缀）
`build/android-hw-x86/MsHwPlayer.apk` 7,598,807 B，SHA-256
`cca9328459df0ebc9ddaf51822404af710b0d7c173da3ae929db8df85667b7ca`
（含 Annex B 与 FFmpeg 日志钩子）。AVD：CJZB_API18，sdk=18，abi=x86，512 MB，TCG / `-accel off`，emulator-5556。
本 AVD 只有 `OMX.google.*`，默认 `allow_software=0`，复测用测试 extra `force_decoder=OMX.google.h264.decoder`（模拟器软解，**不得写成真机硬解**）。证据：`build/avd-accept-20261123/remain/api18-annexb/`。

### 出画

- `p4_seek.mp4`（H.264 **High** 160x120）：`backend=jni-mediacodec` `configured=1`，但 `err=UNKNOWN`、`submitted=1`、无 first_output。判断为 API 18 `OMX.google.h264.decoder` 基本不接 High，不是 Annex B 回退。
- `short.ts`（Constrained Baseline 160x120）：**出画成功**。`first_output_to_surface=205.096` ms，`submitted=23`，`err=OK`，`hw=0`。
- 30s 自建夹具 `media/base30_subs.mkv`（Constrained Baseline 320x240 + SRT/ASS，未覆盖冻结样本）：`first_output_to_surface=367.409` ms，`submitted` 增长（45→225），`err=OK`，`write_frames` 增长（37888→185344），AAC 为系统软解。

### 截图限制

本 API 18 AVD 的 `screencap -p` **抓不到 MediaCodec Surface 画面**（中区近黑，多次截图像素几乎相同，仅文件头/顶栏有极小差异）。出画证据是 `first_output_to_surface` + `submitted`/`write_frames` 增长，不是可见像素。字幕像素同样无法从截图确认。

### seek / 字幕 / 前后台

- `DEBUG_CMD` 在 API 18 可用（`--el` 被接受，`debug_cmd=seek`，返回 `seek applied (waiting keyframe/CSD)`，`timeline_epoch` 增加，flush 后会再次 first_output）。
- 位置不稳：对 10s/20s 的 seek，pos 出现过 173430（约 1.9s）、1021860（约 11.3s）、末态 183870（约 2.0s）卡住；**不能声称 10s/20s 落点准确**。
- `next_sub`：`sub_id` 3→4→0，轨切换命令有效；无截图像素证明 SRT/ASS 叠字。
- HOME 后 stats 仍 `paused=0`（与 API 34 自动暂停不同，待查）；回前台 `surface_bound=1`，`first_output_to_surface=130.592`，会话未崩。

### API 19

当前盒子只有 AVD CJZB_API18 / CJZB_API34 / CJZB_ARM64。SDK `system-images` 仅有 android-18 与 android-34，**没有 android-19 镜像**，无法做最小启动。未发起镜像下载。

下一步：A 封装、B 外挂字幕、C 歌词、D 演示功能，最后重链全部 APK 与官方 test、API34/API18 回归。



## 2026-11-23 18:48 Asia/Shanghai — API 19 缺镜像；开始 A 封装 demuxer（新前缀）

- API 19：SDK `system-images` 只有 android-18 与 android-34，无 android-19，也无对应 AVD。未下载镜像。最小启动未做。
- A 封装：现有 `--enable-demuxer` 仅有 ts/flv/mov/hls/dash/rtsp/rtp/sdp/aac/h264/ogg/matroska/webvtt。已在宿主 `scripts/ffmpeg9_build.sh` 与四条 Android size 脚本（v7a/v8a/x86_64/x86）中**同样**追加：`hevc,avi,mpegps,mp3,flac,wav,asf,rm,ac3,eac3,srt,ass,microdvd,vobsub`。`CONFIG_DECODERS` 仍为 0，不加软解。
- `ms_ff.c` `format_whitelist` 已同步追加（含 mpeg/webm 等别名保持原状）。
- **不覆盖**已有 `build/ffmpeg-9.1`、`build/ffmpeg-9.1-size`、`build/android-*/size` 冻结/原前缀。宿主新前缀将建在 `build/ffmpeg-9.1-size-morefmt`。Android 新前缀计划 `build/android-*/size-morefmt`（尚未开编）。
- B/C/D：代码里已有 `ms_ff_set_external_subtitle` / `ms_hw_player_set_rate`，演示 App 尚未接倍速/手势/比例/截图/断点，也未见独立 LRC 解析。待新前缀与官方 test 后再补。



## 2026-11-23 18:49 Asia/Shanghai — 宿主 morefmt FFmpeg 前缀建成

- 安装宿主 `nasm` 2.16.03（与既有 size 前缀一样走 x86 asm；未加 `--disable-x86asm`）。
- `MS_SIZE_OPT=1 FFMPEG_BUILD_ROOT=build/ffmpeg-9.1-size-morefmt OPENSSL_PREFIX=build/openssl-3.5.9-size/prefix FFMPEG_REBUILD=1` 调用 `scripts/ffmpeg9_build.sh`，退出 0。
- 未改、未覆盖 `build/ffmpeg-9.1`、`build/ffmpeg-9.1-size`。
- `CONFIG_DECODERS=0`，`CONFIG_SMALL=1`，`CONFIG_GPL=0`，许可 LGPL 2.1+。configure 汇总 Enabled decoders 为空。
- 新 demuxer 均为 1：avi, mpegps, hevc, mp3, flac, wav, asf, rm, ac3, eac3, srt, ass, microdvd, vobsub（以及原有 ts/flv/mov/…）。
- 静态库体积（字节）：

  | | 原 size（未覆盖） | morefmt |
  |---|---|---|
  | libavformat.a | 12,087,570 | 14,941,206 |
  | libavcodec.a | 7,389,958 | 12,919,834 |
  | libavutil.a | 4,421,490 | 4,423,298 |

  avcodec 变大主要是多了 flac/ac3 等 parser（configure 里 Enabled parsers 增 flac、ac3），仍无解码器。后续 relink libmsff 需记录 so 体积。
- 尚未：用该前缀重链宿主 libmsff 并跑 `./build.sh test`；尚未编 Android `size-morefmt` 前缀（须改 OUT 到新目录，避免写进 `build/android-*/size` 与 `ffmpeg-9.1-size`）。



## 2026-11-23 19:09 Asia/Shanghai — 点播 15.7s 停播：不是代理/-no-audio，是每 AU 探测封装时 SEEK_SET 0

### 定性

- 宿主直连公网点播（同一 libmsff，不经本地代理、无 AudioTrack）用 `/tmp/vod-diag/ms_ff_open_vod --probe fast --pump-media-s 60 --pump-wall-s 90`：**同样停在 ~15.7s**。FAST_START 与 DEFAULT 探测都是 `after_n=1059 last_v_90k=1411690`（约 15.685s），之后每包间隔 5–9s，墙钟 90s 仍只有 ~15.9s，`vid≈381–382`、`bytes≈3.10MB`。与 AVD 经代理 60s 墙钟 `submitted` 377→379 同一卡点。
- 不是输入库存耗尽、不是泵/时钟死锁、不是 AVD `-no-audio`：宿主泵不经 AudioTrack，时钟/写线程不在路径上；`backpressure=0`、`drop_nokey=0`。系统 `ffprobe -read_intervals 14%18` 约 8s 读过 18s，样本 pos 连续（约 3.0MB→3.67MB），CDN 与文件本身正常。
- 早先怀疑「8 字节 PNG/JPEG 探测先 `SEEK_SET 0` 把 HTTP 打成按次 Range」：已改成先 `SEEK_CUR` 短回绕，并为 http(s) 加 `multiple_requests` / `reconnect*`（不关 TLS 校验）。**卡点不变**，前 15.7s / 约 3MB 仍然很快，故不是「一打开就按包重连」。

### 根因

`produce_packet` 每个 AU 都调用 `map_format(ic)`。MP4/MOV 路径里 `is_fragmented_mp4` 与 `ftyp_is_qt` 会 `avio_seek(..., 0, SEEK_SET)`、读 4KB/32B、再 seek 回当前偏移。等于每个 AU 对 HTTP 点播做两次回到文件头的 Range 重连。前约 1000 次请求还能跟上，约 15.7s / 3MB 后每包 5–7s（CDN/连接复用被打爆）。系统 ffprobe 不在每包探测封装，所以能越过该点。

### 修复

- `ms_ff` 增加 `format_resolved` / `format_cached`：`map_format` 只在打开后探测一次（`is_fragmented_mp4` / `ftyp_is_qt` 的 I/O 只发生一次），`close_input` 清缓存。
- 保留先 `SEEK_CUR` 回绕与 http(s) 持久连接选项，作为加固，不是本卡点的根因修复。

### 宿主复测

修复后同一工具、同一 URL、FAST_START、`timeout 15000`、系统 CA：

- `open=OK wall_s=7.063` `probe_incomplete=0` `dur_90k=672267510`
- `pump done wall_s=4.286 n=4029 au=4024 vid=1439 aud=2585 bytes=17041671 last_v_90k=5401690 media_v_s=60.019 max_gap_s=1.799`
- `wall_at_15s=3.269 wall_at_30s=3.754 wall_at_60s=4.286`（打开之后的泵墙钟）

已越过 15.7s，60s 媒体时间约 4.3s 墙钟拉完。日志不写完整 URL（仅 domain + 文件名结尾，见既有约定）。

### 官方反例（已写入 tests）

`test_https_moov_tail_pump`：本地 HTTPS + 测试 CA（`MS_FF_CA`），ffmpeg 生成 20s moov 在尾的 MP4（不加 faststart），测试服务器支持 Range 并计数请求。要求拉过 16s 视频、墙钟 < 8s、`nreq < 48` 且远小于 AU 数。不使用公网 URL。待 `./build.sh test` 跑过。

### 附记

- 本步 `./build.sh debug`（显式 `env -u FFMPEG_BUILD_ROOT -u FFMPEG_PREFIX`）在 19:09 重编了 `build/ffmpeg-9.1/prefix`（脚本已含新 demuxer 列表）。**并非有意覆盖冻结产物**。`build/ffmpeg-9.1-size`（mtime 2026-11-21 12:05）与 `android-*/size` 未改。`build/ffmpeg-9.1-size-morefmt` 仍在，尚未作为本步官方 test 前缀。
- 尚未：官方 test、重链 x86_64 APK、AVD 经代理 60s 与 seek 30 分画面确认；以及 API 18 seek / A 体积与各封装 / B C D / 最终重链。



## 2026-11-23 19:20 Asia/Shanghai — 官方 test 通过；moov 在尾 HTTPS 反例通过

- `./build.sh test`（`env -u FFMPEG_BUILD_ROOT -u FFMPEG_PREFIX`）退出 0。`test_abi_old_header`：`sizeof(ms_event)=176`，abi=1。
- `test_https_moov_tail_pump ok`：本地 HTTPS + 测试 CA，20s moov 在尾 MP4（`mdat@40 moov@377466`，391614 字节）。拉到 `last_v_s=16.023 last_a_s=16.022`，`au=1092`，墙钟 **19ms**，`nreq=4 nrange=4`（远小于 AU 数）。不使用公网 URL。
- 开 mpegps 后，短 AAC TS / HLS `.ts` 分片会被 mpeg 探针以 25 分压过 mpegts 的 2 分。已加：
  - `ms_ff`：若头上连续 188/192 的 `0x47`，在 `io_open` 里给尚未定格式的 context 强制 `mpegts`（本地短 TS）。
  - 补丁 `patches/ffmpeg-9.1-mpegts-short-probe.patch`：短 TS 且 `sumscore>6` 时返回 `AVPROBE_SCORE_EXTENSION`（高于 mpegps 的 25）。已挂入 `scripts/ffmpeg9_build.sh` 与四条 Android size 脚本。已打进 `third_party/cache/ffmpeg-9.1`，并在 `build/ffmpeg-9.1` 增量重编 `mpegts.o` / `libavformat.a` 后安装。**未改** `build/ffmpeg-9.1-size` 与 `android-*/size`。`size-morefmt` 前缀尚未增量重编（源已打补丁，下次编该前缀会带上）。
- 下一步：重链 x86_64 APK，AVD 经代理 60s 生长与 seek 30 分画面确认；然后 API 18 seek、A 体积与各封装、B/C/D、最终重链。



## 2026-11-23 19:35 Asia/Shanghai — AVD 经代理 60s 不再卡在 15.7s；seek 30 分画面有变化

- 重链 `./build.sh android-hw-x86_64`（仍用既有 size 前缀，不写冻结目录）。APK `build/android-hw-x86_64/MsHwPlayer.apk` 8,827,622 字节，SHA-256 `91abbd7645e62ff24957810ea0edde1455c99bbb198432b26556599d4b4af98f`。
- 演示 Activity 增加可选 `--ei timeout_ms`（默认仍 15000，不改起播合同）。本趟 AVD+代理+moov 在尾 `dur_open_input=22005ms`，15s 默认会 TIMEOUT；复测用 30000。
- CJZB_API34、`https://127.0.0.1:8443/vod.mp4`、`adb reverse` 仍在、TLS 校验开、应用 pem 已含测试 CA。
- 首帧：墙钟 58s，`first_output_to_surface=42537.694`，`submitted=6`，`err=OK`，`backend=amediacodec`。
- 首帧后 60s 墙钟（TCG 慢于实时，线性增长，没有停在 15.7s）：t+15/30/45/60s 的 `submitted` 139→190→250→292，`video_pts_us` 5.77s→7.94s→10.44s→12.14s。60s 窗内因播放慢未到 15.7s。
- 补测：seek 到 10s 再等，连续越过 15.7s：t+113s 约 14.94s，t+131s **17.06s**（`video_pts_us=17060444`，`submitted=1164`，`pos_90k=1535440`）。
- seek 30 分：`pos_90k=161763712`（约 1797.4s / 29.96 分），`timeline_epoch` 抬升。红acted 截图（遮 URL 栏）平均 RGB 差：首帧 vs 60s=14.660，60s vs seek30=**32.880**，首帧 vs seek30=45.689，画面有变化。证据：`user-vod/22-first-output.png`、`23-after60.png`、`24-seek30.png`、`25-retest-summary.txt`、`26-cross-15s7.txt`。
- 项 1 完成。尚未做：API 18 seek、A 体积与各封装实测、B/C/D、全部 APK 最终重链与回归。


## 2026-11-23 19:59 Asia/Shanghai — 项 2：API 18 seek 10s/20s 落点与 flush 后出帧已复测通过

夹具 `media/base30_subs.mkv` 关键帧在 **0.023 / 10.023 / 20.023**。此前 `AVSEEK_FLAG_BACKWARD` 对恰好 10.000/20.000 的请求落到前一个 GOP，复测才会出现 pos=173430（约 1.9s）和 1021860（约 11.3s）。不是 GOP 本身「对不齐整秒」的表象问题。

### 修改

- `ms_ff_seek`：目标 >0 时先向前放宽 100ms（`MS_FF_SEEK_KEY_SLOP_90K=9000`）再 BACKWARD 并钳位；**seek(0) 不加放宽**（短 TS 只有片头 I 帧时会被 `skip_to_key` 跳过）。seek 后仍清 `cfg_sig_valid`，并置 `cfg_need_resend`。
- JNI `MediaCodec.flush`：API 16–20 会使 `getInputBuffers`/`getOutputBuffers` 数组失效，flush 后两边都重新取（NDK `AMediaCodec_flush` 无此缓存）。
- 播放器 flush 后对仍配置着的解码器重送 CSD。JNI 路径不得把 avcC 配置块当成长度前缀 AU 做 `avcc_au_to_annexb`（会 `OVERFLOW` 并让 pump 退出）。改为排队 configure 时已准备好的带起始码 SPS/PPS（`csd-0`/`csd-1`，`BUFFER_FLAG_CODEC_CONFIG`）。重送后 `drive` 一次，避免 cap=1 挡住关键帧。位置只跟将提交的视频 AU，不跟等待关键帧期间丢掉的包。
- 新增官方用例 `test_ff_seek_keyframe_slop`：25s、10s GOP，seek 10s/20s 后第一枚视频关键帧须落在 9–11s / 19–21s。
- x86 测试包链接增加 `-Wl,-z,norelro`：NDK r29 lld 的 RELRO padding 可能打出 `p_filesz=0` 的 BSS LOAD，API 18 bionic `dlopen` 报 `couldn't map ... segment 3: Invalid argument`。只改 `scripts/android_hw_x86_build.sh`，不关 TLS、不用 UPX。

### 官方 test

`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 退出 0（两次，含 CSD 重送与 slop 修正后）。`test_ff_seek_keyframe_slop ok`，`sizeof(ms_event)=176`，`test_abi_old_header` 通过。HEAD 仍 `1092bec`，未 commit/push。

### CJZB_API18 复测

AVD：CJZB_API18，sdk=18，abi=x86，TCG / `-accel off`，emulator-5556。夹具 `/data/local/tmp/base30_subs.mkv`，`force_decoder=OMX.google.h264.decoder`。证据目录 `build/avd-accept-20261123/remain/api18-seek/`。

- 测试 APK：`build/android-hw-x86/MsHwPlayer.apk` **7,594,711** 字节，mtime 2026-11-23 19:58:43 Asia/Shanghai，SHA-256 `629465c95fc80ad8481ce8577d0a4d802eddf49eb8602a62f3135dcd7449d0ee`。`apksigner` 按脚本 min-sdk 18 校验。
- 首帧：`first_output_to_surface=411.529`，`submitted=9`，`backend=jni-mediacodec`，`err=OK`，`pos=0.348s`。
- seek 10s（t+1s）：`pos_90k=1000980`（**11.122s**），`timeline_epoch=2`，`submitted=59`（9→59），flush 后再出 `first_output_to_surface=57.088`，无 `feed failed`。不是旧的 ~1.93s。
- seek 20s（t+1s）：`pos_90k=1914210`（**21.269s**），`timeline_epoch=3`，`submitted=117`（59→117），`first_output_to_surface=57.585`。不是旧的 ~11.35s。
- 本 AVD 上 `screencap` 仍不能当作 MediaCodec Surface 像素证据（seek 截图体积接近、不能写「画面内容已变」）。落点与出帧依据是 pos / submitted / flush 后 first_output。
- 中间一轮（CSD 仍按 avcC AU 转换）pos 已能落到 10.000/20.000，但立即 `feed failed: OVERFLOW`，`submitted` 停在 25、无新 first_output。日志保留为 `full-logcat-r1-pos-ok-overflow.txt`。JNI 改送起始码 SPS/PPS 后复测通过（`timeline-r2.txt`、`summary-r2.txt`、`full-logcat-r2.txt`）。
- 再前一轮新 `libmshw.so` 因 filesz=0 LOAD 无法 `dlopen`（`r2-no-first-logcat.txt`）；加 `norelro` 后两段 LOAD 且 `p_filesz>0`，加载恢复。

冻结产物未写：`build/ffmpeg-9.1-size/prefix/lib/libavformat.a` mtime 仍 2026-11-21 12:05:01，`build/release/libmsmedia.so.1.0.0` mtime 仍 2026-11-21 11:57:55。`android-*/size` 未动。本步只重链了 x86 测试 APK（既有 `android-x86/ffmpeg-9.1-size` 前缀，不是 morefmt）。

项 2 完成。尚未做：A 体积与各封装、B/C/D、全部 APK 最终重链与 API34/API18 回归。

## 2026-11-23 20:00 Asia/Shanghai — 项 3 起手：libavcodec.a 7.4MB→12.9MB 的来源

对比冻结 `build/ffmpeg-9.1-size/prefix/lib/libavcodec.a`（7,389,958，mtime 2026-11-21 12:05，未改）与已有 `build/ffmpeg-9.1-size-morefmt/prefix/lib/libavcodec.a`（12,919,834，mtime 2026-11-23 19:00）。归档对象合计约 +5.52MB；其中 **仅 morefmt 新增 49 个 .o、约 5.24MB**。

`CONFIG_DECODERS=0`、`--disable-decoders` 仍在，`CONFIG_H263_DECODER=0`。增量不是「打开了软解配置」，而是新 parser 把解码器实现 .o 拉进了 libavcodec.a：

- 最大新对象：`cbs_av1.o` 554KB、`mpeg4videodec.o` 393KB、`ps.o` 292KB、`mpegvideo.o` 239KB、`mpegvideo_dec.o` 235KB、`parse.o` 234KB、`ituh263dec.o` 226KB、`me_cmp.o` 218KB。
- 新增 *parser* 对象合计约 735KB（含 `flac_parser`/`av1_parser`/`mpeg4video_parser`/`mpegaudio_parser` 等）。
- 两边都有但变大的主要是 `parser.o`（96KB→347KB，+251KB）。

与「只保留必须的 parser」直接相关：mpeg4video / h263 / av1 / mpegvideo parser 的依赖把 `*dec.o`、运动估计、CBS 等大文件带进静态库，尽管解码器注册仍关。下步只在 `size-morefmt`（及 Android `size-morefmt` 新目录）上收敛 parser 列表，**不写** `build/ffmpeg-9.1-size` 与 `android-*/size`。尚未用 morefmt 前缀重链宿主 libmsff 跑官方 test，尚未编 Android size-morefmt，尚未测 MP3/FLAC/PCM 与新封装。

## 2026-11-23 20:15 Asia/Shanghai — 项 3：收敛 parser 后宿主 morefmt 官方 test 与 strip 增量

只改 `size-morefmt` 前缀与脚本里的 parser 列表，**未写** `build/ffmpeg-9.1-size`、`android-*/size`、`build/release/libmsmedia.so.1.0.0`。冻结 `libavcodec.a` 仍为 7,389,958 字节，mtime 2026-11-21 12:05:01 Asia/Shanghai。HEAD 仍 `1092bec`，未 commit/push。

### 收敛内容

必须的 parser 现为：`h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,flac,opus,ac3`。去掉会把解码器实现拉进静态库的 `mpeg4video` / `h263` / `av1`（以及非必须的 `cavsvideo`）。`scripts/ffmpeg9_build.sh` 在复用前缀时拒绝 `CONFIG_MPEG4VIDEO_PARSER=1` 或 `CONFIG_MPEGVIDEODEC=1`。`CONFIG_DECODERS=0`、GPL/nonfree 关、`--enable-small` 仍在。加法 ABI：`MS_FORMAT_AVI`…`RM`（13–19）、`MS_CODEC_FLAC`/`PCM`、`MS_SAMPLE_FMT_FLAC`/`PCM` 只追加在枚举末尾；`sizeof(ms_event)` 仍为 176，未追加 `ms_event` 字段。

### 宿主 size-morefmt 前缀

`build/ffmpeg-9.1-size-morefmt/prefix`（mtime 2026-11-23 20:08 Asia/Shanghai，仅此目录）：

- `libavcodec.a` 8,978,670（相对冻结 +1,588,712；相对此前 fat morefmt 的 12,919,834 约 −3.94MB）。归档中已无 `mpeg4videodec.o` / `ituh263dec.o` / `cbs_av1.o` / `mpegvideo_dec.o` / `me_cmp.o`。
- `libavformat.a` 14,941,214（相对冻结 +2,853,644，新 demuxer 预期增长）。
- `CONFIG_DECODERS=0`，`CONFIG_SMALL=1`，`CONFIG_MPEG4VIDEO_PARSER=0`，`CONFIG_MPEGVIDEODEC=0`，`CONFIG_CBS_AV1=0`；`CONFIG_HEVC_PARSER=1`、`CONFIG_MPEGAUDIO_PARSER=1`、`CONFIG_FLAC_PARSER=1`、`CONFIG_MPEGVIDEO_PARSER=1`。

### 官方 test（链 morefmt）

`FFMPEG_BUILD_ROOT`/`FFMPEG_PREFIX` 指向 `build/ffmpeg-9.1-size-morefmt`，`OPENSSL_PREFIX` 指向 `build/openssl-3.5.9-size/prefix`，`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 退出 0。`test_ff_morefmt_containers ok`（mp3/flac/wav-pcm/avi/mpegps 只解复用、无软解），`test_ff_mp3 ok`，`test_ff_seek_keyframe_slop ok`，`test_core` all tests passed，`test_hw_player ok`，`test_abi_old_header`：`sizeof(ms_event)=176`、abi=1、金丝雀完好。这是宿主自测，不是 el7/设备验收。

### 宿主 strip 后 libmsff.so 增量

用同一份当前适配器、` -Os` + `--gc-sections` + `strip --strip-unneeded` 分别链冻结 size 前缀与 morefmt 前缀（**不经过** `ffmpeg9_build.sh`，以免 stamp 不匹配误编冻结前缀）。产物写在新目录，未覆盖 `build/release-size` 与 `build/release`：

- 当前源 + 冻结 size：`build/release-size-twin-frozen/libmsff.so.1.0.0` **5,857,272** 字节
- 当前源 + morefmt：`build/release-size-morefmt-lib/libmsff.so.1.0.0` **6,057,976** 字节
- **增量 +200,704 字节（约 0.19 MiB）**
- 历史 `build/release-size/libmsff.so.1.0.0` 仍为 5,767,128，mtime 2026-11-21 12:35:19，未改。宿主 so 动态链系统 libxml2/zlib，无 NEEDED 系统 libav/libssl。

`./build.sh release-size --out-dir=build/release-size-morefmt` 在编 `test_hw_backend.c` 时被 gcc 对旧 ABI 128 字节缓冲的 `-Werror=array-bounds` 打断，libmsff 尚未由该入口链出；增量改用上述专用链接，不改测试源。

### 尚未完成

Android 各 ABI `size-morefmt` 正在编（先 v7a）；设备/JNI 侧 MP3/FLAC/PCM 与新封装（MediaCodec 或 PCM 直出 AudioTrack，不扩大 AAC 软解例外）；项 4 B/C/D；项 5 重链 APK 与 API34/API18 回归。


## 2026-11-23 20:53 Asia/Shanghai — 项 3–5 进展：各 ABI size-morefmt、APK 重链、官方 test 与 item345 实测（未闭环）

只写 `docs/PROGRESS.md` 与新证据目录。HEAD 仍 `1092bec`，未 commit/push。冻结产物未写：

- `build/ffmpeg-9.1-size/prefix/lib/libavcodec.a` 仍 7,389,958 字节，mtime 2026-11-21 12:05:01 Asia/Shanghai
- `build/android-v7a/size/libmsff.so.1.0.0` 仍 4,235,076，mtime 2026-11-21 12:17:30
- `build/android-v8a/size/libmsff.so.1.0.0` 仍 5,250,888，mtime 2026-11-21 18:27:41
- `build/release/libmsmedia.so.1.0.0` mtime 仍 2026-11-21 11:57:55
- 历史 `build/release-size/libmsff.so.1.0.0` 仍 5,767,128，mtime 2026-11-21 12:35:19

x86 / x86_64 无冻结 `android-*/size` 的 libmsff 对照。未用 UPX，未关 TLS/主机名校验，未换 TLS 后端，未记完整私有流 URL。`CONFIG_DECODERS=0`，GPL/nonfree 关，稳定 C ABI 仍为 1，`sizeof(ms_event)=176`，未追加 `ms_event` 字段。

### 宿主 morefmt 前缀与官方 test（补记 20:15 之后）

对 size-morefmt 跑官方 test 必须带 `MS_SIZE_OPT=1` 且 `env -u FFMPEG_REBUILD`，否则 configure stamp 对不上会在该前缀上重编且丢掉 `--enable-small`。一次漏设后 `libavcodec.a` 曾胀到 10,751,378（`CONFIG_SMALL=0`）；随即以 `MS_SIZE_OPT=1 FFMPEG_REBUILD=1` 只在 `build/ffmpeg-9.1-size-morefmt` 恢复，`libavcodec.a` 回到 **8,978,670**，mtime 2026-11-23 20:28:44 Asia/Shanghai，`CONFIG_SMALL=1`，冻结前缀未动。

`scripts/openssl_build.sh` 的 stamp 比较原用 `$(cat)`，命令替换会吃掉尾换行，导致每次都说不匹配并重编 `android-*/openssl-3.5.9-size`。已改为 `printf '%s' "$PINREC" | cmp -s "$STAMP" -`。v7a 的 openssl 前缀在修好前被就地重编过（冻结 `libmsff.so` 已静态链入 openssl，且未被覆盖）。其它 ABI 重编前有备份 `openssl-3.5.9-size-kept-20261121`。

PCM CSD 补丁之后，用同一适配器、`-Os` + `--gc-sections` + `strip --strip-unneeded` 再测宿主增量（仍不走会写冻结前缀的 `./build.sh release-size`）：

- 当前源 + 冻结 size：`build/release-size-twin-frozen/libmsff.so.1.0.0` **5,861,368**（mtime 20:29:03）
- 当前源 + morefmt：`build/release-size-morefmt-lib/libmsff.so.1.0.0` **6,057,976**（mtime 20:29:06）
- **增量 +196,608 字节（约 0.19 MiB）**（20:15 节的 +200,704 是 PCM CSD 之前那一版孪生目录）

恢复 small 之后，以及演示 DEBUG_CMD 接线之后，两次官方 test 均退出 0：`MS_SIZE_OPT=1`，`FFMPEG_*` 指向 `build/ffmpeg-9.1-size-morefmt`，`OPENSSL_PREFIX=build/openssl-3.5.9-size/prefix`，`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh test`。前缀提示 already present（stamp 匹配，未重编）。`test_ff_morefmt_containers ok`（mp3/flac/wav-pcm/avi/mpegps 只解复用、无软解），`test_ff_mp3 ok`，`test_core` 含 FLAC/PCM `hw_path_ready` 与 MSPM 解析，`test_abi_old_header`：`sizeof(ms_event)=176`、abi=1、金丝雀完好。这是宿主自测，不是 el7/设备验收。第二次约 20:35–20:38 Asia/Shanghai。

### Android 各 ABI size-morefmt（只写新目录）

`ANDROID_SIZE_MOREFMT=1` 时 size 脚本只写 `ffmpeg-9.1-size-morefmt` 与 `size-morefmt`，不写 `android-*/size`。parser 列表与宿主相同；`link_msff` 补上宿主已有的隐藏 `ms_sub.o`；x86/x86_64 链接带 `-Wl,-z,norelro`（API 18 bionic）。strip 后 `libmsff.so`：

- v7a：`build/android-v7a/size-morefmt/libmsff.so.1.0.0` **4,562,796**，相对冻结 4,235,076 **+327,720**（约 0.31 MiB），mtime 2026-11-23 20:18:23 Asia/Shanghai。NEEDED：libz,libm,libdl,libc。
- v8a：`build/android-v8a/size-morefmt/libmsff.so.1.0.0` **5,565,224**，相对冻结 5,250,888 **+314,336**（约 0.30 MiB），mtime 20:19:41。链接本身成功；脚本曾因 `set -u` 下未绑定的 `$arch` 在成功回显时退出 1，已改为字面 ABI 名，so 有效。
- x86：`build/android-x86/size-morefmt/libmsff.so.1.0.0` **5,224,092**，mtime 20:22:41（无冻结对照）。
- x86_64：`build/android-x86_64/size-morefmt/libmsff.so.1.0.0` **6,519,336**，mtime 20:19:40（无冻结对照）。

### 源码与演示（20:17–20:45，小步复用，中文注释）

- 解复用映射 AVI/MPEG-PS/MP3/FLAC/WAV/ASF/RM；PCM 无 lavf extradata 时从 codecpar 发 12 字节小端 `MSPM` CSD（魔数 + le32 rate + le16 ch + le16 bits），不追加 `ms_event` 字段。
- `ms_audio_hw_path_ready` 接纳 FLAC/PCM；FLAC 走 MediaCodec `audio/flac`（STREAMINFO 作 csd-0），`allow_sys` 仍只放行 AAC；PCM 为 `pcm_direct`，跳过解码器挑选，直接开 AudioTrack。不是软解例外扩大。
- 演示：外挂字幕（已有 `nativeLoadSubtitle`）；LRC 在 Java 转 SRT 再走同一通道；DEBUG_CMD 增加 `rate` / `load_sub` / `lyric` / `aspect` / `shot` / `savepos` / `loadpos`；按钮 Aspect/Shot/SavePos/LoadPos；双击暂停/继续、左右滑 ±10s（顶层 `MsHwDemoGestureListener`，避免匿名内部类踩 D8 NPE）；比例 stretch / 16:9 信箱 / 裁切；截图前把地址栏改成 `(url hidden)`，写入 app `files/screenshot-demo.png`（`View.draw`，API 18 上 SurfaceView 视频区通常是黑的，不能当出画像素证据）；断点用路径哈希作 SharedPreferences 键，日志只记哈希。
- 编 APK 时 D8 曾对 `MsHwPlayerActivity$1` 内部 NPE；已把匿名手势/触摸类改成同文件顶层类。
- 中断前已发现：`surfaceCreated` 从 Intent `--es path` 自动开播时只 `nativePlayPath`，**未** `rememberMediaKey`，所以 savepos/loadpos 报 `no media key`。源码已补这一行，**尚未**因此重链 APK，也尚未再跑官方 test。

### 项 5 测试 APK（20:35 Asia/Shanghai，链 size-morefmt）

`ANDROID_SIZE_MOREFMT=1`，`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh android-hw` / `android-hw-x86` / `android-hw-x86_64`。输出在 `build/android-hw*`，不写 `android-*/size`。含上述演示接线，**不含**随后的 `rememberMediaKey` 补丁。

- `build/android-hw/MsHwPlayer.apk` **14,423,137** B，mtime 20:35:37，SHA-256 `03f1fdbe647cab5f25c909a02039db2aeafb06312b60061d1be6bf126860ec02`
- `build/android-hw-x86/MsHwPlayer.apk` **7,541,463** B，mtime 20:35:28，SHA-256 `25421211c7a151a0c361dc51d399db5e057f268e22b4284420f783ee8501b4ab`
- `build/android-hw-x86_64/MsHwPlayer.apk` **8,766,182** B，mtime 20:35:28，SHA-256 `8025026bdaf6c57f4730667235863bc18fbfdd2e5212877a5d2b4d51696660bb`

### item345 实测（20:41 起，证据 `build/avd-accept-20261123/remain/item345/`）

AVD：CJZB_API18 emulator-5556（sdk=18，x86，`OMX.google.h264.decoder`，TCG / `-accel off`，`-no-audio`）；CJZB_API34 emulator-5554（goldfish/c2，`backend=amediacodec`，TCG，`-no-audio`）。**模拟器软件/虚拟解码，不得写成真机硬解。** 未重启模拟器。首轮因把 `/data/local/tmp/ms345` chmod 成 644（目录无执行位）导致 `Permission denied`，日志留在 `run-permdenied.log`；改 755 后续跑，脚本与证据目录均保留。截图已遮顶部地址栏；API 18/34 的 `screencap` 以及演示 `View.draw` 截图**不能当作 MediaCodec Surface 像素证据**（API 18 Surface 区通常是黑的）。

#### CJZB_API18（x86 测试 APK）

- `base30_subs.mkv`：`first_output_to_surface=416.784`，`submitted=45`，`err=OK`，`backend=jni-mediacodec`，`pos≈1.788s`。force 为 `OMX.google.h264.decoder`。
- seek 10s：脚本等了约 37s 后记 `pos=13.978`、`pos_ok=False`、`first_output_to_surface=92.168`、`epoch=2`、`submitted=144`、`err=OK`。项 2 在 seek 后约 1s 测到 11.122s；本次可能是等待过久、媒体时钟从关键帧附近往前走，**尚未当作回归失败，也不当作已通过**，需在 seek 后立刻读 pos 再确认。
- seek 20s：`pos=21.176`（±2.5s 内），`pos_ok=True`，`first_output_to_surface=106.507`，`epoch=3`，`err=OK`，flush 后再出帧。
- pause/resume 通过（`paused=1` / 恢复）。
- `short.ts`：`fo=79.77`，`submitted=23`，`err=OK`，jni-mediacodec。
- morefmt `more_h264.avi`：`fo=94.968`，`submitted=25`，`err=OK`。
- morefmt `more_h264.mpg`（MPEG-PS）：`fo=62.867`，`submitted=25`，`err=OK`。
- MP3：解复用成功（`have=1`，`find_info` 完成），`configured=0`，`write_frames=0`，`audio: status=UNSUPPORTED`，细节为拒绝软件解码器（`OMX.google.mp3` / isSoftwareOnly）。`allow_software=0`，`allow_sys` 仍只放行 AAC，**未扩大软解例外**。本 AVD 没有硬解 MP3。
- FLAC：同样 `UNSUPPORTED`；本 AVD `media_codecs.xml` 只有 `OMX.google.flac.encoder`，无 FLAC 解码器。
- WAV/PCM：路径已走通，`configured=1`，`detail=PCM direct AudioTrack; not a decoder`，`unsupported=0`。`write_frames=0` / `write_bytes=0`，但 `head_frames=2048` 且 `clock_us` 上升。不是「没声」（AVD `-no-audio` 不能当失败），但写出计数与轨头不一致，要查是计数没加还是写出没真正走完。脚本里的 `UNSUPPORTED_seen` 是误匹配，不足为据。
- BCD：`load_sub` / `lyric`（日志有 `lyrics loaded as external SRT`）/ `rate` / `aspect`（stretch、16:9 信箱、crop）/ 应用内截图（`screenshot-demo.png` 37225B，地址栏已遮）脚本记 True。savepos/loadpos 失败，日志明确 `savepos: no media key` / `loadpos: no media key`（见上，Intent path 未记 key）。`input swipe` 位置未动，尚不能区分是手势未接到还是无窗口/点不到画面区。

#### CJZB_API34（x86_64 测试 APK，无 force_decoder）

- `long_multi.mkv`：出画 `first_output_to_surface=24528.052`（约 24.5s，TCG 慢），`submitted=41`，`err=OK`，`backend=amediacodec`，组件为模拟器 goldfish/c2，**不是真机硬解**。pause/resume 通过。
- seek 20s：`pos=20.222` 落点看起来对，但 `paint=False`，`first_output_to_surface=-0.001`，`err=OVERFLOW`，`submitted` 停在 195，`backpressure` 继续涨，flush 后未见新 first_output。**必须修并复测**，不能把 pos 当成出帧成功。
- BCD：load_sub/lyric/rate/aspect/shot 脚本记 True；savepos/loadpos 同样因 no media key 失败（`loadpos_pos_s=0.222`）；swipe 未动。
- morefmt AVI（中断时正在跑，随后已出结果）：`fo=3619.143`，`submitted=25`，`err=OK`，`backend=amediacodec`，`pos≈1.022s`。MPEG-PS 及 MP3/FLAC/WAV 在脚本里还在继续，不中断、不重装、不重启模拟器。

### 接下来（未完成）

1. 查清并修 API34 seek 后 `OVERFLOW`、PCM `write_frames` 与 `head_frames` 不一致、手势是否只是夹具/无窗口问题；`rememberMediaKey` 已补的源码改动后跑官方 test 并重链测试 APK。不扩大 AAC 软解例外。
2. 等当前 `run_reg.py` 把 api34-mpg 及音频用例跑完（可续跑，不无故重装）。
3. 用修好的包复测 API34 seek、断点、手势，以及未闭环的 B/C/D。
4. 最终确认 APK SHA、官方 test、API34/API18 回归。API 19 仍无镜像，跳过。


## 2026-11-23 21:09 Asia/Shanghai — 项 3–5 收束：OVERFLOW/PCM/断点已修并复测，最终 APK 与官方 test

HEAD 仍 `1092bec`，未 commit/push。冻结产物未写：`build/ffmpeg-9.1-size/prefix/lib/libavcodec.a` 仍 7,389,958，mtime 2026-11-21 12:05:01 Asia/Shanghai；`android-v7a/size/libmsff.so.1.0.0` 仍 4,235,076（12:17:30）；`android-v8a/size` 仍 5,250,888（18:27:41）；`build/release/libmsmedia.so.1.0.0` mtime 仍 11:57:55。size-morefmt 宿主 `libavcodec.a` 仍 8,978,670，mtime 20:28:44。模拟器未重启。未用 UPX，未关 TLS，未扩大 AAC 软解例外，ABI 仍为 1，`sizeof(ms_event)=176`。API 19 无镜像，仍跳过。

### item345 脚本已把剩余封装跑完（20:35 包，修前）

证据仍在 `build/avd-accept-20261123/remain/item345/`，脚本与目录未删。中断后 `run_reg.py` 继续跑完，未重装、未重启模拟器。

- API34 MPEG-PS：`fo=2318.416`，`submitted=25`，`err=OK`，`backend=amediacodec`，`pos≈0.86s`（goldfish/c2，非真机硬解）。AVI 此前已出画 `fo=3619.143`，`submitted=25`，`err=OK`。
- API34 MP3/FLAC：与 API18 相同，解复用 `have=1`，`configured=0`，`audio: status=UNSUPPORTED`，细节为拒绝软件解码器（`c2.android` / `omx.google` / isSoftwareOnly）。`allow_sys` 仍只放行 AAC。本 AVD 无硬解 MP3/FLAC，这是策略结果，不是 demux 失败。
- API34 WAV（修前）：`configured=1`，`PCM direct AudioTrack`，`unsupported=0`，但 `write_frames=0`、`head_frames=0`。API18 修前则是 `write_frames=0` 但 `head_frames=2048`。

截图已遮顶部地址栏，**不能当作 Surface 像素证据**。

### 原因与修改

1. **API34 seek 后 `err=OVERFLOW`、flush 后再无 first_output。** 与项 2 在 JNI 路径修过的问题同类：`ms_hw_android_submit`（NDK / `amediacodec`）在 `nal_len_size>0` 时对所有包做 `avcc_au_to_annexb`，flush 后重送的 avcC/起始码 CSD 被当成长度前缀 AU，转换失败并留下 OVERFLOW。已改为 `MS_HW_FLAG_CODEC_CONFIG` 只做原样拷贝，与 JNI 注释一致。
2. **PCM `write_frames=0`。** `pcm_direct` 驱动在 `AudioTrack.write` 返回 0 时立刻丢槽且不计数；压缩音频路径已有的 `-no-audio` 超时丢弃并计入写出，这里没复用。已在直通循环里对齐：未超时则留槽再写，超时则按 `plat_write_pcm` 计入后释放。
3. **savepos/loadpos。** 两处问题：`surfaceCreated` 从 Intent `--es path` 开播时未 `rememberMediaKey`（此前已补）；补上之后仍报 `savepos skipped (near start)`，是因为 `nativePosition` 输出 `pos=12.376s`，解析只认 `pos_90k=`，得到 -1 被当成「接近开头」。已同时解析 `pos=` 秒值，解析失败不再说 near start。日志只记哈希，不记路径/URL。
4. **手势。** 画面区只有顶部 320px 的 `stage`，脚本按整屏 62% 高度滑，API18 会滑到按钮/日志区。另外 SurfaceView 等子 View 会先吃触摸。已在 stage 顶上加透明 `gestureLayer` 接收滑动/双击。即便如此，在本无窗口 AVD 上 `adb input tap/swipe` 仍未触发暂停或 ±10s：API18 带 duration 的 `input swipe` 会报 `Invalid arguments`；dumpsys 里主窗口有 `isReadyForDisplay()=false`。`DEBUG_CMD` 的 pause / `seek_rel` / rate / aspect 可用。 **不能写成手势已在设备上验收**，判断为无窗口注入限制，不是再挡交付的功能缺失。

### 官方 test

源码改动后两次 `MS_SIZE_OPT=1`、`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin`，`FFMPEG_*` 指向 `build/ffmpeg-9.1-size-morefmt`，`OPENSSL_PREFIX=build/openssl-3.5.9-size/prefix`，`./build.sh test` 均退出 0。前缀 already present，未重编，`CONFIG_SMALL=1`，`CONFIG_DECODERS=0`。`test_ff_morefmt_containers ok`，`test_abi_old_header`：`sizeof(ms_event)=176`、abi=1。宿主自测，不是 el7/设备验收。最后一次约 21:06–21:08 Asia/Shanghai（含位置解析与手势层）。

### 修复后复测（不重启模拟器，只 `adb install -r`）

证据：`remain/item345/retest/`（OVERFLOW / PCM / seek 落点）与 `remain/item345/retest2/`（断点）。所用包见下一节最终 APK（retest 用 20:59 包，已含 OVERFLOW/PCM/`rememberMediaKey`；retest2 用 21:05 最终包，另含位置解析与手势层）。force 仅 API18 视频 `OMX.google.h264.decoder`；API34 默认 goldfish/c2。**不得写成真机硬解。**

- API18 seek 10s（seek 后约 2s 读）：`early_pos=12.376`，`pos_ok=True`，`fo=133.216`，`err=OK`，`submitted=107`，`epoch=2`。先前 item345 的 13.978 是等了约 37s 时钟往前走，不是旧的 10s→1.93s 回退。
- API18 seek 20s：`early_pos=22.036`，`fo=65.331`，`err=OK`，`submitted=158`。
- API34 `long_multi` 出画 `fo=18414.733`（TCG 慢），`err=OK`，`submitted=37`。seek 20s：**`early_pos=20.865`，`paint=True`，`fo=1948.916`（flush 后新的 first_output），`err=OK`，`submitted` 83→115，`epoch=3`，`cfg=2`。无 OVERFLOW。**
- PCM：API18 `configured=1`，`write_frames=22528`，`head_frames=22528`，`PCM direct`，非 UNSUPPORTED。API34 `write_frames=12204`，`head_frames=12204`，同样直通成功。AVD `-no-audio` 不能当「有声」验收，但写出路径已走通。
- 断点（最终包）：API18 `savepos key=m2dc838bb t=11772ms`，loadpos 后 `pos=12.213s`（关键帧附近，在 10s±3s 内）。API34 `savepos key=m398e0c93 t=11137ms`，loadpos 后 `pos=10.646s`。不再出现 no media key / near start 误判。
- 外挂字幕、LRC→SRT、倍速、比例、应用内截图（地址栏已遮）在 item345 的 BCD 中已由日志确认。手势见上，本 AVD 注入未跑通。

### 最终测试 APK（21:05:51–21:06:01 Asia/Shanghai，链 size-morefmt）

`ANDROID_SIZE_MOREFMT=1`，`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh android-hw` / `android-hw-x86` / `android-hw-x86_64`。输出只在 `build/android-hw*`。含上述全部修复。

- `build/android-hw/MsHwPlayer.apk` **14,423,137** B，mtime 21:06:01，SHA-256 `e5e8d54d0755aee6ae718fac09ea57588b4f5a88bfafa39e46eada9bffa18dd5`
- `build/android-hw-x86/MsHwPlayer.apk` **7,541,463** B，mtime 21:05:51，SHA-256 `dd247ae2e757454ea1c59119b16618b22931decb76e8e5e03f1f7bb553030678`
- `build/android-hw-x86_64/MsHwPlayer.apk` **8,766,182** B，mtime 21:05:51，SHA-256 `8219a503e036b0bf696ed6fb2f337ffbbb1d870c8424ab797387a9cfc5b9a1f5`

### 未完全项（不挡已测结论）

- 本无窗口 TCG AVD 上，adb 触摸未触发演示手势；真机或有窗口环境未测。
- MP3/FLAC 在这两台只有 Google/c2 软件解码器的 AVD 上按策略 `UNSUPPORTED`（未扩大 AAC 例外）。有硬解或允许软件的设备未测。
- 截图与 screencap 不能证明 Surface 像素；API18 Surface 区通常是黑的。
- 官方 test / 宿主自测不是 el7 或真机验收。


## 2026-11-23 21:13 Asia/Shanghai — 统一交付包（非阶段包）

对照 2026-11-21 18:38 的扁平归档方式，在新目录打包，**未覆盖、未删除** `/workspace/MsMedia-deliver-20261121-1838/` 及其他历史包，未改写冻结产物。HEAD 仍 `1092bec`，未 commit/push。未用 UPX，未关 TLS。包内说明与清单不写完整私有流 URL。

完整路径：`/workspace/MsMedia-deliver-20261123-2113/`

### 内容清单与核对后的 SHA / 体积

| 文件 | 字节 | SHA-256 |
| --- | ---: | --- |
| MsHwPlayer.apk（v7a+v8a 默认交付） | 14,423,137 | e5e8d54d0755aee6ae718fac09ea57588b4f5a88bfafa39e46eada9bffa18dd5 |
| MsHwPlayer-x86.apk（测试用，非默认交付） | 7,541,463 | dd247ae2e757454ea1c59119b16618b22931decb76e8e5e03f1f7bb553030678 |
| MsHwPlayer-x86_64.apk（测试用，非默认交付） | 8,766,182 | 8219a503e036b0bf696ed6fb2f337ffbbb1d870c8424ab797387a9cfc5b9a1f5 |
| MsMedia-src-20261123-2113.tar.gz | 1,085,316 | 8b57c45d3606bb6079d19540ffe161540390529acf7ed0e63d5cf861574715df |
| 验收说明.md | 3,333 | 34a000eff99409dcce168ffffc52a9e9ef265fd5151bcda71f18fc73bab4d0cc |
| 体积对照.md | 2,314 | d039b04c2cc118cbbb9bd81af5a3a031780dbbfefc52a41ca0cf3471860fc0f3 |
| SHA256SUMS.txt | 884 | （清单本身；格式与 18:38 包相同：文件名 +「哈希  绝对路径」；三份 APK 的哈希、体积已与 21:05:51–21:06:01 最终构建产物逐一核对） |

源码 tar 前缀为 `MsMedia/`，纳入当前工作区源码、文档、脚本、测试、`patches/`、`third_party/*.pin`；不含 `.git`、`build/`（因此不含冻结产物与 AVD 证据）、`third_party/cache`。APK mtime 仍为构建时的 21:05:51 / 21:06:01 Asia/Shanghai。

### 验收说明（摘要）

详见包内 `验收说明.md` 与 `体积对照.md`。要点：

- 宿主官方 test 已通过，**不是 el7/真机验收**。
- 模拟器视频是 `OMX.google` / goldfish 软解或虚拟解码，**不是真机硬解**。
- AVI/MPEG-PS 出画、Android 4.3 与较新系统拖进度、PCM 直通写出、外挂 SRT、LRC、倍速、比例、应用内截图、断点续播已在模拟器确认。
- MP3/FLAC 能解复用，但模拟器无硬解，策略不走系统软解（`allow_sys` 仍只放行 AAC）。
- `-no-audio` **不能做出声验收**。
- 无窗口 TCG AVD 上滑动手势未触发，**不能写手势已验收**。
- 无 Android 4.4 镜像，**API 19 未测**。
- 冻结产物未改（`ffmpeg-9.1-size`、`android-v7a/size`、`android-v8a/size`、`build/release/libmsmedia.so.1.0.0`）。


## 2026-11-24 09:52 Asia/Shanghai — 阶段 1：纯自研能力盘点、libmsff 体积解剖、路线 A 单 so（libmsplayer.so）、路线 B 字节预算、MediaPlayer 骨架

对照用户「单架构合并成一个 2 MB 以内的 so、自研与精简 FFmpeg 两条线对比、播放不出错 / 不泄漏 / 不闪退」，以及随后补充的统一产物名 `libmsplayer.so`、清掉用不到的算法、Java 层按系统 `android.media.MediaPlayer` 封装。本轮只做阶段 1（盘点 + 解剖 + 路线 A 原型 + 路线 B 精确预算 + API 差距与骨架），**不是**真机验收，也尚未产出路线 B 精简 FFmpeg 单 so。

红线：未删除或修改任何 `/workspace/MsMedia-deliver-*`；未改写冻结产物（`build/release/libmsmedia.so.1.0.0`、`build/ffmpeg-9.1-size`、`build/android-v7a/size`、`build/android-v8a/size`、`build/release-size`）。对照开跑前的 986 个冻结/交付文件 SHA-256，结束时 **986 全部一致、0 缺失**。HEAD 仍为 `1092bec`，未 commit / push。未使用 UPX，未关闭或弱化 TLS 校验，未启用 GPL/nonfree，FFmpeg 仍 `CONFIG_DECODERS=0`。稳定 C ABI v1 未改（官方 test 金丝雀 `sizeof(ms_event)=176`）。现有多 so 构建脚本 `scripts/android_hw_build.sh` 与官方测试未改行为。新产物只写在 `build/android-v7a/single-native/`、`build/android-v8a/single-native/`、`build/single-analysis/`，以及新增源码 `scripts/android_single_build.sh`、`src/single/libmssingle.map`、`android/hw/java/com/msmedia/MediaPlayer.java`、`docs/MEDIAPLAYER_API.md`。日志与本文不写完整私有流 URL。

### 1. 纯自研核心（不依赖 `libmsff.so` / `libmsass.so`）实际能力

逐项核对源码（`ms_session` / `ms_ts` / `ms_bmff` / `ms_flv` / `ms_source` / `ms_hls` / `ms_dash` / `ms_sub` / `ms_hdr` / `ms_net` / `ms_registry`，以及 JNI 无 `MS_HW_HAVE_FF` 分支）。**「已自研落地」指核心/备用路径有实现；不等于已在 Android 单 so 演示里接通，也不等于真机验收。**

#### 协议

| 协议 | 自研 | 说明 |
| --- | --- | --- |
| 本地 push / 内存源 | 已落地 | `ms_session_push`；JNI 无 FFmpeg 时用 `fopen` 整文件读入再 push |
| 本地文件随机读 | 已落地（核心） | `ms_file_source` + `ms_session_set_source` / `drive`；当前 Android 纯自研 JNI **未调用**，单 so 链接时该 .o 被 `--gc-sections` 整份丢掉 |
| HTTP | 备用库已落地，**不在核心 so** | 可选 `libmsnet`（`src/net/ms_net.c`），核心 `libmsmedia` **不链**网络 |
| HTTPS | 备用库已落地，**不在核心 so** | `libmsnet` 经 `ms_tls.c` 走 **OpenSSL**，校验证书与主机名，不可关；**不是**安卓系统 TLS。Android 现行播放走 `libmsff` + 静态 OpenSSL。纯自研单 so **尚未**带 HTTP/HTTPS |
| HLS | 备用已落地（有界子集） | `ms_hls.c`：仅 media playlist；master / `EXT-X-MEDIA` / ABR / 非 `METHOD=NONE` 的 `EXT-X-KEY` 返回 `UNSUPPORTED`。通用 HLS 以 lavf 为准 |
| DASH | 备用已落地（最小子集） | `ms_dash.c`：单 Period、模板/列表，有界 XML；明确不是 ABR/DRM。通用 DASH 以 lavf 为准 |
| RTSP / RTMP / RTMPS / UDP / RTP | **仅 FFmpeg** | 自研无实现 |

#### 封装

| 封装 | 自研 | 说明 |
| --- | --- | --- |
| MPEG-TS | 已落地 | 188/192；AU 组帧仅 **H.264 Annex B 子集 + AAC ADTS 子集**；其余流多为 `CONTAINER_ID` |
| 渐进 MP4 / MOV | 已落地 | ISO BMFF；`stsd` 只认 **`avc1` / `mp4a`**；加密 `encv`/`enca` 拒绝；`hvc1`/`hev1` 等未解析。时间定位仅渐进样本表 |
| fMP4 | 已落地（AU） | moof+mdat，同样仅 H.264/AAC；**任意时间 seek 返回 `NOT_SEEKABLE`** |
| FLV | 已落地 | 经典 FLV，仅 **AVC / AAC** AU |
| MKV / WebM / AVI / MPEG-PS / 裸 MP3 / FLAC / WAV / ASF / RM | **仅 FFmpeg** | 自研会话无法识别；压力测试中 HEVC MP4、MKV 样本 AU=0，属预期 |

#### 编码（识别 / 组帧，不是软解）

| 编码 | 自研 | 说明 |
| --- | --- | --- |
| H.264 | AU（声明子集） | TS Annex B（AUD 或 7.4.1.2.4）；MP4/MOV/fMP4/FLV 为 avcC / AVC | 
| AAC | AU（声明子集） | TS 仅 ADTS；MP4/FLV 为裸 AU；LATM 不处理 |
| HEVC | 有解析器 / TS 可标识 | `ms_hevc.c`、`ms_hdr.c`；TS `0x24` 为 `CONTAINER_ID`，**不组完整 AU**；BMFF 不认 hev1/hvc1。完整 AU 走 lavf |
| MPEG-2 / MPEG-4 Part 2 / AVS 家族 / AC3 / EAC3 / MPEG_AUDIO | TS 仅 `CONTAINER_ID` | 无自研 AU 组帧 |
| MP3 / MP2 / Opus / FLAC / PCM / G.711 / H.263 / AV1 / AVS+ | 查询为 NONE 或仅比特流工具 | `ms_vbit` / `ms_abit` 有部分探测工具；完整 AU **走 lavf**。PCM 直出不是压缩软解，在 FFmpeg 路径已接线 |

#### 字幕与歌词

| 种类 | 自研 | 说明 |
| --- | --- | --- |
| 外挂 SRT / WebVTT / ASS / SSA 文本 | 已落地 | `ms_sub`：有界 cue / ASS 布局模型与去标签，**不是完整字体栅格** |
| 位图 PGS / DVB / VobSub | 已落地（解析为 RGBA cue） | 不是视频软解 |
| 完整 ASS 特效渲染 | **可选 `libmsass`** | 不在纯自研核心 so 内；缺失则保持去标签路径 |
| LRC | **仅 Java Demo** | `MsHwPlayerActivity.lrcToSrt`，核心无 LRC 解析器 |

#### HDR 与 API 18

- HDR：`ms_hdr.c` 解析 HEVC SEI（Mastering / CLL 等）并映射 Android color standard/transfer。自研 BMFF 目前到不了 HEVC 样本，宿主 HDR 夹具出 AU 仍依赖 lavf。
- API 18：纯自研单 so **不**链 `ms_api18_libc.c`（那是补 `libmsff` 的 `atof`/`signal`）。`readelf` 相对现行 `libmshw`+`libmsmedia` **无新增未解析符号**；运行时 API 18–20 仍走 JNI MediaCodec，21+ 走 `dlopen libmediandk`（不写进 `NEEDED`）。本轮未上 Android 4.3 真机/镜像。

### 2. 当前 `libmsff.so` 体积解剖（morefmt = 现行交付配置）

在新目录 `build/single-analysis/msff-map/<abi>-morefmt/` 按 `android_hw_build.sh` 同一命令重链并加 `-Wl,-Map`，**只读**已有 size 前缀，不改冻结目录。strip 后体积与现行 APK 内 so 一致：v7a **4,563,588**、v8a **5,566,088**。

二次归属（合并字符串按合并前比例估算；动态重定位按 `r_offset` 精确；v8a 的 `.eh_frame_hdr` 按 `.eh_frame` 比例摊回）：

| 库 | v7a 字节 | v7a % | v8a 字节 | v8a % |
| --- | ---: | ---: | ---: | ---: |
| `libcrypto.a` | 2,230,699 | 48.9 | 3,000,548 | 53.9 |
| `libavformat.a` | 806,517 | 17.7 | 834,104 | 15.0 |
| `libssl.a` | 515,618 | 11.3 | 568,044 | 10.2 |
| `libavcodec.a` | 357,490 | 7.8 | 411,094 | 7.4 |
| `libxml2.a` | 331,613 | 7.3 | 371,946 | 6.7 |
| `libavutil.a` | 179,954 | 3.9 | 212,517 | 3.8 |
| 适配器 `.o`（`ms_ff` / `ms_sub` / HEVC/HDR/vbit/abit/avcc 等） | 111,679 | 2.4 | 111,987 | 2.0 |
| 其余（crt / builtins / 头对齐） | ~26,018 | 0.6 | ~55,848 | 1.0 |
| **合计（strip 后文件）** | **4,563,588** | 100 | **5,566,088** | 100 |

其中 **静态 OpenSSL（crypto+ssl）v7a 2,746,317（60.2%），v8a 3,568,592（64.1%）**，是单架构 2 MB 的主要障碍。v8a 另有 `.eh_frame` 505,424 + `.eh_frame_hdr` 99,748（合计约 605 KB，OpenSSL 占其中大部分）。

v7a morefmt 前 20 个最大输入 `.o`（链接器合成段合计 622,333 另计）：`libcrypto-lib-ecp_nistz256-armv4.o` 158,112；`mov.o` 115,932；`encode_key2any.o` 91,640；`libxml2_la-parser.o` 71,280；`obj_dat.o` 68,093；`curve25519.o` 64,731；`ps.o` 40,984；`ms_sub.o` 40,696；`ms_ff.o` 40,684；`h264dsp.o` 37,592；`matroskadec.o` 35,190；`demux.o` 33,504；`hls.o` 31,254；`rtsp.o` 30,552；`t1_lib.o` 29,963；`pixdesc.o` 29,360；`mpegts.o` 29,008；`libxml2_la-valid.o` 28,188；`statem_srvr.o` 25,785；`rtmpproto.o` 25,724。

#### 路线 B（精简 FFmpeg 单 `libmsplayer.so`）压进 2,097,152 字节的预算

不使用 UPX；保持证书 / 主机名校验；`CONFIG_DECODERS=0`；合并为一个 so，version script 只导出 JNI（与路线 A 相同 `src/single/libmssingle.map`），加 `-Os/-Oz`、`--gc-sections`、ICF，v8a 必须加 `-fno-unwind-tables -fno-asynchronous-unwind-tables` 去掉残留 eh_frame。

1. **甩掉静态 OpenSSL**，FFmpeg 网络 TLS 改为 **安卓系统 TLS**（JNI + `HttpsURLConnection` / `SSLSocket` 或等价，校验证书与主机名，0 字节原生 SSL）。不得关校验，不引入 mbedTLS 作为第一方案（v8a 余量紧）。
   - v7a：4,563,588 − 2,746,317 = **1,817,271**，已低于 2 MB。
   - v8a：5,566,088 − 3,568,592 = **1,997,496**，只剩约 100 KB 余量。
2. 与自研核心 + JNI 合并时，适配器约 112 KB 与核心重复，可去重；再计入纯自研单 so 中硬件/会话等增量（v7a 约 187,528 − 111,679 ≈ 76 KB，v8a 约 248,912 − 111,987 ≈ 137 KB）：
   - v7a 估 **≈ 1,893,120**（余量约 204 KB）。
   - v8a 估 **≈ 2,134,421**（超标约 37 KB）。必须再砍：去掉 OpenSSL 走后仍留下的 eh_frame（约 165 KB）后估 **≈ 1.97 MB**；若仍贴线，再去掉 **libxml2**（DASH 改走自研 `ms_dash` 或缩小 XML），v8a 可再省约 372 KB。
3. 继续清未走到的 FFmpeg 实现：关闭 FFmpeg 的 `--enable-openssl`；评估 QUIC/冷门 protocol；demuxer 已按 morefmt 白名单，不默认再扩。禁止启用 decoder / GPL / nonfree。

阶段 2 在 `build/android-v*/single-slimff/libmsplayer.so` 按此预算实测，不改冻结前缀（若需无 OpenSSL 的 FFmpeg，只在新目录重配重编）。

#### 适合剥离 / 重写进自研核心的 FFmpeg 能力（按体积与常用度）

优先：BMFF 补 `hvc1`/`hev1`（解析器已有）；轻量 MKV/WebM 解复用（`matroskadec.o` 约 32–35 KB，但是 lavf 周边还会拉 avutil）；裸 MP3 / FLAC / WAV；AVI / MPEG-PS。网络：自研 HTTP(S) 改走系统 TLS，而不是再链一套 SSL。RTSP/RTMP 协议栈（`rtsp.o` / `rtmpproto.o` 各约 25–30 KB，加上状态机）短期仍留在精简 FFmpeg 线，不在阶段 1 重写。

### 3. 路线 A：纯自研单 `libmsplayer.so`（v7a + v8a）

新增 `scripts/android_single_build.sh`（默认 `MODE=pure`），把与官方脚本相同的核心源 + 硬件/JNI 源合并编译为一个 so，**不链** `libmsff` / `libmsass` / OpenSSL。SONAME / 交付文件名统一为 **`libmsplayer.so`**。`--gc-sections` + ICF + hidden + 仅导出 `JNI_OnLoad` / `Java_com_msmedia_*`。

链接 map 核对后，下列模块在当前 JNI 调用图上被整份 GC（不占交付体积）：`ms_dash.c`、`ms_hls.c`、`ms_hw_vaapi.c`（Linux VAAPI）、`ms_source.c`（JNI 尚未走 Source）。仍编译进对象，便于阶段 2 接通 MediaPlayer / 文件随机读时再被引用；未引用则不会进入 so。

默认交付档为 `-Os`（保守），并复制到目录根：

| 产物 | 字节 | SHA-256 | 相对 2,097,152 |
| --- | ---: | --- | --- |
| `build/android-v7a/single-native/libmsplayer.so`（os） | **187,528** | `356ad6958ab5a2a32ca2e32e346611b109ab9e5e7a8f1602afe5b29ca9a7906e` | 8.9% |
| `build/android-v8a/single-native/libmsplayer.so`（os） | **248,912** | `24aac58b10d59eca63fc007744e4320eebe46d332ec96886bf524c42def988b6` | 11.9% |
| v7a `oz` / `oz-lto` | 174,856 / 172,716 | `48cbd9b8…` / `e50fa2b8…` | 对照 |
| v8a `oz` / `oz-lto` | 225,072 / 222,760 | `c39222fa…` / `cceba465…` | 对照 |

`readelf -d`：`NEEDED` 仅为 `libandroid.so`、`liblog.so`、`libm.so`、`libdl.so`、`libc.so`；SONAME `libmsplayer.so`；动态导出 36 个，全部为 JNI 符号。对照旧 `libmshw`+`libmsmedia` **无新增 UND**。

宿主 ASan + UBSan（`detect_leaks=1`，另用计数分配器核对 `destroy` 后核心内存归零；同编译器的泄漏探针可被 LSan 抓住，说明检测开着）。`tests/single_core_stress.c` 对 33 个本地样本各 30 轮：push 随机分块、Source 短读/AGAIN、渐进 MP4/MOV 随机 seek、随机 cancel、字节变异、外挂字幕解析。**RESULT: PASS**，全部 `live_after=0`，无 ASan/UBSan 异常、无闪退。预期内：HEVC MP4 / MKV AU=0（自研不识别）；TS/FLV/fMP4 的 seek 为 `NOT_SEEKABLE`；渐进 MP4/MOV `seek_ok` 正常。1000 轮加长压力在写本节时仍在跑，不把它写成已完成。

官方测试：`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh test` → **EXIT:0**（全绿）。因宿主 `build/ffmpeg-9.1/prefix` 与当前 OpenSSL pin 不一致，脚本按设计只重配了 **非冻结** 的 `build/ffmpeg-9.1`（日志写明不碰 Android size / 历史 release），随后 `test_abi_old_header: sizeof(ms_event)=176`、各 test 报告 PASS。非正式 el7 / 真机验收。

本轮不是设备播放验收：纯自研单 so 尚未打进 APK；无 FFmpeg 时现有 Activity 只能本地 path + 自研 H.264/AAC 子集；HTTP(S)/MKV 等仍不可播。

### 4. Java 标准 MediaPlayer 封装（阶段 1 骨架）

详见 `docs/MEDIAPLAYER_API.md`。类名建议并已落地：**`com.msmedia.MediaPlayer`**（与系统类同名不同包，业务改 import 即可切换）。

现有 Demo 差距（要点）：多 so `loadLibrary`（`msmedia`/`msff`/`msass`/`msai`/`mshw`）对系统播放器无对应物；`nativePlayPath`/`nativeOpenUrl` 没有 prepare 阶段；时间单位是 90 kHz 且 `nativePosition` 返回字符串；无 `setVolume` / `setLooping` / 标准 listener；速率是私有 `setRate(num,den)` 而不是 `setPlaybackParams`。

骨架 `android/hw/java/com/msmedia/MediaPlayer.java` 已按系统合同写出常用公开 API、常量值、七个指定 listener 与状态机，只加载 **`libmsplayer.so`**。用 android-34 / `--release 11` 单独编译到 `build/single-analysis/mediaplayer-classes/`，**未**加入 `scripts/android_hw_build.sh`，不改变现行 APK。`prepare` 在原生未接线时诚实失败（`IOException` / `OnErrorListener`），不假装已出画。阶段 2 再为 `Java_com_msmedia_MediaPlayer_*` 加薄 JNI，90 kHz↔毫秒转换留在 so 内，不改 C ABI v1。字幕/AI 等私有能力不放进标准类方法名。

### 5. 阶段 2 预告（本轮不做）

- 落地路线 B：新目录重配无静态 OpenSSL 的 FFmpeg，合并为 `build/android-*/single-slimff/libmsplayer.so`，用 map 对账压进 2 MB。
- 把 `com.msmedia.MediaPlayer` 接到 `libmsplayer.so`；需要时再打单 so 演示 APK（仍不覆盖历史交付包）。
- 两条线用同一组样本对比体积、格式覆盖、起播/播控/seek、泄漏与稳定性。

## 2026-11-24 10:55 Asia/Shanghai — 阶段 2：路线 B 单 so（single-slimff）、MediaPlayer 接线、双线对比、起播耗时拆分

本轮仍**不是**设备验收：没有 APK 跑测，没有真机，也没有模拟器。下文所有耗时都来自宿主 x86_64 的 ASan/UBSan `-O1` 构建，**不代表**真机或模拟器速度。HEAD 仍为 `1092bec`，没有提交。冻结产物（986 个文件）按 `build/single-analysis/frozen-stage2-before.sha256` 全部 OK。阶段 1 的 `single-native/libmsplayer.so` 未动（v7a 187,528 B / v8a 248,912 B）。

### 1. 产物（NDK r29，编译 API 21，minSdk 18，-Os，gc-sections + ICF，系统 TLS，无 OpenSSL/UPX/GPL/nonfree）

| 产物 | 字节 | SHA-256 | NEEDED |
|---|---|---|---|
| `build/android-v7a/single-slimff/libmsplayer.so` | 2,199,000（比 2 MiB 多 101,848） | 33ed39fbf70fdfbe0ca705f150577c639d93e3f843854310d8b81ca173a9c1ec | android, log, z, m, dl, c |
| `build/android-v8a/single-slimff/libmsplayer.so` | 2,315,888（比 2 MiB 多 218,736） | a0954bc082462b9d8ceff296d64ab3e7f58b184addf61803da9d4791d0f928a7 | 同上 |
| `build/android-v7a/single-native-s2/libmsplayer.so` | 209,856 | 7dcf3123bcb566f5… | android, log, m, dl, c |
| `build/android-v8a/single-native-s2/libmsplayer.so` | 278,608 | ceb9a181c1b85264ee8d48fc80a421110e28c0c173e31673a17370b4b66dde02 | 同上 |

四个 so 都只导出 59 个符号：`JNI_OnLoad` 和 `Java_com_msmedia_*`，版本 `MSMEDIA_SINGLE_1.0`。用户已放宽到“多几百 KB 没关系”，所以超出 2 MiB 只记录不阻断（`STRICT_LIMIT=1` 才会失败）。

v8a 逐项体积：

| 步骤 | 字节 | 增量 |
|---|---|---|
| 去 asf/rm 前（noadec） | 1,834,984 | |
| 去 asf、rm demuxer 及专属的 rtpdec_asf / rdt 入口 | 1,796,656 | −38,328 |
| + ac3、eac3、mp2 | 1,936,144 | +139,488 |
| + pcm_alaw、pcm_mulaw | 1,939,040 | +2,896 |
| + dca、dca parser、dts demuxer | 2,135,480 | +196,440 |
| + aac_latm（连带 aac 解码器） | 2,315,408 | +179,928 |
| 起播优化后的最终版 | 2,315,888 | +480 |

v7a 的 noadec 从 1,708,780 降到 1,668,460（−40,320）。

LATM 评估：自研 LOAS→raw AAC 解包后交给系统 MediaCodec，约可省 180 KB，本轮未实现，作为后续可选优化。当前用 FFmpeg `aac_latm` 软解求稳。音频策略仍是系统优先：MediaCodec 能解（AAC/MP3/AC3/EAC3/G.711/DTS 视机型）就走系统，否则在 single-slimff 内用 FFmpeg 软解。

### 2. MediaPlayer 接线

- `com.msmedia.MediaPlayer` 与系统 API、常量、7 个 listener 对齐，同一份 Java 可加载任一 so（`isFfmpegRoute()` 可查）。
- 时间单位全部是毫秒，90 kHz 不出 so。
- `prepare` 对本构建播不了的输入诚实失败（-1010/-1004/-110）。
- 非标准诊断接口：`getStartupTimingsUs()`，取自 `nativeStartupStats`，以 request 为零点，单位 µs。
- 已用 javac（android-34）编译通过；运行期行为未在设备上验证。

### 3. 起播（TTFF）与单次建连

- 宿主 HTTP 审计：`tests/single_connect_server.py`，结果在 `build/single-analysis/s2-host/single_connect.txt`。
- 修复了首帧前多出的 HTTP 请求，只在 `MS_SINGLE_SO` 下生效，官方多 so 行为不变：
  - MPEG-TS：网络输入设 `skip_estimate_duration_from_pts`。原先 find_stream_info 为算时长会 seek 到文件尾再回头，首帧前多两次 Range（服务端不保活时就是重连）。现在时长改用码率估算。
  - MP4：主输入 HTTP 打开时缓存文件头 4 KB（AVIO 缓冲内回绕，不产生新请求），fMP4/QT 判别不再 `SEEK_SET 0`。
- 修复后，首帧前请求数：ts、flv、mp4（faststart）、mkv 都是 1 次 GET、1 条连接。moov 在尾部的 mp4 需要 2 次，结构所决定，系统播放器同样如此。
- 宿主起播拆分（mock 解码器，ASan -O1，本地文件），按 request 起算：

| 样本 | A 打开 | A 首包 | A 首关键帧 | A 首次送解码 | A 首输出 | B 打开 | B 首包 | B 首关键帧 | B 首输出 |
|---|---|---|---|---|---|---|---|---|---|
| h264_aac.mp4 | 0.23 ms | 0.66 | 1.03 | 1.08 | 1.09 | 2.17 | 2.04 | 2.37 | 2.40 |
| h264_aac.ts | 0.29 | 0.86 | 1.15 | 1.19 | 1.19 | 2.69 | 2.57 | 3.11 | 3.12 |
| h264_aac.flv | 0.23 | 0.42 | 0.76 | 0.80 | 0.81 | 3.01 | 2.87 | 3.24 | 3.26 |

  - B 的时间主要花在 avformat_open_input 加 find_stream_info（已用 FAST_START：probesize 512 KB、analyzeduration 0.4 s）。
  - 首关键帧到送解码约 10–40 µs，MediaPlayer 外壳的轮询是 15 ms。
  - 与系统 MediaPlayer 的同机对比**未做**，需要真机。

### 4. 双线覆盖对比（同一组样本，宿主 mock）

- 路线 A（纯自研）：
  - 能解：mp4（仅 H.264+AAC；`hevc_aac.mp4` 无视频输出）、ts（H.264+AAC，以及 DVB 字幕）、flv。
  - 无音频：ts 内的 ac3/eac3/mp2/dts/latm。
  - 不支持：mkv、wav、mp3、flac。
  - seek 和循环只在 mp4 上可用。
- 路线 B（slimff）：全部样本都能打开；pause/resume、seek 50%、循环回 0 均通过（宿主用的是官方 libmsff 加上单 so 变体，不是 Android slimff 本体）。
- 宿主异常说明：
  - `h264_dts.ts` 在宿主被识别为 mp3。原因是宿主官方 FFmpeg 没有 dts demuxer 和 dca parser（`CONFIG_DTS_DEMUXER 0`），mpegts 按内容探测时误中 mp3。Android single-slimff 已启用这两项。noadec 变体同样缺这两项，不应作为交付。
  - LATM 在宿主显示 unknown：宿主不带 `MS_SINGLE_FFADEC`，属预期。
- 30 次对比全部 rc=0，ASan/UBSan/LSan 错误为 0。

### 5. 测试

- 官方测试 `env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh test`：EXIT:0（`build/single-analysis/official-test-s2-final.log`）。
- 宿主 ASan/UBSan/LSan 下：DVB 端到端 DVB_E2E_PASS，netcore 25 项 NETCORE_PASS，双线对比与 HTTP 单连审计 sanitizer 错误均为 0。

### 6. 未验证

- 真机和模拟器：硬解首帧、TTFF、与系统播放器对比。
- 真实 TLS 握手，以及真实 SOCKS5 代理。
- 音频实际出声，包括各软解的出声与降混。
- API 18 实机加载：只做了 readelf 静态检查。新增导入有 inet_pton、strcasecmp、cbrt、exp2f、ldexpf、powf、lrint、lrintf。
- MediaPlayer 运行期行为。
- 路线 B 在设备上的 DVB 字幕。
- DTS/LATM 在 Android slimff 内的实际识别与解码。
- 网络 TS 跳过 PTS 估时长后的时长精度（改为码率估算，可能有偏差）。


## 阶段 3 收尾 + 阶段 4（2026-11-24 13:25 Asia/Shanghai 交付）

阶段 3 收尾：按用户要求不再跑模拟器轮次。阶段 3 包为 `build/single-apk-s3/`（11:28，保留不动）；阶段 3 起播优化（MPEG-TS 网络输入不再 seek 文件尾，首帧前 1 次 GET、1 条连接）见上文。

阶段 4 交付目录：`/workspace/MsMedia-deliver-20261124-1325`（含 SHA256SUMS.txt）。下面全文同交付 README：

### 1. 产物（-Oz + ThinLTO，NDK r29，minSdk 18）

| 路线 | ABI | 字节 | SHA-256 | NEEDED | 导出 |
|---|---|---|---|---|---|
| pure | armeabi-v7a | 261,272 | f1cc1be6d0655f21904085888db1b1bf234be7df91182f71a45877bc2778eaba | libandroid, liblog, libm, libdl, libc | 62 |
| pure | arm64-v8a | 336,384 | aae02d195d5c0588b3b3eeff0c4b3c2119377c1f234d10aa7284fe32f7ff66d9 | 同上 | 62 |
| pure | x86 | 379,928 | c7ed70bc0613b06181caf6b20b2d2548443a0a3a83dd408b2e26b28f7850cce3 | 同上 | 62 |
| pure | x86_64 | 360,824 | 058843bba4982490bdc87b112362f5899fd0182ac51d9dce1a2fd90740855a06 | 同上 | 62 |
| slimff | armeabi-v7a | 1,922,708 | f6d6ba23ce864efaa55cab057e29c2598cd44049f770b3899756cfe1a0a655f1 | libandroid, liblog, libz, libm, libdl, libc | 62 |
| slimff | arm64-v8a | 1,936,552 | 2e73813e8ae450234c04d0b5d3264b07f7380646e1f4bb47de774bf23e1b073c | 同上 | 62 |
| slimff | x86 | 2,051,836 | 230917940dbdda3aa3d9bc51a3dbc15a5dacb2bd81fdfd3f776e72ba59ba243b | 同上 | 62 |
| slimff | x86_64 | 2,039,240 | a3a045c0a8902bc727c8a6a58b67acc2468a882c5007c85d21cc6dbb272762ba | 同上 | 62 |

导出符号：只有 `JNI_OnLoad` 和 `Java_com_msmedia_*`，版本节点为 `MSMEDIA_SINGLE_1.0`，内部 C 符号全部隐藏。本阶段新增 3 个导出：

- `MediaPlayer.nativeSetPreload`
- `MediaPlayer.nativeSetAudioPassthrough`
- `MediaPlayer.nativeGetAudioPassthroughRoute`

`apk/` 中是调试签名的测速包：

| 文件 | 字节 |
|---|---|
| MsSpeed-pure-release-arm.apk | 643,625 |
| MsSpeed-pure-debug.apk | 1,393,332 |
| MsSpeed-slimff-release-arm.apk | 3,908,137 |
| MsSpeed-slimff-debug.apk | 8,000,180 |

包名分别为 `com.msmedia.single.pure` 和 `com.msmedia.single.slimff`，可以同机安装做对比。

### 2. 体积与裁剪对照

阶段 3 与阶段 4 的单 so 对比如下：

| 路线 | ABI | 阶段 3（-Os） | 阶段 4（-Oz+ThinLTO） | 差值 |
|---|---|---|---|---|
| slimff | v7a | 2,204,296 | 1,922,708 | −281,588 |
| slimff | v8a | 2,322,640 | 1,936,552 | −386,088 |
| slimff | x86_64 | 2,396,280 | 2,039,240 | −357,040 |
| slimff | x86 | 2,236,552（noxml、无 DASH） | 2,051,836（含 DASH） | −184,716 |
| pure | v7a | 214,360 | 261,272 | +46,912（新增 netsrc/协议/MKV/PS/容错/直通） |
| pure | v8a | 284,512 | 336,384 | +51,872 |

slimff 的裁剪内容：

- **XML**：去掉 libxml2，改链自研 ms_xml 垫片。二进制里 libxml2 特征串数量从 5 降到 0。
- **去掉的 demuxer**：ogg、microdvd、vobsub；asf、rm 在更早的阶段已去掉。
- **去掉的协议**：rtmpt、srtp。
- **本来就没开、本次加了构建断言的**：image2、imf、jacosub、mpl2、sami、realtext。

slimff 保留的内容：

- **全套音频软解**：aac、aac_latm、ac3、eac3、mp2、pcm_alaw、pcm_mulaw、dca。
- **常用字幕所需的 demuxer**：mpegts、matroska、mov、webvtt、srt、ass、hls、dash。DVB、PGS、mov_text 走自研字幕路径。

### 3. 新增能力（阶段 4）

两条线共用的部分：

- **伪装头剥离**：用同一份 `ms_probe` 识别并剥掉图片伪装头（PNG/JPEG/GIF/BMP/WebP）和前导脏字节。
  - 路线 B 本地/HTTP 渐进式输入：用位置重映射的 AVIO 包装，所以 MP4/MKV 的绝对偏移仍然正确。
  - 路线 B HLS 切片：由 hls.c 补丁处理。

路线 A 新增：

- **网络输入**：HTTP 渐进式、HLS（支持 TS、伪装切片、fMP4 `EXT-X-MAP` + `.m4s`、主清单）、DASH、UDP/RTP 组播、RTMP/RTMPS、RTSP（TCP 交织，支持 Basic/Digest 认证；载荷支持 H264/H265/AAC/MP2T）。
- **预加载线程**：提前缓存时长和内存上限都可配置；Keep-Alive 复用连接；取消即时生效。
- **转封装**：本地/网络的 MKV、MPEG-PS（含 GB28181 风格、VOB）转为 TS 或 FLV。
- **TS 容错**：坏包过滤，33 位 PTS 回绕可容忍。

Java/JNI 新增 API：

- **预加载配置**：`MediaPlayer.setPreloadConfig(aheadMs, maxBytes)`。
- **音频直通**（对齐 Media3）：
  - 接口：`setAudioPassthroughEnabled` / `isAudioPassthroughEnabled` / `getAudioPassthroughRoute` / `getAudioPassthroughCaps(Context)`。
  - 能力探测：API 29+ 用 `AudioTrack.isDirectPlaybackSupported`；API 21–28 用 HDMI 粘性广播 `EXTRA_ENCODINGS`（Amazon 设备另读 `external_surround_sound_enabled`）。高版本 API 都放在嵌套类里并用 `SDK_INT` 守卫。
  - 输出方式：原始 ENCODING_AC3/E_AC3/DTS，或者 IEC61937 封装（E-AC-3 用 4 倍采样率）。
  - 回退：设备不支持或 AudioTrack 打不开时自动回退软解，不会静音。
  - 封装正确性：AC-3、E-AC-3、DTS 的 IEC61937 输出与 FFmpeg `-f spdif` 逐字节一致（差异 0 字节）。

### 4. 质量验证

### 4.1 官方回归

命令：`env -u CC -u CXX -u LD -u FFMPEG_REBUILD PATH=/usr/bin:/bin ./build.sh test`

- 第一次运行 EXIT 1。原因是单 so 新增的 Enhanced FLV HEVC 解析违反了官方契约。
- 已用 `MS_SINGLE_SO` 隔离，官方多 so 保持原来“诚实报不支持”的行为；测试期望没有改动。
- 修复后 EXIT 0，在最终源码上复跑也是 EXIT 0，`FAIL` 计数为 0。
- 官方 `android_hw_build.sh` 的源码集合用官方参数编译通过，没有新增未定义的 ms_* 符号。

### 4.2 宿主 ASan + UBSan + LSan

`tests/single_s4_quality.sh` 结果为 QUALITY_PASS fails=0（12:50:35 CST）：

| 项 | 结果 |
|---|---|
| passthrough | PASS，覆盖路由/回退、帧采样数、IEC61937 突发、坏帧 |
| probe 伪装剥离 | 48 个用例 0 失败，模糊测试 20000 次 |
| ms_xml 垫片 | 与 libxml2 逐字节差分一致，变异模糊测试通过 |
| 高频 open/close + 随机 0–59 ms 取消 + 13 个 URL 轮换切台 | 300 个生命周期：opened 300、eos 104、canceled 192、errors 0，close 最大 101 ms |
| 6 线程并发生命周期 | 240 个：opened 240、errors 0，close 最大 99 ms |
| 畸形本地文件（101 个） | 0 字节、1 字节、噪声、超大 box、超大 EBML/PS、坏 PNG/JPG 头、5 档截断、4 档变异。全部可打开，单个输入最长 3004 ms，close 最大 17 ms，无崩溃、无死锁 |
| 畸形网络（30 个） | 详见下方说明。全部以错误收尾，最长 203 ms，无崩溃、无挂死 |
| netsrc 全量回归 | 39 例 NETSRC_PASS fails=0 |

畸形网络的 30 个输入包括：

- 中途断连（`/cut/*`）；
- 伪造超大 Content-Length；
- 自循环、超大或二进制 m3u8；
- 坏 `EXT-X-MAP`；
- 2 万层嵌套的 MPD、超大 MPD、截断 MPD；
- 8 种畸形 RTMP：超大消息长度、chunk size 0x7fffffff、扩展时间戳风暴、半截断开；
- 8 种畸形 RTSP：超大 Content-Length、无 CRLF 的 70 KB 头、随机 interleaved 帧、坏 SDP。

所有项目 Sanitizer 均为 0 报告：0 泄漏，0 越界，0 UB。

测试中发现并修复了两个问题：

- 压测驱动曾在 close 之后才 cancel，属于测试本身的用法错误；现已与 JNI 的“先置空再 close”契约一致。
- 路线 B 的 JNI 残留了对 `ms_netsrc_cancel` 的引用，导致链接失败；已加上 `!MS_HW_HAVE_FF` 守卫。

### 5. 模拟器三方对比（x86 TCG 软件模拟，毫秒数偏大，只用于同环境横比）

### API 34（emulator-5554）

每个 URL 跑 3 轮，数字为中位数（prepared / 首帧，毫秒）：

| 输入 | 系统 MediaPlayer | A 纯自研 | B 精简 FF |
|---|---|---|---|
| HTTP TS | 6771 / 10348 | 801 / 3581 | 874 / 5983 |
| HTTP MP4 | 4378 / 8051 | 643 / 3807 | 768 / 4240 |
| HTTP HLS | 3126 / 6124 | 717 / 3996 | 1111 / 6472（2/3 成功，1 次 IO −1004） |
| 本地 TS / MP4 / FLV / MKV | 不可比（mediaserver 无权读 /data/local/tmp，IOException） | 484/4005、650/4440、684/5598、688/4925 | 672/4578、676/4782、822/4837、962/4031 |

生命周期测试：seek 一半 → 暂停 → 恢复 → 释放。两条线各 6/6 输入 `MSLIFE OK`，首帧如下：

| 输入 | A 纯自研 | B 精简 FF |
|---|---|---|
| png_mkv | 2689 | 3685（本次修复前 B 为 −1004） |
| gb_aac.ps | 3609 | 4749 |
| h264_ac3.ts | 2297 | 5060 |
| fMP4 HLS | 4061 | 4966 |
| h264_aac.mkv | 11191 | 11537 |
| HTTP TS | 2726 | 5787 |

日志中没有本应用的崩溃、ANR 或 Fatal signal。出现过的 Fatal 来自 youtube 和 bluetooth 系统进程，与本应用无关。

### API 18（emulator-5556，Dalvik）

- `MSAPI sdk=18 pt_caps=0x0`，没有 VerifyError、NoSuchMethodError、NoSuchFieldError 或 UnsatisfiedLinkError。只有一条预期的软校验告警（PlaybackParams 被替换），属于既有情况。
- 两条线的本地输入全部能 prepared，耗时 48–162 ms。
- 首帧全部超时，系统播放器也一样失败。原因是该 AVD 的 `OMX.google.h264.decoder` 报 ERROR(0x80001001)，属于环境限制。
- 收尾时 api18-slimff 复测还在运行，已记录 23 条，每条都已 prepared 或报错，没有崩溃。

### 6. 接入替换

1. 把 `pure/lib/<abi>/libmsplayer.so` 或 `slimff/lib/<abi>/libmsplayer.so` 放进 `jniLibs`，两者二选一。
2. 加入 `com.msmedia.MediaPlayer` 和 `MsNativeLoader`、`MsSystemTls`、`net/MsHttpClient` 这几个 Java 文件。
3. 把 `android.media.MediaPlayer` 换成 `com.msmedia.MediaPlayer`，接口与系统版一致。
4. 可选扩展：`setPreloadConfig(10000, 16<<20)`、`setAudioPassthroughEnabled(true)`，都在下一次 prepare 时生效。

### 7. 真机待验清单（本机无法验证）

- 海思等真机上的 AVS+ 硬解出画。宿主只用了合成样本。
- HDMI/光纤功放的 AC-3/E-AC-3/DTS 源码透传点亮：功放面板要显示杜比/DTS 标识，并要求无静音、无爆音。模拟器 `pt_caps=0`，只验证了回退软解这条路。
- 真实外网 <1 s 首帧，以及真机 TTFF 与系统播放器的对比。模拟器是 TCG，数字只能横比。
- 真机上的 API 18–20、扬声器听感，以及 E-AC-3 IEC61937 下 4 倍采样率时钟在功放上的同步。
- 路线 A 已知限制：
  - HLS 不支持 seek；
  - 重定向后相对 URI 按原 URL 解析；
  - TS 内的 AC3/EAC3/DTS/MP2/HEVC 在纯核心中不成 AU，直通只在路线 B 生效；
  - 不支持 G.711；
  - 未实现 EIA-608 CC 解码，两条线都没有实现。

## 2026-11-24 15:43 Asia/Shanghai — 阶段 6：路线 B 主力单 so = 精简 FFmpeg s6 + 裁剪 OpenSSL 3.5.9 静态 + Media3 风格 HLS

by: Long <mycjzb@gmail.com>。审计见 `docs/DESIGN_AUDIT.md`。s5 产物（`single-slimff-adec-s5`）未动，`scripts/single_s5_verify.sh` 仍 ALL PASS。

**OpenSSL**（`scripts/openssl_slim_build.sh` + `scripts/openssl_slim_patch.py`，源码副本 `build/openssl-3.5.9-slim-src`，原缓存只读）：
TLS 1.2/1.3 纯客户端；ECDHE P-256/P-384 + RSA/ECDSA 证书 + AES-GCM/CBC + SHA-1/2/HKDF。关掉 DTLS/QUIC/SSL3/TLS1.0/1.1、DH、ChaCha20-Poly1305、X25519/X448/Ed25519（`no-ecx`）、全部冷门算法、PQC、CMS/OCSP/CT/HTTP/sock/stdio/err 文案、engine/dso/legacy；default provider 去掉编码器/存储/KEM/SKEYMGMT 表（`MS_OSSL_TLS_CLIENT_ONLY`）。`-Oz -flto=full -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -DOPENSSL_SMALL_FOOTPRINT`，`no-asm`。不用 mbedTLS，不用 UPX。

**TLS 隔离**：netcore TLS 泵 `src/netcore/ms_nc_tls_ossl.c`，两个缓存 SSL_CTX：严格（系统 CA 目录 `/apex/com.android.conscrypt/cacerts` → `/system/etc/security/cacerts`，SSL_set1_host 主机名/IP 校验）与不校验。播放器（FFmpeg tls → `tls_android.c` → `ms_nc_tls_open2(host, verify)`）默认 verify=0，Java `setPlaybackTlsVerify(true)` 切严格；`MsHttpClient` 走 `ms_nc_tls_open`，永远严格。证书失败报 EPROTO，不进瞬时重试。

**体积（strip 后，字节；zip -9 估算 APK 压缩后）**

| ABI | s5 | s6 | 增量 | s5 压缩 | s6 压缩 | 压缩增量 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| armeabi-v7a | 1710844 | 2794244 | +1083400 | 989099 | 1597445 | +608346 |
| arm64-v8a | 1649200 | 3148136 | +1498936 | 963082 | 1609319 | +646237 |
| x86 | 1790248 | 3205768 | +1415520 | 1004418 | 1666680 | +662262 |
| x86_64 | 1777784 | 3409464 | +1631680 | 965348 | 1637631 | +672283 |

增量几乎全是 OpenSSL（预下载器约 5–10 KB；AES 复用 s5 已有的 libavutil `av_aes`，0 字节）。段级增量（v7a / v8a / x86 / x86_64）：`.text` +677K/+793K/+1000K/+920K，`.rodata` +175K/+179K/+183K/+184K，`.data.rel.ro` +123K/+223K/+123K/+224K，重定位表 +96K/+286K/+96K/+286K（API 18 不能用 RELR/APS2 压缩重定位，64 位每条 24 字节）。
APK 内 .so 为 Stored（不压缩）：`MsSpeed-slimff-release-arm.apk` 5993001 字节，`MsSpeed-slimff-debug.apk` 12612276 字节。
NEEDED：libandroid/libc/libdl/liblog/libm/libz；导出 64（s5 的 62 + `nativeSetBufferConfig` + `nativeSetTlsVerify`）；无 RELR。`scripts/single_s6_verify.sh` 4 ABI ALL PASS（376 项）。

**Media3 风格 HLS**：`src/ff/ms_hls_pf.{h,c}` + `scripts/slimff_patch_s6_hls.py`（hls.c，`MS_SLIM_S6`）+ `ms_ff_set_buffer_config` / JNI / Java `setBufferConfig`。默认 15000/30000/1500/3000 ms、16 MiB。-1004 修复：`open_input_with_retry`（300/700/1500 ms）。

**测试**
- 宿主 `tests/single_s6_hls_test.sh`（ASan+UBSan+LSan）29/29：TS/伪装 png/fMP4 MAP/伪装 jpg/BYTERANGE/AES 显式 IV/AES 序号 IV/Keep-Alive/直播/水位/关预下载/弱网断点续传（retry=3）/弱网 AES/慢首片/404/伪装 MKV/伪装 TS/打开中取消 14 ms/播放中取消 6 ms/seek/seek AES/30 轮切台/15 轮弱网 AES 切台/自签 HTTPS 播放器放行（含 AES）/播放器严格拦截/MsHttpClient 严格拦截/带 CA 放行/主机名不符拦截。修掉 ASan 发现的预下载线程 UAF。
- `./build.sh test`：EXIT 0。
- CJZB_API34：bench（sys,ms ×5）MsMedia 40/40 OK：HTTP TS、HTTP MP4、HLS（18190）、AES HLS、自签 HTTPS HLS、自签 HTTPS AES HLS、伪装 MKV、伪装 png HLS；MsMedia 0 次 -1004（-1004 全部来自系统播放器播自签 HTTPS）。App 内 MsHttpClient：自签 HTTPS 拦截、HTTP 200、google.com 严格校验 200。生命周期（seek/暂停/恢复/释放）10/10 MSLIFE OK。原生单测（x86_64）全部通过；外网 HTTPS 偶发 `SSL_R_WRONG_VERSION_NUMBER`，是本机出口代理返回非 TLS 数据（同时段 HTTP 也返回代理 500），非证书问题。
- CJZB_API18：原生单测（x86，同一批对象）16/16 符合预期，含严格校验访问 google.com 200、自签拦截、AES、取消 33 ms；APK 生命周期 prepare 全部成功、无崩溃，首帧超时与 s4 相同（模拟器 4.3 无解码输出，需真机）。

**未做 / 限制**：Java MediaPlayer 未开放 getTrackInfo/selectTrack/TimedText；SAMPLE-AES/DRM 不支持；字节上限可被在途一片超出；真机项见 `docs/DESIGN_AUDIT.md` 第 6 节。
