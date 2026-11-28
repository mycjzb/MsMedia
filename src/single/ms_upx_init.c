/*
 * UPX 3.96 --android-shlib 挂法对齐超级直播 libaliiptv：
 *   - 要有 DT_INIT（空 _init；纯 C/lld 默认没有）
 *   - 同时留 init_array（constructor），与 aliiptv 一样给壳挂点
 * 链接行：-Wl,-init=_init -Wl,-u,_init -Wl,-e,0
 * by: Long <mycjzb@gmail.com>
 */
__attribute__((used)) void _init(void) {}

__attribute__((constructor(101))) static void ms_upx_ctor(void) {}
