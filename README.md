# MsMedia

by: Long <mycjzb@gmail.com>

自研播放内核。核心使用 C（C11），对外稳定 C ABI，供 Android、Windows、Linux、iOS 共用。网络取流、协议插件、硬解、渲染、时钟调度都在核心之外。

当前进行到 **P1i**：在 P1h 余数时间戳与 TS 连续性之上，修复中途接入 H.264 等待 SPS/PPS/IDR 的安全恢复，以及 evq 严格 FIFO。已实现子集为 MPEG-TS + AAC ADTS + 逐行 H.264 Annex B（AUD 或 ITU-T H.264 7.4.1.2.4），不是全部 H.264/AAC，也不是原生网络/HLS 模块。

宿主开发自测（`./build.sh debug` 带 ASan/UBSan，以及 `./build.sh release`）当前为 31 组核心测试通过。这不等于正式发版，也不等于 el7/四平台或硬解出画验收，也不把开发解析阶段写成全部完成。有界实流抽样中，中途接入的 A/B 视频 AU 已与 ffprobe 自第一个关键包起的包数一致；音频相对 ffprobe 各少 2 帧，原因尚未单独定因。不得把抽样写成硬解或出画成功。

公开仓库：<https://github.com/mycjzb/MsMedia>

## 现在可以读什么

| 文档 | 内容 |
| --- | --- |
| [docs/DESIGN.md](docs/DESIGN.md) | 边界、C ABI、格式与编码矩阵、硬解、字幕、seek、AI 字幕 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | P0–P5 优先级、P1a 起步范围和验收门槛 |
| [docs/PROGRESS.md](docs/PROGRESS.md) | 真实进度、证据、阻塞和下一步 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 小步修改、回归和注释规范 |
| [SECURITY.md](SECURITY.md) | 日志、畸形输入和漏洞报告口径 |
| [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) | 第三方许可（当前没有第三方代码） |
| [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) | 兼容性参考归类。全部未实现 |

## 编译

唯一入口是根目录 `build.sh`：

```bash
./build.sh              # 默认
./build.sh debug        # 可用时带 ASan/UBSan
./build.sh release      # 优化并 strip
./build.sh release upx  # 只对可执行文件调用 pack，不对库做 UPX
./build.sh clean
```

开发自检：`./build.sh debug && ./build/debug/test_core`

正式 C 发版应在 CentOS 7 隔离容器中进行。宿主机编译不能代替四平台或 el7 验收。

## 许可证

许可证**尚未选定**。本仓库不放置 LICENSE 文件，也不把某一种开源许可证写成已经生效。用户明确要求提交时可以推送实现，不把尚未选许可证当作该次提交的阻塞。

## 作者

by: Long <mycjzb@gmail.com>


## 开发交叉验证（可选）

产品库不依赖、不链接 FFmpeg。若宿主机已有 `ffmpeg`/`ffprobe`，可在 `./build.sh debug` 之后运行：

    tests/dev_crosscheck.sh

它会在 `build/dev-samples/` 即时生成短小 lavfi 样本（不入库），并用 `ms_ts_inspect`（公共 ABI）对照 ffprobe/独立 PES 重组。该脚本不是 `./build.sh` 入口，也不作为正式验收。
