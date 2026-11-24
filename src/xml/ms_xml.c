/*
 * 自研极简纯 C 只读 XML 解析器 + libxml2 子集 DOM 垫片（供 FFmpeg dashdec，替换 libxml2）。
 * 支持：XML 声明/处理指令/注释/CDATA、DOCTYPE（整体跳过，含内部子集，不做任何实体定义与展开）、
 * 嵌套元素、单双引号属性、命名空间前缀（name 取本地名，同 libxml2）、预定义实体与数字字符引用
 * （&#NNN; / &#xHH; → UTF-8）、UTF-8 BOM。未知实体原样保留，不访问网络/文件（无 XXE）。
 * 有界：深度 ≤256、输入 ≤ INT_MAX；任何格式错误返回 NULL 并释放已分配节点（零泄漏）。
 * by: Long <mycjzb@gmail.com>
 */
#include "libxml/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MSX_MAX_DEPTH 256

void xmlFree(void *p) { free(p); }
void *xmlRealloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void *xmlMalloc(size_t n) { return malloc(n ? n : 1); }
void xmlCheckVersion(int version) { (void)version; }
void xmlCleanupParser(void) {}

int xmlStrlen(const xmlChar *s) { return s ? (int)strlen((const char *)s) : 0; }

int xmlStrcmp(const xmlChar *a, const xmlChar *b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcmp((const char *)a, (const char *)b);
}

static xmlChar *dupn(const char *s, size_t n)
{
    xmlChar *d = (xmlChar *)malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static xmlChar *dups(const xmlChar *s) { return s ? dupn((const char *)s, strlen((const char *)s)) : NULL; }

static size_t put_utf8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}

/* 实体反转义：输出不会比输入长（数字引用最短 4 字节 → 最多 4 字节 UTF-8）。 */
static xmlChar *unescape(const char *s, size_t n)
{
    char *o = (char *)malloc(n + 1);
    size_t i = 0, k = 0;
    if (!o) return NULL;
    while (i < n) {
        if (s[i] == '&') {
            const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
            if (semi) {
                size_t el = (size_t)(semi - (s + i)) + 1; /* 含 & 和 ; */
                const char *e = s + i + 1;
                size_t en = el - 2;
                int ok = 1;
                if (en == 2 && !memcmp(e, "lt", 2)) o[k++] = '<';
                else if (en == 2 && !memcmp(e, "gt", 2)) o[k++] = '>';
                else if (en == 3 && !memcmp(e, "amp", 3)) o[k++] = '&';
                else if (en == 4 && !memcmp(e, "quot", 4)) o[k++] = '"';
                else if (en == 4 && !memcmp(e, "apos", 4)) o[k++] = '\'';
                else if (en >= 2 && e[0] == '#') {
                    uint32_t c = 0;
                    size_t j = 1;
                    int hex = (e[1] == 'x' || e[1] == 'X');
                    if (hex) j = 2;
                    if (j >= en) ok = 0;
                    for (; ok && j < en; j++) {
                        char ch = e[j];
                        uint32_t v;
                        if (ch >= '0' && ch <= '9') v = (uint32_t)(ch - '0');
                        else if (hex && ch >= 'a' && ch <= 'f') v = (uint32_t)(ch - 'a' + 10);
                        else if (hex && ch >= 'A' && ch <= 'F') v = (uint32_t)(ch - 'A' + 10);
                        else { ok = 0; break; }
                        c = c * (hex ? 16u : 10u) + v;
                        if (c > 0x10FFFF) ok = 0;
                    }
                    if (ok && (c == 0 || (c >= 0xD800 && c <= 0xDFFF))) ok = 0;
                    if (ok) k += put_utf8(c, o + k);
                } else ok = 0;
                if (ok) { i += el; continue; }
            }
        }
        o[k++] = s[i++];
    }
    o[k] = 0;
    return (xmlChar *)o;
}

static xmlNodePtr new_node(xmlElementType t)
{
    xmlNodePtr n = (xmlNodePtr)calloc(1, sizeof(*n));
    if (n) n->type = t;
    return n;
}

static void append_child(xmlNodePtr parent, xmlNodePtr c)
{
    c->parent = parent;
    c->doc = parent->doc;
    c->prev = parent->last;
    if (parent->last) parent->last->next = c; else parent->children = c;
    parent->last = c;
}

static void free_attrs(xmlAttrPtr a)
{
    while (a) {
        xmlAttrPtr nx = a->next;
        free((void *)a->name);
        free(a->value);
        free(a);
        a = nx;
    }
}

/* 迭代释放子树（不递归，防深树栈溢出）。 */
static void free_tree(xmlNodePtr n)
{
    while (n) {
        if (n->children) {
            xmlNodePtr c = n->children;
            n->children = NULL;
            n->last = NULL;
            /* 把孩子链接到当前兄弟链之前，下一轮继续处理 */
            {
                xmlNodePtr tail = c;
                while (tail->next) tail = tail->next;
                tail->next = n->next;
                n->next = c;
            }
        }
        {
            xmlNodePtr nx = n->next;
            free((void *)n->name);
            free(n->content);
            free(n->prefix);
            free_attrs(n->properties);
            free(n);
            n = nx;
        }
    }
}

void xmlFreeNode(xmlNodePtr cur)
{
    if (!cur) return;
    /* 只释放该节点及其子树，不动兄弟 */
    cur->next = NULL;
    free_tree(cur);
}

void xmlFreeDoc(xmlDocPtr d)
{
    if (!d) return;
    free_tree(d->root);
    free(d->url);
    free(d);
}

typedef struct px {
    const char *s;
    size_t n, i;
    xmlDocPtr doc;
} px;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_name(char c)
{
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           u == '_' || u == ':' || u == '-' || u == '.';
}

static const char *find(px *p, const char *pat)
{
    size_t m = strlen(pat), i;
    if (p->n < m) return NULL;
    for (i = p->i; i + m <= p->n; i++)
        if (p->s[i] == pat[0] && !memcmp(p->s + i, pat, m)) return p->s + i;
    return NULL;
}

/* 读名字，返回本地名（去前缀）副本；*prefix 可选输出前缀。 */
static xmlChar *read_name(px *p, xmlChar **prefix)
{
    size_t b = p->i, colon = (size_t)-1;
    while (p->i < p->n && is_name(p->s[p->i])) {
        if (p->s[p->i] == ':' && colon == (size_t)-1) colon = p->i;
        p->i++;
    }
    if (p->i == b) return NULL;
    if (colon != (size_t)-1 && colon > b && colon + 1 < p->i) {
        if (prefix) *prefix = dupn(p->s + b, colon - b);
        return dupn(p->s + colon + 1, p->i - colon - 1);
    }
    return dupn(p->s + b, p->i - b);
}

static void skip_ws(px *p) { while (p->i < p->n && is_ws(p->s[p->i])) p->i++; }

/* 跳过 <!DOCTYPE ...>，含 [ 内部子集 ]；不解释任何声明。 */
static int skip_doctype(px *p)
{
    int br = 0;
    char q = 0;
    p->i += 9;
    while (p->i < p->n) {
        char c = p->s[p->i++];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '[') br++;
        else if (c == ']') br--;
        else if (c == '>' && br <= 0) return 0;
    }
    return -1;
}

static int add_text(px *p, xmlNodePtr parent, const char *s, size_t n, xmlElementType t)
{
    xmlNodePtr tn;
    if (!parent || n == 0) return 0;
    tn = new_node(t);
    if (!tn) return -1;
    tn->name = dups((const xmlChar *)(t == XML_CDATA_SECTION_NODE ? "cdata-section" :
                                      t == XML_COMMENT_NODE ? "comment" : "text"));
    tn->content = t == XML_TEXT_NODE ? unescape(s, n) : dupn(s, n);
    if (!tn->name || !tn->content) { xmlFreeNode(tn); return -1; }
    append_child(parent, tn);
    (void)p;
    return 0;
}

static int parse_attrs(px *p, xmlNodePtr el, int *self_close)
{
    xmlAttrPtr last = NULL;
    for (;;) {
        xmlChar *nm, *pre = NULL;
        char q;
        size_t vb;
        const char *ve;
        xmlAttrPtr a;
        skip_ws(p);
        if (p->i >= p->n) return -1;
        if (p->s[p->i] == '>') { p->i++; *self_close = 0; return 0; }
        if (p->s[p->i] == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '>') { p->i += 2; *self_close = 1; return 0; }
        nm = read_name(p, &pre);
        if (!nm) { free(pre); return -1; }
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != '=') { free(pre); free(nm); return -1; }
        p->i++;
        skip_ws(p);
        if (p->i >= p->n || (p->s[p->i] != '"' && p->s[p->i] != '\'')) { free(pre); free(nm); return -1; }
        q = p->s[p->i++];
        vb = p->i;
        ve = memchr(p->s + vb, q, p->n - vb);
        if (!ve) { free(pre); free(nm); return -1; }
        p->i = (size_t)(ve - p->s) + 1;
        /* xmlns / xmlns:x 是命名空间声明，同 libxml2 不进 properties。 */
        if ((pre && !strcmp((const char *)pre, "xmlns")) || (!pre && !strcmp((const char *)nm, "xmlns"))) {
            free(pre);
            free(nm);
            continue;
        }
        free(pre);
        a = (xmlAttrPtr)calloc(1, sizeof(*a));
        if (!a) { free(nm); return -1; }
        a->type = XML_ATTRIBUTE_NODE;
        a->name = nm;
        a->parent = el;
        a->value = unescape(p->s + vb, (size_t)(ve - p->s) - vb);
        if (last) last->next = a; else el->properties = a;
        last = a;
        if (!a->value) return -1;
    }
}

xmlDocPtr xmlReadMemory(const char *buffer, int size, const char *url,
                        const char *encoding, int options)
{
    px p;
    xmlNodePtr stack[MSX_MAX_DEPTH];
    xmlNodePtr holder; /* 文档伪根，收集顶层元素 */
    int depth = 0;
    xmlDocPtr d;
    (void)encoding;
    (void)options;
    if (!buffer || size <= 0) return NULL;
    memset(&p, 0, sizeof(p));
    p.s = buffer;
    p.n = (size_t)size;
    if (p.n >= 3 && (unsigned char)p.s[0] == 0xEF && (unsigned char)p.s[1] == 0xBB && (unsigned char)p.s[2] == 0xBF)
        p.i = 3;
    d = (xmlDocPtr)calloc(1, sizeof(*d));
    holder = new_node(XML_DOCUMENT_NODE);
    if (!d || !holder) { free(d); free(holder); return NULL; }
    d->type = XML_DOCUMENT_NODE;
    if (url) d->url = (char *)dups((const xmlChar *)url);
    holder->doc = d;
    stack[0] = holder;
    while (p.i < p.n) {
        if (p.s[p.i] != '<') {
            const char *lt = memchr(p.s + p.i, '<', p.n - p.i);
            size_t e = lt ? (size_t)(lt - p.s) : p.n;
            if (depth > 0 && add_text(&p, stack[depth], p.s + p.i, e - p.i, XML_TEXT_NODE) != 0) goto fail;
            if (depth == 0) {
                size_t k;
                for (k = p.i; k < e; k++) if (!is_ws(p.s[k])) goto fail; /* 根外非空白文本 */
            }
            p.i = e;
            continue;
        }
        if (p.n - p.i >= 4 && !memcmp(p.s + p.i, "<!--", 4)) {
            const char *e;
            p.i += 4;
            e = find(&p, "-->");
            if (!e) goto fail;
            if (depth > 0 && add_text(&p, stack[depth], p.s + p.i, (size_t)(e - p.s) - p.i, XML_COMMENT_NODE) != 0) goto fail;
            p.i = (size_t)(e - p.s) + 3;
            continue;
        }
        if (p.n - p.i >= 9 && !memcmp(p.s + p.i, "<![CDATA[", 9)) {
            const char *e;
            p.i += 9;
            e = find(&p, "]]>");
            if (!e || depth == 0) goto fail;
            if (add_text(&p, stack[depth], p.s + p.i, (size_t)(e - p.s) - p.i, XML_CDATA_SECTION_NODE) != 0) goto fail;
            p.i = (size_t)(e - p.s) + 3;
            continue;
        }
        if (p.n - p.i >= 9 && !memcmp(p.s + p.i, "<!DOCTYPE", 9)) {
            if (depth != 0 || skip_doctype(&p) != 0) goto fail;
            continue;
        }
        if (p.n - p.i >= 2 && p.s[p.i + 1] == '?') {
            const char *e;
            p.i += 2;
            e = find(&p, "?>");
            if (!e) goto fail;
            p.i = (size_t)(e - p.s) + 2;
            continue;
        }
        if (p.n - p.i >= 2 && p.s[p.i + 1] == '/') {
            xmlChar *nm, *pre = NULL;
            p.i += 2;
            nm = read_name(&p, &pre);
            free(pre);
            if (!nm || depth == 0 || strcmp((const char *)nm, (const char *)stack[depth]->name) != 0) { free(nm); goto fail; }
            free(nm);
            skip_ws(&p);
            if (p.i >= p.n || p.s[p.i] != '>') goto fail;
            p.i++;
            depth--;
            continue;
        }
        {
            xmlNodePtr el;
            int sc = 0;
            xmlChar *pre = NULL;
            p.i++;
            if (depth == 0 && holder->children) goto fail; /* 只允许一个根元素 */
            if (depth + 1 >= MSX_MAX_DEPTH) goto fail;
            el = new_node(XML_ELEMENT_NODE);
            if (!el) goto fail;
            el->name = read_name(&p, &pre);
            el->prefix = pre;
            append_child(stack[depth], el);
            if (!el->name) goto fail;
            if (parse_attrs(&p, el, &sc) != 0) goto fail;
            if (!sc) stack[++depth] = el;
        }
    }
    if (depth != 0 || !holder->children) goto fail;
    d->root = holder->children;
    d->root->parent = NULL;
    holder->children = NULL;
    free(holder);
    return d;
fail:
    free_tree(holder);
    free(d->url);
    free(d);
    return NULL;
}

xmlNodePtr xmlDocGetRootElement(const xmlDoc *doc) { return doc ? doc->root : NULL; }

xmlNodePtr xmlFirstElementChild(xmlNodePtr parent)
{
    xmlNodePtr c;
    if (!parent) return NULL;
    for (c = parent->children; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE) return c;
    return NULL;
}

xmlNodePtr xmlNextElementSibling(xmlNodePtr node)
{
    xmlNodePtr c;
    if (!node) return NULL;
    for (c = node->next; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE) return c;
    return NULL;
}

xmlChar *xmlGetProp(const xmlNode *node, const xmlChar *name)
{
    const xmlAttr *a;
    if (!node || !name || node->type != XML_ELEMENT_NODE) return NULL;
    for (a = node->properties; a; a = a->next)
        if (!strcmp((const char *)a->name, (const char *)name)) return dups(a->value);
    return NULL;
}

/* 元素：拼接全部后代文本/CDATA（迭代深度优先）；文本类节点：自身内容。 */
xmlChar *xmlNodeGetContent(const xmlNode *cur)
{
    size_t len = 0, k = 0;
    const xmlNode *n;
    char *o = NULL;
    int pass;
    if (!cur) return NULL;
    if (cur->type != XML_ELEMENT_NODE) return dups(cur->content ? cur->content : (const xmlChar *)"");
    for (pass = 0; pass < 2; pass++) {
        n = cur->children;
        while (n) {
            if ((n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) && n->content) {
                size_t l = strlen((const char *)n->content);
                if (pass) memcpy(o + k, n->content, l);
                k += l;
            }
            if (n->type == XML_ELEMENT_NODE && n->children) { n = n->children; continue; }
            while (n && n != cur && !n->next) n = n->parent;
            if (!n || n == cur) break;
            n = n->next;
        }
        if (!pass) {
            len = k;
            k = 0;
            o = (char *)malloc(len + 1);
            if (!o) return NULL;
        }
    }
    o[len] = 0;
    return (xmlChar *)o;
}

void xmlNodeSetContent(xmlNodePtr cur, const xmlChar *content)
{
    xmlNodePtr t;
    if (!cur) return;
    if (cur->type != XML_ELEMENT_NODE) {
        free(cur->content);
        cur->content = dups(content ? content : (const xmlChar *)"");
        return;
    }
    if (cur->children) {
        free_tree(cur->children);
        cur->children = cur->last = NULL;
    }
    if (!content) return;
    t = new_node(XML_TEXT_NODE);
    if (!t) return;
    t->name = dups((const xmlChar *)"text");
    t->content = unescape((const char *)content, strlen((const char *)content)); /* 同 libxml2：内容允许实体引用 */
    if (!t->name || !t->content) { xmlFreeNode(t); return; }
    append_child(cur, t);
}

xmlChar *xmlEncodeSpecialChars(const xmlDoc *doc, const xmlChar *in)
{
    size_t n = 0, k = 0;
    const xmlChar *s;
    char *o;
    (void)doc;
    if (!in) return NULL;
    for (s = in; *s; s++) {
        switch (*s) {
        case '<': case '>': n += 4; break;
        case '&': n += 5; break;
        case '"': n += 6; break;
        case '\r': n += 5; break;
        default: n++;
        }
    }
    o = (char *)malloc(n + 1);
    if (!o) return NULL;
    for (s = in; *s; s++) {
        const char *r = NULL;
        switch (*s) {
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '&': r = "&amp;"; break;
        case '"': r = "&quot;"; break;
        case '\r': r = "&#13;"; break;
        default: o[k++] = (char)*s; continue;
        }
        memcpy(o + k, r, strlen(r));
        k += strlen(r);
    }
    o[k] = 0;
    return (xmlChar *)o;
}

xmlNodePtr xmlNewNode(xmlNsPtr ns, const xmlChar *name)
{
    xmlNodePtr n;
    (void)ns;
    if (!name) return NULL;
    n = new_node(XML_ELEMENT_NODE);
    if (!n) return NULL;
    n->name = dups(name);
    if (!n->name) { free(n); return NULL; }
    return n;
}

static xmlNodePtr copy_one(const xmlNode *s)
{
    xmlNodePtr n = new_node(s->type);
    const xmlAttr *a;
    xmlAttrPtr last = NULL;
    if (!n) return NULL;
    n->name = dups(s->name);
    n->content = dups(s->content);
    n->prefix = dups(s->prefix);
    if ((s->name && !n->name) || (s->content && !n->content) || (s->prefix && !n->prefix)) { xmlFreeNode(n); return NULL; }
    for (a = s->properties; a; a = a->next) {
        xmlAttrPtr c = (xmlAttrPtr)calloc(1, sizeof(*c));
        if (!c) { xmlFreeNode(n); return NULL; }
        c->type = XML_ATTRIBUTE_NODE;
        c->parent = n;
        if (last) last->next = c; else n->properties = c;
        last = c;
        c->name = dups(a->name);
        c->value = dups(a->value);
        if (!c->name || !c->value) { xmlFreeNode(n); return NULL; }
    }
    return n;
}

/* extended=1 深拷贝（迭代，不递归）；0 只拷节点本身与属性。 */
xmlNodePtr xmlCopyNode(xmlNodePtr node, int extended)
{
    xmlNodePtr root, src, dst;
    if (!node) return NULL;
    root = copy_one(node);
    if (!root || !extended) return root;
    src = node->children;
    dst = root;
    while (src) {
        xmlNodePtr c = copy_one(src);
        if (!c) { xmlFreeNode(root); return NULL; }
        append_child(dst, c);
        c->doc = NULL;
        if (src->children) { src = src->children; dst = c; continue; }
        while (src != node && !src->next) {
            src = src->parent;
            dst = dst->parent;
        }
        if (src == node) break;
        src = src->next;
    }
    return root;
}
