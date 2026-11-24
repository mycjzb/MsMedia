/* 自研 XML 垫片：版本头（供 FFmpeg configure 的 xmlCheckVersion 检测）。by: Long <mycjzb@gmail.com> */
#ifndef MS_XML_SHIM_VERSION_H
#define MS_XML_SHIM_VERSION_H
#ifdef __cplusplus
extern "C" {
#endif
#define LIBXML_VERSION 21308
#define LIBXML_DOTTED_VERSION "2.13.8-msxml"
void xmlCheckVersion(int version);
#define LIBXML_TEST_VERSION xmlCheckVersion(LIBXML_VERSION);
#ifdef __cplusplus
}
#endif
#endif
