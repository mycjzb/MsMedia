/* 宿主自检：A(url) + B(m3u8 body)。编译见 qjs_eval_host.sh */
#include "../src/qjs/ms_qjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_url(void)
{
    const char *script =
            "function main(item) {\n"
            "  return { url: 'https://example.com/live/index.m3u8', referer: 'https://example.com/' };\n"
            "}\n";
    ms_qjs_result r;
    if (ms_qjs_eval_ex(script, strlen(script), "http://cdn.example/a.js?page=1", &r) != 0) {
        fprintf(stderr, "A eval fail\n");
        return 1;
    }
    if (r.kind != MS_QJS_KIND_URL || !r.url ||
        strcmp(r.url, "https://example.com/live/index.m3u8") != 0) {
        fprintf(stderr, "A bad url kind=%d\n", r.kind);
        ms_qjs_result_clear(&r);
        return 2;
    }
    if (!r.headers || strstr(r.headers, "Referer:") == NULL) {
        fprintf(stderr, "A bad hdr\n");
        ms_qjs_result_clear(&r);
        return 3;
    }
    printf("ok A url=%s\n", r.url);
    ms_qjs_result_clear(&r);
    return 0;
}

static int check_body(void)
{
    const char *script =
            "function main(item) {\n"
            "  return { m3u8: '#EXTM3U\\n#EXTINF:1,\\nhttps://cdn.example/a.ts\\n', url: item.pageUrl };\n"
            "}\n";
    ms_qjs_result r;
    if (ms_qjs_eval_ex(script, strlen(script), "http://cdn.example/b.js?page=https://x/", &r) !=
        0) {
        fprintf(stderr, "B eval fail\n");
        return 1;
    }
    if (r.kind != MS_QJS_KIND_BODY || !r.body || r.body_len < 7 ||
        memcmp(r.body, "#EXTM3U", 7) != 0) {
        fprintf(stderr, "B bad body kind=%d len=%zu\n", r.kind, r.body_len);
        ms_qjs_result_clear(&r);
        return 2;
    }
    if (!r.content_type || strstr(r.content_type, "mpegurl") == NULL) {
        fprintf(stderr, "B bad ct=[%s]\n", r.content_type ? r.content_type : "");
        ms_qjs_result_clear(&r);
        return 3;
    }
    printf("ok B m3u8 len=%zu ct=%s\n", r.body_len, r.content_type);
    ms_qjs_result_clear(&r);
    return 0;
}

static int check_stream(void)
{
    const char *script =
            "function main(item) {\n"
            "  return { url: 'https://example.com/a.m3u8', stream: true,\n"
            "           streamArgs: { k: 'v' } };\n"
            "}\n"
            "function stream_call(url, args) {\n"
            "  return 'TS' + url + (args && args.k ? args.k : '');\n"
            "}\n";
    ms_qjs_result r;
    unsigned char *bytes = NULL;
    size_t n = 0;
    if (ms_qjs_eval_ex(script, strlen(script), "http://cdn/s.js", &r) != 0 || !r.stream ||
        !r.stream_args || strstr(r.stream_args, "\"k\"") == NULL) {
        fprintf(stderr, "stream flag fail stream=%d args=%s\n", r.stream,
                r.stream_args ? r.stream_args : "");
        ms_qjs_result_clear(&r);
        return 1;
    }
    if (ms_qjs_stream_call(script, strlen(script), "https://cdn/x.ts", r.stream_args, &bytes, &n) !=
                0 ||
        !bytes || n < 4 || memcmp(bytes, "TS", 2) != 0) {
        fprintf(stderr, "stream_call fail n=%zu\n", n);
        free(bytes);
        ms_qjs_result_clear(&r);
        return 2;
    }
    printf("ok stream_call n=%zu\n", n);
    free(bytes);
    ms_qjs_result_clear(&r);
    return 0;
}

int main(void)
{
    int rc = check_url();
    if (rc) {
        return rc;
    }
    rc = check_body();
    if (rc) {
        return rc;
    }
    return check_stream();
}
