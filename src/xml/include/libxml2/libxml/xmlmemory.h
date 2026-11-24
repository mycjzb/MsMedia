/* 自研 XML 垫片：内存接口（xmlFree/xmlRealloc 即 libc free/realloc）。by: Long <mycjzb@gmail.com> */
#ifndef MS_XML_SHIM_MEMORY_H
#define MS_XML_SHIM_MEMORY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void xmlFree(void *p);
void *xmlRealloc(void *p, size_t n);
void *xmlMalloc(size_t n);
#ifdef __cplusplus
}
#endif
#endif
