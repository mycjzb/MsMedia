#!/usr/bin/env bash
# 与超级直播（bs-tv / bs-player）同一套本机 Android 工具链解析。
# 由其它 scripts/*.sh source；勿单独当入口。
# by: Long <mycjzb@gmail.com>
#
# 优先级：
#   NDK: ANDROID_NDK / ANDROID_NDK_HOME / ANDROID_NDK_ROOT
#        → $SDK/ndk/21.4.7075529（超级直播 ndkVersion）
#        → 旧 Bot 路径 android-ndk-r29（仅兜底）
#   SDK: ANDROID_SDK_ROOT / ANDROID_HOME → /home/long/Android/SDK → ~/Android/Sdk
#   JDK: JAVA_HOME → /home/long/Android/jdk-17 → jdk-11 → 旧 Bot jdk

_ms_first_dir() {
  local d
  for d in "$@"; do
    [[ -n "$d" && -d "$d" ]] && { printf '%s\n' "$d"; return 0; }
  done
  return 1
}

MS_SDK="$(_ms_first_dir \
  "${ANDROID_SDK_ROOT:-}" \
  "${ANDROID_HOME:-}" \
  /home/long/Android/SDK \
  "${HOME:+$HOME/Android/Sdk}" \
  /home/box/.local/android-sdk)" || MS_SDK=""

MS_NDK="$(_ms_first_dir \
  "${ANDROID_NDK:-}" \
  "${ANDROID_NDK_HOME:-}" \
  "${ANDROID_NDK_ROOT:-}" \
  ${MS_SDK:+"$MS_SDK/ndk/21.4.7075529"} \
  /home/box/.local/android-ndk-r29)" || MS_NDK=""

MS_JDK="$(_ms_first_dir \
  "${JAVA_HOME:-}" \
  /home/long/Android/jdk-17 \
  /home/long/Android/jdk-11 \
  /home/box/.local/jdk)" || MS_JDK=""

# 超级直播 so 压壳工具（可选；PACK_SO=1 时用）
MS_BS_PLAYER_SCRIPTS="${MS_BS_PLAYER_SCRIPTS:-/home/long/code/bs-p2p/bs-player/scripts}"
MS_UPX396="$(_ms_first_dir \
  "${MS_UPX396:-}" \
  "$MS_BS_PLAYER_SCRIPTS/upx396" \
  /usr/local/bin)" || MS_UPX396=""
MS_UPX53="$(_ms_first_dir \
  "${MS_UPX53:-}" \
  "$MS_BS_PLAYER_SCRIPTS/upx" \
  /usr/local/bin)" || MS_UPX53=""

export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$MS_SDK}"
export ANDROID_HOME="${ANDROID_HOME:-$MS_SDK}"
export ANDROID_NDK="$MS_NDK"
export ANDROID_NDK_HOME="$MS_NDK"
export ANDROID_NDK_ROOT="$MS_NDK"
export JAVA_HOME="${JAVA_HOME:-$MS_JDK}"

if [[ -z "$MS_NDK" || ! -x "$MS_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
  echo "error: Android NDK clang missing (expect SDK/ndk/21.4.7075529 like 超级直播)" >&2
  echo "error: set ANDROID_NDK or install ndk; MS_SDK=${MS_SDK:-unset}" >&2
  return 2 2>/dev/null || exit 2
fi
