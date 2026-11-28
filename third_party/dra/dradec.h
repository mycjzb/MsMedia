#ifndef AVCODEC_DRADEC_H
#define AVCODEC_DRADEC_H

typedef struct DRAFrameInfo {
    int nChannels;
    int nSampleRate;
    int nFrameSize;
} DRAFrameInfo;

void *DRADecCreate(void);
void  DRADecDestroy(void *pDecoder);
int   DRADecGetFrameInfo(void *pDecoder, DRAFrameInfo *info);
int   DRADecSendData(void *pDecoder, unsigned char *data, int len);
int   DRADecRecvFrame(void *pDecoder, unsigned char **pcm, int *pcm_len);

#endif /* AVCODEC_DRADEC_H */
