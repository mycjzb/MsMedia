/* 自研 XML 垫片：解析入口（只读、无网络、无外部实体、无 DTD 展开）。by: Long <mycjzb@gmail.com> */
#ifndef MS_XML_SHIM_PARSER_H
#define MS_XML_SHIM_PARSER_H
#include "xmlversion.h"
#include "tree.h"
#include "xmlmemory.h"
#ifdef __cplusplus
extern "C" {
#endif
xmlDocPtr xmlReadMemory(const char *buffer, int size, const char *url,
                        const char *encoding, int options);
void xmlCleanupParser(void);
#ifdef __cplusplus
}
#endif
#endif
