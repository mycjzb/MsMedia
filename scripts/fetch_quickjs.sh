#!/usr/bin/env bash
# 拉取 QuickJS 源码到 third_party/quickjs（客户本机执行一次即可）。
# 优先：本机超级直播树；否则 git clone Bellard 官方（需网络）。
# 用法：
#   bash scripts/fetch_quickjs.sh
#   QJS_SRC=/path/to/quickjs bash scripts/fetch_quickjs.sh
#   QJS_GIT=1 bash scripts/fetch_quickjs.sh   # 强制 git
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/quickjs"
NEED=(quickjs.c quickjs.h cutils.c cutils.h libregexp.c libunicode.c dtoa.c)

have_tree() {
  local d="$1" f
  [[ -d "$d" ]] || return 1
  for f in "${NEED[@]}"; do
    [[ -f "$d/$f" ]] || return 1
  done
  return 0
}

copy_tree() {
  local src="$1"
  mkdir -p "$DEST"
  # 只拷编译所需 + 许可/版本，避免整仓 examples/test262
  for f in \
    quickjs.c quickjs.h quickjs-atom.h quickjs-opcode.h \
    cutils.c cutils.h \
    libregexp.c libregexp.h libregexp-opcode.h \
    libunicode.c libunicode.h libunicode-table.h \
    dtoa.c dtoa.h list.h \
    LICENSE VERSION Changelog readme.txt
  do
    [[ -f "$src/$f" ]] && cp -f "$src/$f" "$DEST/$f"
  done
  # 无 VERSION 时写死与超级直播同款
  if [[ ! -f "$DEST/VERSION" ]]; then
    echo "2026-06-04" >"$DEST/VERSION"
  fi
  echo "fetch_quickjs: copied from $src -> $DEST (version=$(cat "$DEST/VERSION"))"
}

# 1) 显式路径
if [[ -n "${QJS_SRC:-}" ]]; then
  have_tree "$QJS_SRC" || { echo "error: QJS_SRC incomplete: $QJS_SRC" >&2; exit 2; }
  copy_tree "$QJS_SRC"
  exit 0
fi

# 2) 已存在且完整 → 跳过
if have_tree "$DEST" && [[ "${QJS_FORCE:-0}" != 1 ]]; then
  echo "fetch_quickjs: already present $DEST (version=$(cat "$DEST/VERSION" 2>/dev/null || echo '?'))"
  exit 0
fi

# 3) 本机超级直播 / 其它常见路径
CAND=(
  "${MS_BS_PLAYER:-/home/long/code/bs-p2p/bs-player}/third_party/quickjs"
  "$HOME/code/bs-p2p/bs-player/third_party/quickjs"
)
if [[ "${QJS_GIT:-0}" != 1 ]]; then
  for c in "${CAND[@]}"; do
    if have_tree "$c"; then
      copy_tree "$c"
      exit 0
    fi
  done
fi

# 4) git clone（浅克隆官方）
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetch_quickjs: git clone https://github.com/bellard/quickjs.git …"
git clone --depth 1 https://github.com/bellard/quickjs.git "$TMP/quickjs"
have_tree "$TMP/quickjs" || { echo "error: clone missing sources" >&2; exit 2; }
copy_tree "$TMP/quickjs"
