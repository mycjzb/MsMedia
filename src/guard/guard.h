#ifndef MS_GUARD_H
#define MS_GUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Soft 防篡改/防调试（对齐超级直播）：ensure 不 _exit，入口查 ms_guard_ok()。 */

int ms_guard_traced(void);
int ms_guard_hostile(void);
int ms_guard_ok(void);

#if defined(__ANDROID__)
const char *ms_guard_block_reason(void);
void ms_guard_force_fail(const char *reason);
#endif

void ms_guard_ensure(void);
void ms_guard_probe_tracer(void);
void ms_guard_probe_maps(void);
void ms_guard_probe_integrity(void);

unsigned ms_guard_taint(void);
void ms_guard_mix_key(unsigned char *key, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MS_GUARD_H */
