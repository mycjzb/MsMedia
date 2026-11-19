# MsMedia

by: Long <mycjzb@gmail.com>

自研播放内核的设计草稿。核心使用 C（C11），对外稳定 C ABI，供 Android、Windows、Linux、iOS 共用。网络取流、协议插件、硬解、渲染、时钟调度都在核心之外。

**当前阶段是设计审阅，没有已批准的实现。** 仓库里的 Markdown 是待确认初稿。识别清单、容器清单和硬解路径都是目标，不是已完成能力。没有播放器界面。

公开仓库：<https://github.com/mycjzb/MsMedia>

## 现在可以读什么

| 文档 | 内容 |
| --- | --- |
| [docs/DESIGN.md](docs/DESIGN.md) | 边界、C ABI、格式与编码矩阵、硬解、字幕、seek、AI 字幕 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | P0–P5 优先级、依赖和验收门槛 |
| [docs/PROGRESS.md](docs/PROGRESS.md) | 真实进度、证据、阻塞和下一步 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 小步修改、回归和注释规范 |
| [SECURITY.md](SECURITY.md) | 日志、畸形输入和漏洞报告口径 |
| [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) | 第三方许可（当前没有第三方代码） |

## 许可证

许可证**尚未选定**。本仓库不放置 LICENSE 文件，也不把某一种开源许可证写成已经生效。选定前不公开发布实现代码。

## 作者

by: Long <mycjzb@gmail.com>

这是作者明确要求写入项目的公开署名。后续源文件在合适位置使用同一行署名。
