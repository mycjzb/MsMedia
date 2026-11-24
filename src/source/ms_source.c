#define _DEFAULT_SOURCE
/*
 * Source 开发适配器：内存缓冲与本地文件。不是网络客户端。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static ms_status mem_size(void *ctx, uint64_t *out_size)
{
    ms_mem_source *m = (ms_mem_source *)ctx;
    if (m == NULL || out_size == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (m->canceled) {
        return MS_ERR_CANCELED;
    }
    *out_size = (uint64_t)m->size;
    return MS_OK;
}

static ms_status mem_read_at(void *ctx, uint64_t offset, uint8_t *buf,
                             size_t len, size_t *out_n)
{
    ms_mem_source *m = (ms_mem_source *)ctx;
    size_t off;
    size_t n;

    if (out_n != NULL) {
        *out_n = 0;
    }
    if (m == NULL || (buf == NULL && len != 0) || out_n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (m->canceled) {
        return MS_ERR_CANCELED;
    }
    if (m->again_left > 0) {
        m->again_left--;
        return MS_AGAIN;
    }
    if (m->fail_after >= 0) {
        if (m->fail_after == 0) {
            return (m->fail_st != MS_OK) ? m->fail_st : MS_ERR_UNKNOWN;
        }
        m->fail_after--;
    }
    if (offset >= (uint64_t)m->size) {
        return MS_OK;
    }
    off = (size_t)offset;
    n = m->size - off;
    if (n > len) {
        n = len;
    }
    if (m->max_read > 0 && n > m->max_read) {
        n = m->max_read;
    }
    if (n > 0 && m->data != NULL) {
        memcpy(buf, m->data + off, n);
    }
    *out_n = n;
    m->nreads++;
    return MS_OK;
}

static void mem_cancel(void *ctx)
{
    ms_mem_source *m = (ms_mem_source *)ctx;
    if (m != NULL) {
        m->canceled = 1;
    }
}

void ms_mem_source_init(ms_mem_source *m, const uint8_t *data, size_t size)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->struct_size = sizeof(*m);
    m->data = data;
    m->size = size;
    m->fail_after = -1;
    m->fail_st = MS_ERR_UNKNOWN;
}

void ms_mem_source_bind(ms_mem_source *m, ms_source *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->ctx = m;
    out->size_fn = mem_size;
    out->read_at_fn = mem_read_at;
    out->cancel_fn = mem_cancel;
}

static ms_status file_size(void *ctx, uint64_t *out_size)
{
    ms_file_source *f = (ms_file_source *)ctx;
    if (f == NULL || out_size == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->canceled) {
        return MS_ERR_CANCELED;
    }
    if (f->fd < 0) {
        return MS_ERR_STATE;
    }
    *out_size = f->size;
    return MS_OK;
}

static ms_status file_read_at(void *ctx, uint64_t offset, uint8_t *buf,
                              size_t len, size_t *out_n)
{
    ms_file_source *f = (ms_file_source *)ctx;
    ssize_t n;

    if (out_n != NULL) {
        *out_n = 0;
    }
    if (f == NULL || (buf == NULL && len != 0) || out_n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->canceled) {
        return MS_ERR_CANCELED;
    }
    if (f->fd < 0) {
        return MS_ERR_STATE;
    }
    if (offset >= f->size) {
        return MS_OK;
    }
    if (len == 0) {
        return MS_OK;
    }
    n = pread(f->fd, buf, len, (off_t)offset);
    if (n < 0) {
        return MS_ERR_UNKNOWN;
    }
    *out_n = (size_t)n;
    return MS_OK;
}

static void file_cancel(void *ctx)
{
    ms_file_source *f = (ms_file_source *)ctx;
    if (f != NULL) {
        f->canceled = 1;
    }
}

ms_status ms_file_source_open(ms_file_source *f, const char *path)
{
    int fd;
    off_t end;

    if (f == NULL || path == NULL || path[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    memset(f, 0, sizeof(*f));
    f->struct_size = sizeof(*f);
    f->fd = -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return MS_ERR_UNKNOWN;
    }
    /* 用 lseek 取大小，避免宿主 libc 的 fstat@GLIBC_2.33 符号。 */
    end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
        close(fd);
        return MS_ERR_UNKNOWN;
    }
    f->fd = fd;
    f->size = (uint64_t)end;
    return MS_OK;
}

void ms_file_source_close(ms_file_source *f)
{
    if (f == NULL) {
        return;
    }
    if (f->fd >= 0) {
        close(f->fd);
    }
    f->fd = -1;
}

void ms_file_source_bind(ms_file_source *f, ms_source *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->ctx = f;
    out->size_fn = file_size;
    out->read_at_fn = file_read_at;
    out->cancel_fn = file_cancel;
}
