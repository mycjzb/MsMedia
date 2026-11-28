#!/usr/bin/env bash
# 路线 B 主力单 so 用：OpenSSL 3.5.9 LTS「TLS 1.2/1.3 纯客户端最小集合」极致裁剪静态库。
#  - 裁剪原则（用户定调）：「现在没用到就裁掉，业务有需求再调整」。只保留当前 HTTPS / RTMPS 播放与
#    MsHttpClient 严格校验实际必需：AES-GCM/CBC、SHA-1/2、HMAC/HKDF、RSA / ECDSA / ECDHE(P-256/384) / X25519；
#    ChaCha20-Poly1305、DHE(DH)、socket BIO 也关掉（TLS1.3 强制套件是 AES-128-GCM，主流服务器均有 ECDHE+AES-GCM）；
#  - 冷门/老旧算法、DTLS/QUIC/SSL3/TLS1.0/1.1、CMS/CT/OCSP/TS/SRP/PSK/GOST/ENGINE/DSO/legacy、
#    deprecated 旧接口、错误文案大表、后量子(ML-KEM/ML-DSA/SLH-DSA) 全部关闭；
#  - 编译：-Oz -flto=full -ffunction-sections -fdata-sections -fno-unwind-tables（静态库为 LTO 位码，
#    在 libmsplayer.so 最终链接时与 FFmpeg/自研代码统一 Full LTO + --gc-sections 去死代码）；
#  - 源码与签名校验复用 scripts/openssl_build.sh 已下载并校验过的 third_party/cache/openssl-3.5.9（只读，不改）。
# 输出（新目录，不碰冻结的 openssl-3.5.9 / openssl-3.5.9-size 前缀）：
#   build/android-{v7a,v8a,x86,x86_64}/openssl-3.5.9-slim-flto/prefix/lib/{libssl.a,libcrypto.a}
#   build/host/openssl-3.5.9-slim/prefix   （ABIS=host：宿主 ASan/UBSan 单测用，-Os 非 LTO）
# 用法：ABIS="armeabi-v7a arm64-v8a x86 x86_64 host" scripts/openssl_slim_build.sh
# 严禁 mbedTLS、严禁 UPX。by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/android_env.sh"
NDK="$MS_NDK"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
ORIG_SRC="$ROOT/third_party/cache/openssl-3.5.9"   # 已签名校验的原始源码（只读）
SRC="$ROOT/build/openssl-3.5.9-slim-src"           # 打过 scripts/openssl_slim_patch.py 的副本
ABIS="${ABIS:-armeabi-v7a arm64-v8a x86 x86_64}"
JOBS="${JOBS:-$(nproc)}"
[[ -x "$ORIG_SRC/Configure" ]] || { echo "error: missing $ORIG_SRC (先运行 scripts/openssl_build.sh 下载并校验)" >&2; exit 2; }
# 复制一份源码再打补丁，原始缓存保持只读；补丁幂等。
if [[ ! -x "$SRC/Configure" ]]; then
  rm -rf "$SRC.tmp"; cp -a "$ORIG_SRC" "$SRC.tmp"; mv "$SRC.tmp" "$SRC"
fi
python3 "$ROOT/scripts/openssl_slim_patch.py" "$SRC"

# 关闭清单（均为 OpenSSL 3.5.9 Configure 的 @disablables，逐项核对过）。
SLIM_OFF=(
  no-shared no-module no-dso no-dynamic-engine no-engine no-legacy no-fips no-apps no-docs no-tests
  no-deprecated no-err no-filenames no-autoload-config no-ui-console no-stdio
  no-ssl3 no-tls1 no-tls1_1 no-dtls no-quic no-dgram no-sctp no-srtp no-ktls no-tfo
  no-srp no-psk no-weak-ssl-ciphers no-nextprotoneg no-ssl-trace no-sslkeylog
  no-comp no-zlib no-brotli no-zstd
  no-md2 no-md4 no-mdc2 no-rmd160 no-whirlpool no-blake2 no-sm2 no-sm3 no-sm4 no-gost
  no-des no-rc2 no-rc4 no-rc5 no-bf no-cast no-idea no-camellia no-aria no-seed
  no-ocb no-siv no-cmac no-siphash no-scrypt no-argon2
  no-dsa no-ec2m no-tls-deprecated-ec no-ml-kem no-ml-dsa no-slh-dsa
  no-cms no-ct no-ocsp no-ts no-cmp no-http no-rfc3779
  no-async no-thread-pool no-default-thread-pool no-multiblock no-secure-memory
  no-dh no-sock no-chacha no-poly1305 no-ecx no-integrity-only-ciphers
  no-asm
)
# 说明：no-asm —— 体积优先（ARMv7 盒子多数无 AES 指令；TLS 只承载切片下载，C 实现吞吐足够）；
#       no-stdio —— 证书全部由我们自己读文件后经内存 BIO 解析，不需要 FILE* 接口；
#       no-sock —— 收发全部走我们自己的 BIO（netcore / FFmpeg tcp 子连接），不需要 socket BIO；
#       no-dh   —— 只用 ECDHE / X25519 密钥交换；RTMPE（需要 DH）本就未启用，RTMPS 只是 RTMP over TLS；
#       no-ecx  —— 去掉 X25519/X448/Ed25519/Ed448：RFC 8446 §9.1 规定 TLS1.3 必须实现 secp256r1，
#                  所有公网服务器都支持 P-256，客户端只报 P-256/P-384 即可；Ed25519 证书在公网 CA 中几乎不存在；
#       -DMS_OSSL_TLS_CLIENT_ONLY —— 见 scripts/openssl_slim_patch.py（default provider 去掉编码器/存储/KEM 表）。
#       保留 ec、threads。如后续业务需要某项，只需从本清单删掉对应 no-xxx 重编。

build_one() {
  local abi="$1" target cc out opt_c tag
  case "$abi" in
    armeabi-v7a) target=android-arm;    tag=android-v7a ;;
    arm64-v8a)   target=android-arm64;  tag=android-v8a ;;
    x86)         target=android-x86;    tag=android-x86 ;;
    x86_64)      target=android-x86_64; tag=android-x86_64 ;;
    host)        target=linux-x86_64;   tag=host ;;
    *) echo "error: unsupported ABI $abi" >&2; exit 2 ;;
  esac
  # OPENSSL_SLIM_LTO=0：出 ELF .a，供 bfd+Hardening+UPX（对齐超级直播）。默认 1=flto 位码（体积优，只能 lld）。
  local use_lto=1
  [[ "${OPENSSL_SLIM_LTO:-1}" == 0 ]] && use_lto=0
  if [[ "$abi" == host ]]; then
    out="$ROOT/build/host/openssl-3.5.9-slim"
    opt_c=(-Os -fPIC -ffunction-sections -fdata-sections -DMS_OSSL_TLS_CLIENT_ONLY)
  elif [[ "$use_lto" == 1 ]]; then
    out="$ROOT/build/$tag/openssl-3.5.9-slim-flto"
    opt_c=(-Oz -flto=full -fPIC -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
           -DOPENSSL_SMALL_FOOTPRINT -DMS_OSSL_TLS_CLIENT_ONLY "-D__ANDROID_API__=$API")
    [[ "$abi" == armeabi-v7a ]] && opt_c+=(-mthumb)
  else
    out="$ROOT/build/$tag/openssl-3.5.9-slim"
    opt_c=(-Oz -fPIC -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
           -DOPENSSL_SMALL_FOOTPRINT -DMS_OSSL_TLS_CLIENT_ONLY "-D__ANDROID_API__=$API")
    [[ "$abi" == armeabi-v7a ]] && opt_c+=(-mthumb)
  fi
  local prefix="$out/prefix" bdir="$out/build"
  if [[ -f "$prefix/lib/libssl.a" && -f "$prefix/.msmedia-slim-ok" && "${OPENSSL_SLIM_REBUILD:-0}" != 1 ]]; then
    echo "openssl-slim: $abi 已存在，跳过（OPENSSL_SLIM_REBUILD=1 重编）"; return 0
  fi
  rm -rf "$bdir" "$prefix"; mkdir -p "$bdir" "$prefix"
  {
    echo "# OpenSSL 3.5.9 slim client $abi $(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
    printf 'Configure %s' "$target"; printf ' %q' "${SLIM_OFF[@]}" "${opt_c[@]}"; echo
  } > "$out/configure-flags.txt"
  (
    cd "$bdir"
    if [[ "$abi" == host ]]; then
      env -u CPATH -u C_INCLUDE_PATH -u PKG_CONFIG_PATH -u OPENSSL_CONF CC=gcc \
        "$SRC/Configure" "$target" --prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib \
        "${SLIM_OFF[@]}" "${opt_c[@]}" >"$out/configure.log" 2>&1
    else
      env -u CPATH -u C_INCLUDE_PATH -u PKG_CONFIG_PATH -u OPENSSL_CONF \
        ANDROID_NDK_ROOT="$NDK" PATH="$PREBUILT/bin:/usr/bin:/bin" \
        "$SRC/Configure" "$target" "-D__ANDROID_API__=$API" --prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib \
        "${SLIM_OFF[@]}" "${opt_c[@]}" >"$out/configure.log" 2>&1
      # 强制最后一个 -O 为 -Oz（模板自带 -O3 在前），并用 llvm-ar/ranlib 生成可被 LTO 读取的位码静态库。
      sed -i -e 's/^CFLAGS=\(.*\)$/CFLAGS=\1 -Oz/' -e "s#^AR=.*#AR=$PREBUILT/bin/llvm-ar#" \
             -e "s#^RANLIB=.*#RANLIB=$PREBUILT/bin/llvm-ranlib#" Makefile
    fi
    if [[ "$abi" == host ]]; then
      make -j"$JOBS" build_libs >"$out/make.log" 2>&1
      make install_dev >>"$out/make.log" 2>&1
    else
      PATH="$PREBUILT/bin:/usr/bin:/bin" ANDROID_NDK_ROOT="$NDK" make -j"$JOBS" build_libs >"$out/make.log" 2>&1
      PATH="$PREBUILT/bin:/usr/bin:/bin" ANDROID_NDK_ROOT="$NDK" make install_dev >>"$out/make.log" 2>&1
    fi
  ) || { echo "error: openssl-slim $abi 失败，见 $out/*.log" >&2; tail -30 "$out/make.log" "$out/configure.log" 2>/dev/null >&2; exit 1; }
  rm -rf "$prefix/lib/pkgconfig" "$prefix/lib/cmake"
  date '+%F %T' > "$prefix/.msmedia-slim-ok"
  echo "openssl-slim: $abi -> $prefix ($(stat -c%s "$prefix/lib/libcrypto.a") + $(stat -c%s "$prefix/lib/libssl.a") bytes archive)"
}

for abi in $ABIS; do build_one "$abi"; done
