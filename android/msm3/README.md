# MsM3：演示 APK（调用 SDK，不内嵌 SDK 源码）

## 分工

| 模块 | 路径 | 职责 |
|---|---|---|
| **SDK** | `android/msplayer`（`:msplayer`） | Media3 Java 源码 + `MediaPlayer` + so；**release 已 R8 裁过再交付**（HLS/DASH 保留） |
| **Demo** | `android/msm3/app` | 只 `implementation project(':msplayer')` |

## 编译 / 安装

```bash
# 一键：so + UPX + SDK AAR + demo APK
cd /home/long/code/MsMedia && bash scripts/build_msm3.sh -va both
bash scripts/build_msm3.sh -va v7a -adb 192.168.0.166
# 已有 so 只打 Java：加 -skip-so
```

## 测试频道

只读包内 `app/src/main/assets/channels.txt`（`台名,URL`）。改列表后重新打 APK。换台抄超级直播（频道键/方向键，demo 上下反转），无底栏按钮。

### 内置 JS 脚本

样例在 `assets/js/`（与仓库 `samples/js/` 同步）。频道 URL = **脚本绝对路径 + 查询串**：

```text
asset:///js/yspweb.js?id=cctv1
asset:///js/hntv_live.js?id=hnws
asset:///js/mgtv_live.js?id=hndsj
asset:///js/fhx_live.js?id=fhzw
asset:///js/sdtv_live.js?id=sdws
asset:///js/gxtv_live.js?id=gxws
```

`id` 由脚本 `ms.getQuery` 读。广西走 `stream_call` 解 PES。包内 `asset:///js/…`；线上同形态 http(s) 绝对地址。

改脚本：先改 `samples/js/`，再 `cp samples/js/*.js android/msm3/app/src/main/assets/js/`，重打 APK。
