# 从超级直播原样接入（禁止改写）

| 本目录 | 真源 |
|---|---|
| `Hardening.cmake` | `cp` 自 `bs-p2p/bs-player/cmake/Hardening.cmake` |
| `bs-player-scripts/` | 软链 → `bs-p2p/bs-player/scripts/`（含 `pack_apk_so.py`） |
| `align_elf_16k.py` | 软链 → 真源同名 |

mycjzb `build.sh --pack 1` 真链路：

```text
bfd + Hardening(-z,relro -z,now -z,noexecstack -Bsymbolic) → 2×LOAD
→ MS_SO_LZMA=0 python3 pack_apk_so.py --strict …
```

本仓：`LINK_BFD=1` 才压壳；对 lld 4×LOAD 禁止 UPX。更新 Hardening 时从真源再 `cp` 一次，勿手改。

压壳交付示例：

```bash
OPENSSL_SLIM_LTO=0 OPENSSL_SLIM_REBUILD=1 ABIS=armeabi-v7a bash scripts/openssl_slim_build.sh
FF_OPT=os ABIS=armeabi-v7a … bash scripts/android_slimff_ffmpeg.sh   # 非 LTO
LINK_BFD=1 OPT=os PACK_SO=1 ABIS=armeabi-v7a MODE=slimff ADEC=1 WITH_XML=0 TRIM=s6 \
  bash scripts/android_single_build.sh
```
