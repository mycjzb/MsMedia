/*
 * ms_xml 变异模糊：对 MPD 样本做截断/翻转/插入，跑解析、遍历、拷贝、释放，靠 ASan/LSan 判零泄漏零越界。
 * 另含深嵌套（1 万层，须拒绝且不栈溢出）与实体边界用例。
 * by: Long <mycjzb@gmail.com>
 */
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ull;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)rs; }

static void exercise(const char *b, int n, int *ok)
{
    xmlDocPtr d = xmlReadMemory(b, n, NULL, NULL, 0);
    xmlNodePtr r, c, cp;
    if (!d) return;
    (*ok)++;
    r = xmlDocGetRootElement(d);
    for (c = xmlFirstElementChild(r); c; c = xmlNextElementSibling(c)) {
        xmlChar *v = xmlNodeGetContent(c), *p = xmlGetProp(c, (const xmlChar *)"id");
        xmlFree(v); xmlFree(p);
    }
    cp = xmlCopyNode(r, 1);
    xmlNodeSetContent(cp, (const xmlChar *)"a&amp;b&#0;&#xD800;&#x110000;&bogus;&");
    xmlFreeNode(cp);
    xmlFreeDoc(d);
}

int main(int argc, char **argv)
{
    int i, ok = 0, rounds = 0;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        char *b, *m;
        long n;
        int r;
        if (!f) return 2;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc((size_t)n); m = malloc((size_t)n + 64);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
        for (r = 0; r < 4000; r++) {
            int len = (int)n, k, muts = 1 + (int)(rnd() % 6);
            memcpy(m, b, (size_t)n);
            for (k = 0; k < muts; k++) {
                int pos = (int)(rnd() % (unsigned)len);
                switch (rnd() % 4) {
                case 0: m[pos] = (char)rnd(); break;
                case 1: len = pos + 1; break;
                case 2: m[pos] = "<>&;\"'/![]?-"[rnd() % 12]; break;
                default: if (len + 1 < n + 64) { memmove(m + pos + 1, m + pos, (size_t)(len - pos)); m[pos] = '<'; len++; }
                }
            }
            exercise(m, len, &ok);
            rounds++;
        }
        free(b); free(m);
    }
    {
        int depth = 10000, k, pos = 0;
        char *deep = malloc((size_t)depth * 7 + 16);
        for (k = 0; k < depth; k++) { memcpy(deep + pos, "<a>", 3); pos += 3; }
        for (k = 0; k < depth; k++) { memcpy(deep + pos, "</a>", 4); pos += 4; }
        if (xmlReadMemory(deep, pos, NULL, NULL, 0) != NULL) { printf("FUZZ_FAIL deep accepted\n"); return 1; }
        free(deep);
    }
    { xmlDocPtr d = xmlReadMemory("<a>&#65;&#x42;&lt;</a>", 22, NULL, NULL, 0); xmlChar *c; if (!d) { printf("FUZZ_FAIL simple\n"); return 1; } c = xmlNodeGetContent(xmlDocGetRootElement(d)); if (strcmp((char *)c, "AB<")) { printf("FUZZ_FAIL ent\n"); return 1; } xmlFree(c); xmlFreeDoc(d); }
    printf("FUZZ_OK rounds=%d parsed=%d (deep 10000 rejected)\n", rounds, ok);
    return 0;
}
