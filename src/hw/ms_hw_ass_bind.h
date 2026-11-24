#ifndef MSMEDIA_MS_HW_ASS_BIND_H
#define MSMEDIA_MS_HW_ASS_BIND_H
/*
 * 播放器侧可选 libmsass 绑定（dlopen，不 NEEDED 链入 libmshw）。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "msmedia/ms_hw.h"

typedef struct ms_hw_ass_bind ms_hw_ass_bind;

int ms_hw_ass_bind_probe(void);
ms_status ms_hw_ass_bind_ensure(const ms_allocator *alloc, ms_hw_ass_bind **io);
void ms_hw_ass_bind_destroy(ms_hw_ass_bind *b);
int ms_hw_ass_bind_loaded(const ms_hw_ass_bind *b);
int ms_hw_ass_bind_has_script(const ms_hw_ass_bind *b);
ms_status ms_hw_ass_bind_set_header(ms_hw_ass_bind *b, const uint8_t *data, size_t size);
ms_status ms_hw_ass_bind_set_script(ms_hw_ass_bind *b, const uint8_t *data, size_t size);
ms_status ms_hw_ass_bind_add_event(ms_hw_ass_bind *b, const uint8_t *data, size_t size,
                                   int has_times, int64_t start_90k, int64_t end_90k);
ms_status ms_hw_ass_bind_clear(ms_hw_ass_bind *b);
ms_status ms_hw_ass_bind_set_canvas(ms_hw_ass_bind *b, int w, int h);
ms_status ms_hw_ass_bind_frame_at(ms_hw_ass_bind *b, uint64_t time_90k, ms_hw_ass_frame *out);

#endif
