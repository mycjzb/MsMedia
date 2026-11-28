#ifndef MS_QJS_H
#define MS_QJS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main() 返回：A=远程 URL；B=正文；可带 stream 要求分片走 stream_call。 */
enum {
    MS_QJS_KIND_URL = 0,
    MS_QJS_KIND_BODY = 1,
};

typedef struct ms_qjs_result {
    int kind;                  /* MS_QJS_KIND_* */
    char *url;                 /* A：可播 URL；B：可选基址 */
    char *headers;             /* "Name: value\n…" */
    char *content_type;        /* B：MIME；可空则嗅探 */
    unsigned char *body;       /* B：正文 */
    size_t body_len;
    int stream;                /* 非 0：后续分片/二次拉流调 stream_call */
    char *stream_args;         /* JSON 字符串，传给 stream_call 第二参；可空 */
} ms_qjs_result;

void ms_qjs_result_clear(ms_qjs_result *r);

/* 跑 main(item)；body≤8MiB。成功 0。 */
int ms_qjs_eval_ex(const char *script, size_t script_len, const char *item_url,
                   ms_qjs_result *out);

int ms_qjs_eval(const char *script, size_t script_len, const char *item_url, char *out_url,
                size_t out_url_cap, char *out_hdr, size_t out_hdr_cap);

/* 固定方法 stream_call(url, argsObj) → 字节。args_json 可空（传 {}）。
 * 成功 0，*out 需 free；单次上限 32MiB。 */
int ms_qjs_stream_call(const char *script, size_t script_len, const char *url,
                       const char *args_json, unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MS_QJS_H */
