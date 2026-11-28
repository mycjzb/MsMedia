# JS 解析脚本样例（每站一个文件）

业务逻辑对照超级直播台源；宿主同时挂 `ms` / `ku9` / `cjs`（参数兼容）。推荐只写 `ms.*`。

| 文件 | 用途 | 频道地址例 |
| --- | --- | --- |
| `web_page.js` | 网页里抠首个 m3u8/flv | `…/web_page.js?page=https://站点/直播页` |
| `yy_live.js` | YY 直播 | 脚本内解析 yy.com / `yyloop://`，或 `?` 透传 |
| `gdy_tv.js` | 广电云 | `…/gdy_tv.js?page=https://web.guangdianyun.tv/tv/?id=&uin=` |
| `mgtv_live.js` | 芒果直播 | `asset:///js/mgtv_live.js?id=hndsj` |
| `hntv_live.js` | 河南台（hndt，非湖南） | `asset:///js/hntv_live.js?id=hnws` |
| `fhx_live.js` | 凤凰秀 | `asset:///js/fhx_live.js?id=fhzw` |
| `sdtv_live.js` | 山东台 | `asset:///js/sdtv_live.js?id=sdws` |
| `huya_live.js` | 虎牙 | `…/huya_live.js?id=房间号` |
| `hebtv_live.js` | 河北 TV（md5） | `…/hebtv_live.js?id=路径名` |
| `ahtv_live.js` | 安徽台 | `…/ahtv_live.js?id=数字 id` |
| `cctv_live.js` | 央视新闻（HMAC+AES） | `…/cctv_live.js?id=直播间号` |
| `yspweb.js` | 央视频 Web 明文源 | 改完后 `pack_msjs.py` 再进 APK |
| `yspweb.msjs` | 同上 MSJS 加密包 | msm3 assets 里 `yspweb.js` 已是此密文；频道仍 `asset:///js/yspweb.js?id=cctv1` |
| `body_demo.js` | B：JS 下 m3u8 交内存起播 | `…/body_demo.js?page=https://…/index.m3u8` |
| `stream_hook_demo.js` | B + `stream` → 分片走 `stream_call` | 透传样板 |
| `gxtv_live.js` | 广西台（A + stream 解密 TS） | `asset:///js/gxtv_live.js?id=gxws` |

用法：`main` → **A** url / **B** m3u8|body；若 `stream:true`（+`streamArgs`），分片调固定 **`stream_call(url, args)`** 回字节。

**只写 `ms.*`**。详见 `docs/SDK接入指南.md` §12；对照 https://www.aliiptv.com/js-rules 。
