/*
 * 自研极简只读 XML DOM 的 libxml2 兼容头（只覆盖 FFmpeg dashdec 用到的子集）。
 * 字段名与 libxml2 一致（type/name/children/next/properties…），布局不保证二进制兼容，
 * 只能与本仓库 src/xml/ms_xml.c 一起编译链接，不得与真 libxml2 混用。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_XML_SHIM_TREE_H
#define MS_XML_SHIM_TREE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char xmlChar;

typedef enum {
    XML_ELEMENT_NODE = 1,
    XML_ATTRIBUTE_NODE = 2,
    XML_TEXT_NODE = 3,
    XML_CDATA_SECTION_NODE = 4,
    XML_PI_NODE = 7,
    XML_COMMENT_NODE = 8,
    XML_DOCUMENT_NODE = 9
} xmlElementType;

struct _xmlNode;
struct _xmlDoc;

typedef struct _xmlAttr {
    void *_private;
    xmlElementType type;
    const xmlChar *name;      /* 本地名（已去命名空间前缀，同 libxml2） */
    struct _xmlNode *parent;
    struct _xmlAttr *next;
    xmlChar *value;           /* 已做实体反转义 */
} xmlAttr;
typedef xmlAttr *xmlAttrPtr;

typedef struct _xmlNode {
    void *_private;
    xmlElementType type;
    const xmlChar *name;      /* 元素本地名；文本节点为 "text" */
    struct _xmlNode *children;
    struct _xmlNode *last;
    struct _xmlNode *parent;
    struct _xmlNode *next;
    struct _xmlNode *prev;
    struct _xmlDoc *doc;
    xmlChar *content;         /* 文本/CDATA/注释内容 */
    struct _xmlAttr *properties;
    xmlChar *prefix;          /* 命名空间前缀（可空） */
} xmlNode;
typedef xmlNode *xmlNodePtr;

typedef struct _xmlDoc {
    void *_private;
    xmlElementType type;
    struct _xmlNode *root;
    char *url;
} xmlDoc;
typedef xmlDoc *xmlDocPtr;

typedef void *xmlNsPtr;

xmlNodePtr xmlDocGetRootElement(const xmlDoc *doc);
xmlNodePtr xmlFirstElementChild(xmlNodePtr parent);
xmlNodePtr xmlNextElementSibling(xmlNodePtr node);
xmlChar *xmlGetProp(const xmlNode *node, const xmlChar *name);
xmlChar *xmlNodeGetContent(const xmlNode *cur);
void xmlNodeSetContent(xmlNodePtr cur, const xmlChar *content);
xmlChar *xmlEncodeSpecialChars(const xmlDoc *doc, const xmlChar *input);
xmlNodePtr xmlCopyNode(xmlNodePtr node, int extended);
xmlNodePtr xmlNewNode(xmlNsPtr ns, const xmlChar *name);
void xmlFreeNode(xmlNodePtr cur);
void xmlFreeDoc(xmlDocPtr cur);
int xmlStrlen(const xmlChar *str);
int xmlStrcmp(const xmlChar *a, const xmlChar *b);

#ifdef __cplusplus
}
#endif
#endif
