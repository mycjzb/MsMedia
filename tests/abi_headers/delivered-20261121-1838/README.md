# 已交付 1838 公共头冻结副本

来源：`MsMedia-src-20261121-1838.tar.gz`
SHA-256：`7303150d5f0745d1642fc8ece3251805e4b77bd3c27e15581a5caa82e4a3c7a6`

本目录的 `include/msmedia/*.h` 从该包原样抽出，供
`tests/test_abi_old_header.c` 以旧头编译、再动态链接当前 `.so`。
不要把这里的头当成当前产品 API 编辑；产品头仍在仓库根 `include/`。

在本机 LP64 上，该包的 `sizeof(ms_event)` 为 176。
`ms_sub_cue`、`ms_session_list_tracks`、`ms_ff_list_tracks`
在 1838 头中尚不存在（加法 API），旧客户端专项不调用它们。
