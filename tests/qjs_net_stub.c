/* 宿主 qjs 自检用：不链完整 netcore。 */
#include "../src/netcore/ms_netcore.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ms_nc_http {
    int status;
};

int ms_nc_http_open(const ms_nc_req *req, ms_nc_http **out)
{
    (void)req;
    if (!out) {
        return -1;
    }
    *out = (ms_nc_http *)calloc(1, sizeof(ms_nc_http));
    if (!*out) {
        return -1;
    }
    (*out)->status = 200;
    return 0;
}

int ms_nc_http_status(const ms_nc_http *h)
{
    return h ? h->status : 0;
}

const char *ms_nc_http_headers(const ms_nc_http *h)
{
    (void)h;
    return "";
}

int64_t ms_nc_http_content_length(const ms_nc_http *h)
{
    (void)h;
    return 0;
}

int ms_nc_http_read(ms_nc_http *h, uint8_t *buf, int cap)
{
    (void)h;
    (void)buf;
    (void)cap;
    return 0;
}

void ms_nc_http_cancel(ms_nc_http *h)
{
    (void)h;
}

void ms_nc_http_close(ms_nc_http *h)
{
    free(h);
}
