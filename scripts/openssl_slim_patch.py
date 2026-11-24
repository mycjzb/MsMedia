#!/usr/bin/env python3
# 极致裁剪 OpenSSL 3.5.9 源码副本的补丁（幂等）：只作用于 build/openssl-3.5.9-slim-src 副本，
# third_party/cache/openssl-3.5.9 原始源码只读不改。
# 补丁内容（均包在 MS_OSSL_TLS_CLIENT_ONLY 宏里，未定义该宏时与上游完全一致）：
#   default provider 的 deflt_query 不再对外提供 ENCODER / STORE / KEM / SKEYMGMT 四类算法表。
#   TLS 1.2/1.3 客户端（ECDHE + RSA/ECDSA 证书校验）只用到 摘要/对称/MAC/KDF/随机数/密钥管理/
#   密钥交换/签名/非对称加密/DER 解码；这四类表只被 deflt_query 引用，去掉引用后
#   Full LTO + --gc-sections 能把 PEM/DER 编码器、file: 存储加载器、RSA/EC KEM 等实现整体删掉。
#   业务若将来需要（例如后量子混合 KEM、导出密钥），去掉 -DMS_OSSL_TLS_CLIENT_ONLY 即可恢复。
# by: Long <mycjzb@gmail.com>
import sys, pathlib
src = pathlib.Path(sys.argv[1]) / "providers" / "defltprov.c"
s = src.read_text()
MARK = "MS_OSSL_TLS_CLIENT_ONLY"
if MARK in s:
    print("openssl-slim-patch: already applied"); sys.exit(0)
def guard(case, ret):
    global s
    old = f"    case {case}:\n        return {ret};\n"
    assert s.count(old) == 1, case
    s = s.replace(old, f"#ifndef {MARK} /* MsMedia：TLS 纯客户端不需要 */\n{old}#endif\n")
for c, r in (("OSSL_OP_KEM", "deflt_asym_kem"), ("OSSL_OP_ENCODER", "deflt_encoder"),
             ("OSSL_OP_STORE", "deflt_store"), ("OSSL_OP_SKEYMGMT", "deflt_skeymgmt")):
    guard(c, r)
src.write_text(s)
print("openssl-slim-patch: applied", src)
