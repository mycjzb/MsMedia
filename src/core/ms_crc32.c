/* by: Long <mycjzb@gmail.com> */

#include "ms_crc32.h"

uint32_t ms_crc32_mpeg2(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;

    if (data == NULL && len != 0) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        crc ^= ((uint32_t)data[i]) << 24;
        for (b = 0; b < 8; b++) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04C11DB7u;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
