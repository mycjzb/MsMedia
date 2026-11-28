#ifndef MS_QJS_WRAP_H
#define MS_QJS_WRAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JS 脚本装包（对齐 Lua MSPL 能力，密钥 MsMedia 独有）：
 *   1) 明文：含 function main
 *   2) 裸 gzip：1f 8b …
 *   3) MSJS 密文（文件头固定 24+ 字节）：
 *        [0..3]   magic = 'M''S''J''S'（固定，便于嗅探）
 *        [4..5]   ver   = 0x0001 BE（只认 1）
 *        [6..7]   flags BE：bit0=GCM(必1) bit2=gzip
 *        [8..19]  iv 12B
 *        [20..23] clen BE
 *        [24..)   cipher[clen] + tag[16]
 *
 * 成功：*out malloc，*out_len；且 (*out)[*out_len]==0（QuickJS JS_Eval 硬性要求）。
 * 调用方 free。失败 -1。脚本（明文或整文件）上限 512KiB。
 */
int ms_qjs_unwrap(const unsigned char *in, size_t in_len, unsigned char **out, size_t *out_len);

/* 1 = 头像是 MSJS / gzip / 明文 JS（供嗅探） */
int ms_qjs_looks_wrapped(const unsigned char *in, size_t in_len);

#ifdef __cplusplus
}
#endif

#endif
