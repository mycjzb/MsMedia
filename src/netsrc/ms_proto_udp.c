/*
 * UDP / RTP 输入（纯自研）：udp://[@]host:port 或 rtp://[@]host:port[?localaddr=IP]
 * - 组播地址（224.0.0.0/4）自动 IP_ADD_MEMBERSHIP；单播绑定本地端口。
 * - RTP（含 udp:// 里自动识别的 RTP/MP2T）：剥 12B 头 + CSRC + 扩展头 + padding，按序号丢迟到/重复包，
 *   负载为 MPEG-TS（PT 33 或负载首字节 0x47）。缺包由 TS 连续计数自恢复。
 * - 取消标志 ≤100ms 生效；read_timeout 内无数据报错退出（不无限挂起）。
 * by: Long <mycjzb@gmail.com>
 */
#define _GNU_SOURCE
#include "ms_netsrc.h"
#include "../netcore/ms_netcore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void errf(ms_ns_out *o, const char *m)
{
    if (o->err && o->err_cap) snprintf(o->err, o->err_cap, "%s", m);
}

/* 解析 udp://[@]host:port[?localaddr=x]；host 为空或 @ 表示任意地址 */
static int parse_hostport(const char *url, char *host, size_t hcap, int *port, char *laddr, size_t lcap)
{
    const char *p = strstr(url, "://"), *q, *colon;
    size_t hl;
    if (!p) return -1;
    p += 3;
    if (*p == '@') p++;
    q = p + strcspn(p, "/?");
    colon = NULL;
    {
        const char *c;
        for (c = p; c < q; c++) if (*c == ':') colon = c;
    }
    if (!colon) return -1;
    hl = (size_t)(colon - p);
    if (hl >= hcap) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    laddr[0] = 0;
    {
        const char *la = strstr(q, "localaddr=");
        if (la) {
            size_t l = strcspn(la + 10, "&");
            if (l < lcap) { memcpy(laddr, la + 10, l); laddr[l] = 0; }
        }
    }
    return 0;
}

/* 返回 TS 负载偏移，-1=不是可用 RTP 包 */
static int rtp_payload(const uint8_t *d, int n, uint16_t *seq)
{
    int off, cc;
    if (n < 12 || (d[0] >> 6) != 2) return -1;
    cc = d[0] & 0x0F;
    off = 12 + cc * 4;
    if (d[0] & 0x10) {
        if (off + 4 > n) return -1;
        off += 4 + 4 * ((d[off + 2] << 8) | d[off + 3]);
    }
    if (off > n) return -1;
    if (d[0] & 0x20) {
        int pad = d[n - 1];
        if (pad > n - off) return -1;
        n -= pad;
    }
    *seq = (uint16_t)((d[2] << 8) | d[3]);
    return off;
}

int ms_proto_udp_run(const char *url, int is_rtp, ms_ns_out *o)
{
    char host[128], laddr[64];
    int port, fd, one = 1, rcvbuf = 4 * 1024 * 1024, have_seq = 0, mode = is_rtp ? 1 : -1; /* -1 待判定 0 裸 TS 1 RTP */
    uint16_t last = 0;
    struct sockaddr_in sa;
    uint8_t *buf;
    int idle = 0, ret = 0;
    if (parse_hostport(url, host, sizeof(host), &port, laddr, sizeof(laddr)) != 0) {
        errf(o, "udp: bad url");
        return MS_NC_E_ARG;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { errf(o, "udp: socket failed"); return MS_NC_E_IO; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    {
        struct in_addr ia;
        int mcast = host[0] && inet_pton(AF_INET, host, &ia) == 1 && (ntohl(ia.s_addr) >> 28) == 0xE;
        /* 组播绑组地址（只收本组），单播/空主机绑任意地址 */
        sa.sin_addr.s_addr = mcast ? ia.s_addr : htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close(fd);
            errf(o, "udp: bind failed");
            return MS_NC_E_IO;
        }
        if (mcast) {
            struct ip_mreq mr;
            memset(&mr, 0, sizeof(mr));
            mr.imr_multiaddr = ia;
            mr.imr_interface.s_addr = htonl(INADDR_ANY);
            if (laddr[0]) inet_pton(AF_INET, laddr, &mr.imr_interface);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
                close(fd);
                errf(o, "udp: join multicast failed");
                return MS_NC_E_IO;
            }
        }
    }
    buf = (uint8_t *)malloc(65536);
    if (!buf) { close(fd); return MS_NC_E_NOMEM; }
    for (;;) {
        struct pollfd pf;
        int r, n, off = 0;
        if (o->cancel && __atomic_load_n(o->cancel, __ATOMIC_ACQUIRE)) { ret = MS_NC_E_CANCELED; break; }
        pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
        r = poll(&pf, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { ret = MS_NC_E_IO; break; }
        if (r == 0) {
            idle += 100;
            if (idle >= o->read_timeout_ms) { errf(o, "udp: no data (timeout)"); ret = MS_NC_E_TIMEOUT; break; }
            continue;
        }
        idle = 0;
        n = (int)recv(fd, buf, 65536, 0);
        if (n <= 0) continue;
        if (mode < 0) {
            uint16_t s;
            int po = rtp_payload(buf, n, &s);
            mode = (buf[0] == 0x47) ? 0 : (po >= 12 && po < n && buf[po] == 0x47) ? 1 : 0;
        }
        if (mode == 1) {
            uint16_t s;
            int po = rtp_payload(buf, n, &s);
            if (po < 0) continue;
            if (have_seq) {
                int16_t d = (int16_t)(s - last);
                if (d <= 0 && d > -1000) continue; /* 迟到/重复 */
            }
            have_seq = 1;
            last = s;
            if (buf[0] & 0x20) n -= buf[n - 1];
            off = po;
        }
        if (n > off && o->write(o->ctx, buf + off, (size_t)(n - off)) != 0) { ret = MS_NC_E_CANCELED; break; }
    }
    free(buf);
    close(fd);
    return ret;
}
