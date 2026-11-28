#!/usr/bin/env bash
# 构建钉选官方 OpenSSL 3.5.9 LTS 的 PIC 静态库（无共享模块/无 apps/tests）。
# 由仓库根目录 ./build.sh 在需要 libmsff 时调用。下载按 pin 校验，不自动升级。
# 宿主自测，不能代替 el7/四平台正式验收。
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIN="$ROOT/third_party/openssl-3.5.9.pin"
CACHE="$ROOT/third_party/cache"
OPENSSL_ROOT="${OPENSSL_BUILD_ROOT:-$ROOT/build/openssl-3.5.9}"
PREFIX="${OPENSSL_PREFIX:-$OPENSSL_ROOT/prefix}"
BUILDDIR="${OPENSSL_BUILDDIR:-$OPENSSL_ROOT/build}"
SRC="$CACHE/openssl-3.5.9"
FLAGS_LOG="$OPENSSL_ROOT/configure-flags.txt"
STAMP="$PREFIX/.msmedia-openssl-pin"
OPENSSL_TARGET="${OPENSSL_TARGET:-linux-x86_64}"

# shellcheck disable=SC1090
source "$PIN"

need() { command -v "$1" >/dev/null || { echo "error: missing $1" >&2; exit 2; }; }
need curl; need gpg; need sha256sum; need tar; need make; need perl

mkdir -p "$CACHE" "$OPENSSL_ROOT" "$(dirname "$BUILDDIR")" "$PREFIX"

download() {
  local url="$1" dest="$2"
  if [[ -f "$dest" ]]; then
    return 0
  fi
  curl -fL --retry 3 --retry-delay 2 -o "$dest.part" "$url"
  mv "$dest.part" "$dest"
}

download "$pgp_key_url" "$CACHE/openssl-pubkeys.asc"
download "$signature_url" "$CACHE/openssl-3.5.9.tar.gz.asc"
download "$sha256_url" "$CACHE/openssl-3.5.9.tar.gz.sha256"
download "$tarball_url" "$CACHE/openssl-3.5.9.tar.gz"

got="$(sha256sum "$CACHE/openssl-3.5.9.tar.gz" | awk '{print $1}')"
if [[ "$got" != "$tarball_sha256" ]]; then
  echo "error: SHA-256 mismatch for openssl-3.5.9.tar.gz" >&2
  echo "  expected $tarball_sha256" >&2
  echo "  got      $got" >&2
  echo "  refusing to auto-upgrade; check $PIN" >&2
  exit 1
fi
# Official published SHA-256 file must agree with the pin.
if ! awk '{print $1}' "$CACHE/openssl-3.5.9.tar.gz.sha256" | grep -qx "$tarball_sha256"; then
  echo "error: official openssl-3.5.9.tar.gz.sha256 does not match pin" >&2
  cat "$CACHE/openssl-3.5.9.tar.gz.sha256" >&2
  exit 1
fi

GPGLOG="$OPENSSL_ROOT/gpg-verify.log"
mkdir -p "$(dirname "$GPGLOG")"
gpg --import "$CACHE/openssl-pubkeys.asc" >/dev/null 2>&1 || true
# LANG=C：中文 locale 会输出「完好的签名」，下面的 Good signature 匹配会误杀。
if ! LANG=C gpg --verify "$CACHE/openssl-3.5.9.tar.gz.asc" "$CACHE/openssl-3.5.9.tar.gz" 2>"$GPGLOG"; then
  # 未信任钥时 exit≠0，但文案仍可能是 Good signature；以文案为准。
  if ! grep -qE 'Good signature|完好的签名' "$GPGLOG"; then
    echo "error: gpg --verify failed; see $GPGLOG" >&2
    exit 1
  fi
fi
if ! grep -qE 'Good signature|完好的签名' "$GPGLOG"; then
  echo "error: PGP signature not good; see $GPGLOG" >&2
  exit 1
fi
fp_found="$(tr -d ' \n' < "$GPGLOG" | tr '[:lower:]' '[:upper:]')"
fp_want="$(echo "$pgp_key_fingerprint" | tr -d ' ' | tr '[:lower:]' '[:upper:]')"
if ! grep -q "$fp_want" <<<"$fp_found"; then
  echo "error: unexpected PGP key; see $GPGLOG" >&2
  exit 1
fi

if [[ ! -x "$SRC/Configure" ]]; then
  tar -xzf "$CACHE/openssl-3.5.9.tar.gz" -C "$CACHE"
fi
if [[ ! -f "$SRC/VERSION.dat" ]] && [[ ! -f "$SRC/VERSION" ]]; then
  echo "error: extracted OpenSSL tree missing VERSION" >&2
  exit 1
fi
if ! grep -q "3.5.9" "$SRC/VERSION.dat" 2>/dev/null && \
   ! grep -q "3.5.9" "$SRC/VERSION" 2>/dev/null && \
   ! grep -q 'MAJOR=3' "$SRC/VERSION.dat"; then
  # VERSION.dat uses MAJOR/MINOR/PATCH
  :
fi
if [[ -f "$SRC/VERSION.dat" ]]; then
  ver="$(awk -F= '/^MAJOR=/{m=$2} /^MINOR=/{n=$2} /^PATCH=/{p=$2} END{printf "%s.%s.%s",m,n,p}' "$SRC/VERSION.dat" | tr -d ' \r')"
  if [[ "$ver" != "3.5.9" ]]; then
    echo "error: extracted VERSION.dat is $ver, expected 3.5.9" >&2
    exit 1
  fi
fi

# 保留 TLS 1.2/1.3 客户端通用 HTTPS/RTMPS/RTSPS 所需算法与 asm。
# 关闭无关 apps/tests/engines/动态 provider、过时协议与少见算法。
# no-module/no-dso：default provider 编入 libcrypto，不加载系统 provider .so。
CFG_BASE=(
  --prefix="$PREFIX"
  --openssldir="$PREFIX/ssl"
  --libdir=lib
  -fPIC
  no-shared
  no-module
  no-dso
  no-dynamic-engine
  no-engine
  no-legacy
  no-apps
  no-docs
  no-tests
  no-fips
  no-ssl3
  no-tls1
  no-tls1_1
  no-dtls
  no-dtls1
  no-dtls1_2
  no-srp
  no-psk
  no-weak-ssl-ciphers
  no-comp
  no-zlib
  no-md2
  no-md4
  no-rc2
  no-rc4
  no-rc5
  no-idea
  no-seed
  no-whirlpool
  no-sm2
  no-sm3
  no-sm4
  no-aria
  no-bf
  no-cast
  no-camellia
  no-des
  no-ts
  no-cms
  no-cmp
  # 保留 SRTP 扩展：FFmpeg 9.1 libavformat/tls_openssl.c 无条件引用
  # SSL_CTX_set_tlsext_use_srtp（DTLS+use_srtp 路径）。no-srtp 会使该声明消失并编不过。
  no-filenames
)
# 体积优化（MS_SIZE_OPT=1）：-Os / section / SMALL_FOOTPRINT。
# 不照搬 OpenSSL 1.1.1 的 no-err/no-ocsp/no-ct/no-srtp/no-dh 等，以免减功能冒充优化。
# 仍保留 TLS 1.2/1.3 与证书/主机名校验；SRTP 扩展因 FFmpeg tls_openssl.c 必须保留。
if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
  CFG_BASE+=(
    -Os
    -ffunction-sections
    -fdata-sections
    -DOPENSSL_SMALL_FOOTPRINT
  )
fi
if [[ "$OPENSSL_TARGET" == android-* ]]; then
  CFG_BASE+=("-D__ANDROID_API__=${ANDROID_API:-21}")
fi

OPTIONAL=()
# 可选选项：不存在则跳过，避免不同 Configure 拒识。
help="$BUILDDIR/../configure-help.txt"
mkdir -p "$(dirname "$help")"
if [[ ! -f "$help" ]] || ! grep -q -- no-shared "$help"; then
  (cd "$SRC" && ./Configure --help >"$help" 2>&1 || true)
fi
for opt in no-quic no-autoload-config enable-ec_nistp_64_gcc_128; do
  if grep -F -q -- "$opt" "$help"; then
    OPTIONAL+=("$opt")
  fi
done
CFG_ARGS=("${CFG_BASE[@]}" "${OPTIONAL[@]+"${OPTIONAL[@]}"}")

mkdir -p "$OPENSSL_ROOT"
{
  echo "# OpenSSL 3.5.9 trimmed static PIC configure (archived $(date -Iseconds))"
  echo "source=$SRC"
  echo "prefix=$PREFIX"
  echo "pin_sha256=$tarball_sha256"
  printf 'Configure %s' "$OPENSSL_TARGET"
  for a in "${CFG_ARGS[@]}"; do printf ' %q' "$a"; done
  echo
} | tee "$FLAGS_LOG"

FLAGHASH="$(printf '%s\n' "$OPENSSL_TARGET" "${MS_SIZE_OPT:-0}" "${CFG_ARGS[@]}" | sha256sum | awk '{print $1}')"
PINREC="version=$version
tarball_sha256=$tarball_sha256
target=$OPENSSL_TARGET
size_opt=${MS_SIZE_OPT:-0}
flags_sha256=$FLAGHASH
"

if [[ -f "$PREFIX/lib/libssl.a" && -f "$PREFIX/lib/libcrypto.a" && \
      -f "$STAMP" && "${OPENSSL_REBUILD:-0}" != 1 ]]; then
  # 不得用 $(cat stamp)：命令替换会吃掉末尾换行，导致每次都误判 mismatch 并重编前缀。
  if printf '%s' "$PINREC" | cmp -s "$STAMP" -; then
    echo "openssl 3.5.9 prefix already present at $PREFIX (set OPENSSL_REBUILD=1 to rebuild)"
    exit 0
  fi
  echo "openssl prefix stamp mismatch; rebuilding"
fi

rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"
# 不用系统 OpenSSL 的 -I/-L
env -u PKG_CONFIG_PATH -u OPENSSL_CONF -u OPENSSL_MODULES -u CPATH -u C_INCLUDE_PATH \
  "$SRC/Configure" "$OPENSSL_TARGET" "${CFG_ARGS[@]}"
# gcc 以最后一个 -O 为准：若目标模板把 -O3 放在 -Os 之后，则把 -Os 追加到 CFLAGS 末尾。
if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
  ev="$(awk -F= '/^CFLAGS=/{print; exit}' Makefile || true)"
  echo "effective $ev" | tee -a "$FLAGS_LOG"
  last=$(echo "$ev" | grep -oE -- '-O([0-9s]|fast|g)' | tail -1 || true)
  if [[ "$last" != "-Os" ]]; then
    echo "warning: Makefile CFLAGS last -O is ${last:-none}, appending -Os" | tee -a "$FLAGS_LOG"
    awk '{if($0 ~ /^CFLAGS=/){print $0 " -Os"} else print}' Makefile > Makefile.size && mv Makefile.size Makefile
    ev="$(awk -F= '/^CFLAGS=/{print; exit}' Makefile || true)"
    echo "adjusted $ev" | tee -a "$FLAGS_LOG"
    last=$(echo "$ev" | grep -oE -- '-O([0-9s]|fast|g)' | tail -1 || true)
    if [[ "$last" != "-Os" ]]; then
      echo "error: failed to make -Os the last optimization flag ($ev)" >&2
      exit 1
    fi
  fi
fi
make -j"$(nproc)"
make install_sw

# 不自动加载系统 openssl.cnf / provider 模块。默认信任通过符号链接指向宿主 CA 目录。
mkdir -p "$PREFIX/ssl"
if [[ ! -f "$PREFIX/ssl/openssl.cnf" ]]; then
  cat > "$PREFIX/ssl/openssl.cnf" << 'CNF'
# MsMedia stub: no provider modules, no system openssl.cnf.
# The default provider is built into libcrypto (no-module).
# This file is not a skip-verify switch.
openssl_conf = openssl_init
[openssl_init]
CNF
fi
if [[ -d /etc/ssl/certs && ! -e "$PREFIX/ssl/certs" ]]; then
  ln -sfn /etc/ssl/certs "$PREFIX/ssl/certs"
fi
if [[ -f /etc/ssl/cert.pem && ! -e "$PREFIX/ssl/cert.pem" ]]; then
  ln -sfn /etc/ssl/cert.pem "$PREFIX/ssl/cert.pem"
elif [[ -f /etc/ssl/certs/ca-certificates.crt && ! -e "$PREFIX/ssl/cert.pem" ]]; then
  ln -sfn /etc/ssl/certs/ca-certificates.crt "$PREFIX/ssl/cert.pem"
fi

printf '%s' "$PINREC" > "$STAMP"
# 记录版本实证
if [[ -x "$PREFIX/bin/openssl" && "$OPENSSL_TARGET" != android-* ]]; then
  "$PREFIX/bin/openssl" version
else
  echo "openssl 3.5.9 static prefix installed at $PREFIX (target=$OPENSSL_TARGET)"
fi
echo "installed $PREFIX"
