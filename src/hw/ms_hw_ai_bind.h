/*
 * 播放器侧可选 libmsai 绑定（dlopen，不 NEEDED 链入 libmshw）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_HW_AI_BIND_H
#define MSMEDIA_MS_HW_AI_BIND_H

#include "msmedia/msmedia.h"
#include "msmedia/ms_hw.h"
#include "msmedia/ms_ai_sub.h"

typedef struct ms_hw_ai_bind ms_hw_ai_bind;

int ms_hw_ai_bind_probe(void);
ms_status ms_hw_ai_bind_ensure(const ms_allocator *alloc, ms_hw_ai_bind **io);
void ms_hw_ai_bind_destroy(ms_hw_ai_bind *b);
int ms_hw_ai_bind_loaded(const ms_hw_ai_bind *b);
ms_status ms_hw_ai_bind_configure(ms_hw_ai_bind *b, const ms_ai_sub_opts *opts);
ms_status ms_hw_ai_bind_use_fake_local(ms_hw_ai_bind *b);
ms_status ms_hw_ai_bind_set_enabled(ms_hw_ai_bind *b, int on);
int ms_hw_ai_bind_enabled(const ms_hw_ai_bind *b);
void ms_hw_ai_bind_attach_audio(ms_hw_ai_bind *b, struct ms_hw_audio *a);
void ms_hw_ai_bind_set_epochs(ms_hw_ai_bind *b, uint32_t session, uint32_t timeline,
                              uint32_t selection);
void ms_hw_ai_bind_bump_timeline(ms_hw_ai_bind *b);
void ms_hw_ai_bind_bump_selection(ms_hw_ai_bind *b);
ms_status ms_hw_ai_bind_drive(ms_hw_ai_bind *b);
void ms_hw_ai_bind_flush(ms_hw_ai_bind *b);
ms_sub_doc *ms_hw_ai_bind_doc(ms_hw_ai_bind *b);
const char *ms_hw_ai_bind_status(const ms_hw_ai_bind *b);
ms_status ms_hw_ai_bind_query_caps(const ms_hw_ai_bind *b, ms_ai_vendor_caps *out);
ms_status ms_hw_ai_bind_get_stats(const ms_hw_ai_bind *b, ms_ai_sub_stats *out);
ms_ai_sub *ms_hw_ai_bind_engine(ms_hw_ai_bind *b);

#endif
