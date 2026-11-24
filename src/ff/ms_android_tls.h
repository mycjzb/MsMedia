/*
 * 安卓系统 TLS 引擎（JNI + SSLEngine）。证书与主机名校验不可关。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_ANDROID_TLS_H
#define MS_ANDROID_TLS_H

int  ms_android_tls_open(void **out, const char *hostname);
void ms_android_tls_close(void *eng);
int  ms_android_tls_handshake(void *eng, unsigned char *buf, int cap, int *n);
int  ms_android_tls_feed_net(void *eng, const unsigned char *buf, int n);
int  ms_android_tls_read(void *eng, unsigned char *buf, int cap,
                         unsigned char *net, int net_cap, int *net_n, int *need_net);
int  ms_android_tls_write(void *eng, const unsigned char *buf, int n,
                          unsigned char *net, int net_cap, int *net_n);

#endif
