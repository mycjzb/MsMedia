# QuickJS（第三方）

MsMedia JS 规则引擎用。源码**不强制入库**；客户拿到工程后执行：

```bash
cd /path/to/MsMedia
bash scripts/fetch_quickjs.sh
```

优先从本机超级直播 `bs-player/third_party/quickjs` 拷贝；没有则 `git clone` Bellard 官方。  
版本文件见 `VERSION`（与超级直播对齐时可 `2026-06-04`）。
