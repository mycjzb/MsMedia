/*
 * XML 垫片差分测试工具：同一份代码分别对真 libxml2 和自研 src/xml/ms_xml.c 编译，
 * 只用 dashdec 用到的 API 遍历并规范化输出，由 single_xml_test.sh 逐字节对比。
 * 另做：xmlCopyNode 深拷贝、xmlNodeSetContent(xmlEncodeSpecialChars())、xmlNewNode/xmlFreeNode。
 * by: Long <mycjzb@gmail.com>
 */
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump(xmlNodePtr n, int d)
{
    for (; n; n = n->next) {
        if (n->type == XML_ELEMENT_NODE) {
            xmlAttrPtr a;
            xmlChar *c = xmlNodeGetContent(n);
            printf("%*sE %s content_len=%d\n", d * 2, "", (const char *)n->name, xmlStrlen(c));
            xmlFree(c);
            for (a = n->properties; a; a = a->next) {
                xmlChar *v = xmlGetProp(n, a->name);
                printf("%*s@%s=[%s]\n", d * 2 + 1, "", (const char *)a->name, v ? (const char *)v : "(null)");
                xmlFree(v);
            }
            dump(n->children, d + 1);
        } else if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
            xmlChar *c = xmlNodeGetContent(n);
            printf("%*sT%d [%s]\n", d * 2, "", (int)n->type, c ? (const char *)c : "(null)");
            xmlFree(c);
        }
    }
}

static void elem_walk(xmlNodePtr n, int d)
{
    for (n = xmlFirstElementChild(n); n; n = xmlNextElementSibling(n)) {
        printf("%*sW %s\n", d, "", (const char *)n->name);
        elem_walk(n, d + 1);
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    char *buf;
    long n;
    xmlDocPtr doc;
    xmlNodePtr root, cp, nn;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    LIBXML_TEST_VERSION
    doc = xmlReadMemory(buf, (int)n, "http://example.invalid/x.mpd", NULL, 0);
    if (!doc) { printf("PARSE_FAIL\n"); free(buf); return 0; }
    root = xmlDocGetRootElement(doc);
    printf("ROOT %s type=%d\n", (const char *)root->name, (int)root->type);
    dump(root, 0);
    elem_walk(root, 0);
    cp = xmlCopyNode(root, 1);
    {
        xmlChar *a = xmlNodeGetContent(root), *b = xmlNodeGetContent(cp);
        printf("COPY_EQ %d\n", strcmp((char *)a, (char *)b) == 0);
        xmlFree(a); xmlFree(b);
    }
    {
        xmlChar *esc = xmlEncodeSpecialChars(NULL, (const xmlChar *)"http://h/a?x=1&y=<2>\"z\"/");
        xmlChar *c;
        nn = xmlNewNode(NULL, (const xmlChar *)"BaseURL");
        printf("ESC [%s]\n", (char *)esc);
        xmlNodeSetContent(nn, esc);
        xmlNodeSetContent(cp, esc);
        xmlFree(esc);
        c = xmlNodeGetContent(nn);
        printf("SET [%s] child_text=%d\n", (char *)c, nn->children && nn->children->type == XML_TEXT_NODE);
        xmlFree(c);
        c = xmlNodeGetContent(cp);
        printf("SET2 [%s]\n", (char *)c);
        xmlFree(c);
        c = xmlRealloc(xmlNodeGetContent(nn), 200);
        strcat((char *)c, "/");
        printf("REALLOC [%s]\n", (char *)c);
        xmlFree(c);
    }
    xmlFreeNode(nn);
    xmlFreeNode(cp);
    xmlFreeDoc(doc);
    xmlCleanupParser();
    free(buf);
    return 0;
}
