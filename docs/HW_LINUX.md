# Linux VAAPI 探测与码流 profile 映射

by: Long <mycjzb@gmail.com>

日期：2026-11-22 05:18 CST（Asia/Shanghai）。

本文件记录本轮在 Bot 宿主上可实际验证的一块 Linux 硬解前置工作。**不是设备硬解出画验收，也不是四平台或 el7 验收。**

## 本机真实环境（只读核查，未提权、未启动未知服务）

| 项 | 本机事实（2026-11-22 05:00 CST） |
| --- | --- |
| `/dev/dri` | 不存在，无 render/card 节点 |
| `/dev/video*`、`/dev/nvidia*` | 不存在 |
| `libva.so.2` / `libva-drm.so.2` | 已安装（2.22.0-3），**无** `libva-dev` / `vainfo` / `<va/va.h>` |
| Windows 交叉 | 无 `x86_64-w64-mingw32-gcc`，无 Media Foundation / D3D11 SDK |
| iOS / Apple | 无 Xcode、osxcross、VideoToolbox 头 |
| 容器 | 无 `docker` / `podman` / `nerdctl`，无 `/var/run/docker.sock`，无 `/ms`，无 `ms-build-c7` 镜像。**不能把宿主编译当 el7。** 未用 docker socket 绕过权限。 |
| `/dev/kvm` | 节点存在（`crw-rw---- root:pulse-access`），当前用户不在该组、无权限；**不得启动共享 AVD** |

因此本轮只做 Linux 这一块可编译、可测试的实质闭环：码流参数 → VAProfile 映射，以及运行时 `dlopen` 后的 DRM/VAAPI 探测。Windows / iOS 未堆未编译代码。未扩格式范围。

## 设计约束与许可证

遵循 `docs/DESIGN.md` 第 6 节：Linux 目标后端为 VAAPI / V4L2 或厂商设备；**打开到硬件设备并查到 profile 才算硬解**；没有设备则不支持。`is_hardware=1` 还必须关闭软解回落。本构建另加一条更严的诚实边界：即便将来在有设备的机器上 `vaInitialize` 成功，**在 slice/picture 提交与呈现实现之前，`decode_implemented` 与 `is_hardware` 必须为 0**。不得只按 MIME / `ms_codec` 枚举声称支持。

公开 API 来自 Intel libva 2.22.0 的 `vaGetDisplayDRM` / `vaInitialize` / `vaQueryConfigProfiles` / `vaQueryConfigEntrypoints` 等（MIT）。本仓库只保留最小 ABI 子集 `src/hw/ms_hw_vaapi_libva.h`（含 Intel 版权声明），运行时 `dlopen("libva.so.2")` 与 `dlopen("libva-drm.so.2")`。`libmshw` **不 NEEDED** `libva` 或 `libdrm`。未把 libva 源码纳入仓库，未链软件 decoder，未使用 FFmpeg 软解。

libva 2.22 无 AVS+ profile：广播类 `profile_id=0x48` 可解析完成，但 `mapped=0`，不得假装有 VA 硬解。

## 已实现（源码 + 宿主编译 / 测试）

- `ms_hw_vaapi_parse_stream`：从真实 SPS / PTL / sequence_extension / VOS / 短视频头 / av1C 或 SH OBU 读取 profile、constraint、level、chroma、bit depth，再映射到与 libva 2.22 一致的 `VAProfile` 数值。未约束 Baseline 不静默改写成 Constrained Baseline；High 4:2:2 等无对应 profile 时保持未映射。
- `ms_hw_vaapi_probe_device`：扫描 `/dev/dri/renderD*` 与 `card*`，打开后 `vaInitialize` 并查询 profile 与 VLD entrypoint。无目录、打不开或初始化失败时 `have_device=0`，`verified=1`，`is_hardware=0`。
- 默认 `ms_hw_decoder_query`（非 Android）与 `mock_mode=0` 的 `configure` 走上述探测与解析：配置先解析 CSD，再按设备/映射/「提交未实现」返回 `UNSUPPORTED`，并把解析到的 profile 写入 `detail`。`MS_HW_MOCK_*` 状态机路径保持独立，不与 VAAPI 探测混称。
- 公共加法 API 见 `include/msmedia/ms_hw.h`（`ms_hw_vaapi_*`）。`MSMEDIA_HW_ABI` 仍为 1。Android 构建编入同一源文件：解析可用，探针对该平台返回 `backend=none`（VAAPI 未按该平台设备路径构建）。

## 本宿主实测（不是出画）

`test_hw_vaapi`（官方 ASan/UBSan 矩阵，2026-11-22 05:17 CST）：

- 夹具 avcC Constrained Baseline、手工 Baseline / Main / High / High10、HEVC Main、MPEG-2 Main、MPEG-4 Simple、H.263 Baseline、AV1 Profile0 均可映射。
- High 4:2:2、H.263 PLUSPTYPE、AVS+ 广播类：完整解析但未映射；Jizhun（0x20）拒绝为非 AVS+；空缓冲 / 非 SPS 不得按 MIME 映射。
- 探测：`libva_loaded=1`，`have_device=0`，`decode_implemented=0`，`is_hardware=0`，说明含 `no /dev/dri`。
- 默认 configure 返回 `UNSUPPORTED`，`detail` 含 `H264ConstrainedBaseline`；`MS_HW_MOCK_AS_HW` 仍走 mock，并标明 `not a device`。

## 明确未做

- VAAPI slice / picture 提交、surface / DMABUF 呈现、同步与错误恢复。缺 GPU 不等于不能先写 fail-closed 的真实 submit（解析得到的 picture/slice 参数、表面池、无设备时保持 `UNSUPPORTED` / `is_hardware=0`），但尚未编写，不得用未验证伪实现对账。
- V4L2 或其它厂商设备路径。
- Windows Media Foundation / D3D11、iOS VideoToolbox（本机无相应工具链，本轮不堆未编译代码）。
- 在有 GPU 的 Linux 机器上打开设备并查出 profile 的验证；本箱无 DRM 节点。
- 把 `UNSUPPORTED` 或「源码已写」写成硬解完成。
