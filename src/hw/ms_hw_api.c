/*
 * 见 ms_hw_api.h。by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_api.h"

#ifdef __ANDROID__
#include "ms_hw_mediandk.h"
#include <stdlib.h>
#include <sys/system_properties.h>

int ms_hw_device_sdk_int(void)
{
    char v[PROP_VALUE_MAX];
    int n, x;
    v[0] = 0;
    n = __system_property_get("ro.build.version.sdk", v);
    if (n <= 0) {
        return 0;
    }
    x = atoi(v);
    return (x > 0) ? x : 0;
}

int ms_hw_use_ndk_mediacodec(void)
{
    int sdk = ms_hw_device_sdk_int();
    /* API 18–20 没有 libmediandk.so。API 21+ 仅在符号解析成功时走 NDK 路径。 */
    if (sdk < 21) {
        return 0;
    }
    return ms_hw_mediandk_available();
}

#else
int ms_hw_device_sdk_int(void)
{
    return 0;
}

int ms_hw_use_ndk_mediacodec(void)
{
    return 0;
}
#endif
