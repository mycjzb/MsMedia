/*
 * libtvcore.so（TVBus / SOPCast）加载器：证书校验绕过 + 手动 JNI 绑定。
 *
 * 移植自 bs-player/jni/p2p-m3/src/ms_tvb.c（超级直播验证过的做法），
 * 去掉内嵌 blob 分支（本 SDK 的 so 随 APK jniLibs 交付，按绝对路径加载即可）。
 *
 * 顺序：dlopen(软捕获) → GOT[memcmp] hook → tvcore JNI_OnLoad → RegisterNatives。
 * hook 必须在 JNI_OnLoad 之前，因为签名校验在引擎初始化早期就会跑。
 *
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_tvb.h"

#include "guard.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#define TVB_TAG "MsTvb"
#define TVB_LOG(...) __android_log_print(ANDROID_LOG_INFO, TVB_TAG, __VA_ARGS__)
#else
#define TVB_LOG(...) ((void)0)
#endif

/** tvcore 导出的初始化符号；hook 只认「从它附近 0x400 字节内发起的 16 字节 memcmp」。 */
static const char k_tvb_init_sym[] = "Java_com_tvbus_engine_TVCore_initialise";

static int (*g_real_memcmp)(const void *, const void *, size_t);
static uintptr_t g_init;
static const uintptr_t g_init_span = 0x400;
static int g_tvb_ok;

int ms_tvb_ok(void)
{
    return g_tvb_ok;
}

/* ---------- 软捕获 dlopen（SIGBUS / SIGSEGV / SIGABRT） ---------- */

static sigjmp_buf g_dl_jmp;
static volatile sig_atomic_t g_dl_armed;

static void dl_on_crash(int sig)
{
    (void)sig;
    if (g_dl_armed) {
        siglongjmp(g_dl_jmp, 1);
    }
}

static int under_native_bridge(void)
{
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX];
    if (__system_property_get("ro.dalvik.vm.native.bridge", v) > 0 && v[0] &&
        strcmp(v, "0") != 0) {
        return 1;
    }
    if (__system_property_get("ro.berberis.version", v) > 0 && v[0]) {
        return 1;
    }
#endif
    return 0;
}

struct sig_guard {
    struct sigaction bus, seg, abrt;
};

static void guard_begin(struct sig_guard *g)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dl_on_crash;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g->bus);
    sigaction(SIGSEGV, &sa, &g->seg);
    sigaction(SIGABRT, &sa, &g->abrt);
    g_dl_armed = 1;
}

static void guard_end(struct sig_guard *g)
{
    g_dl_armed = 0;
    sigaction(SIGBUS, &g->bus, NULL);
    sigaction(SIGSEGV, &g->seg, NULL);
    sigaction(SIGABRT, &g->abrt, NULL);
}

static void *dlopen_soft(const char *path, int flags, int *crashed_out)
{
    struct sig_guard sg;
    void *h = NULL;
    if (crashed_out) {
        *crashed_out = 0;
    }
    guard_begin(&sg);
    if (sigsetjmp(g_dl_jmp, 1) == 0) {
        h = dlopen(path, flags);
    } else {
        TVB_LOG("dlopen crash, skip tvbus path=%s", path ? path : "");
        h = NULL;
        if (crashed_out) {
            *crashed_out = 1;
        }
    }
    guard_end(&sg);
    return h;
}

/* ---------- memcmp GOT hook ---------- */

static int hooked_memcmp(const void *a, const void *b, size_t n)
{
    if (n == 16 && g_init) {
        uintptr_t ra = (uintptr_t)__builtin_return_address(0);
        if (ra >= g_init && ra < g_init + g_init_span) {
            TVB_LOG("cert guard memcmp ok +0x%lx", (unsigned long)(ra - g_init));
            return 0;
        }
    }
    return g_real_memcmp ? g_real_memcmp(a, b, n) : 1;
}

static uintptr_t base_of(void *sym)
{
    Dl_info info;
    if (sym && dladdr(sym, &info) && info.dli_fbase) {
        return (uintptr_t)info.dli_fbase;
    }
    return 0;
}

static ElfW(Dyn) *elf_dynamic(uintptr_t base)
{
    ElfW(Ehdr) *eh = (ElfW(Ehdr) *)base;
    ElfW(Phdr) *ph;
    int i;
    if (!eh || eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F') {
        return 0;
    }
    ph = (ElfW(Phdr) *)(base + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            return (ElfW(Dyn) *)(base + ph[i].p_vaddr);
        }
    }
    return 0;
}

/* 动态表里有的项是绝对地址，有的仍是相对基址。 */
static uintptr_t dyn_fix(uintptr_t base, ElfW(Addr) v)
{
    return (v && v < base) ? base + v : (uintptr_t)v;
}

static int patch_got(void **got, void *hook)
{
    long ps = sysconf(_SC_PAGESIZE);
    if (ps < 4096) {
        ps = 4096;
    }
    if (mprotect((void *)((uintptr_t)got & ~(uintptr_t)(ps - 1)), (size_t)ps,
                 PROT_READ | PROT_WRITE) != 0) {
        TVB_LOG("mprotect RW fail errno=%d", errno);
        return -1;
    }
    g_real_memcmp = (int (*)(const void *, const void *, size_t))*got;
    *got = hook;
    __builtin___clear_cache((char *)got, (char *)got + sizeof(void *));
    (void)mprotect((void *)((uintptr_t)got & ~(uintptr_t)(ps - 1)), (size_t)ps,
                   PROT_READ | PROT_EXEC);
    TVB_LOG("GOT[memcmp] <- hook (orig %p)", (void *)g_real_memcmp);
    return 0;
}

static int patch_memcmp(uintptr_t base, ElfW(Dyn) *dyn)
{
    ElfW(Sym) *symtab = 0;
    const char *strtab = 0;
    void *jmprel = 0;
    size_t pltrelsz = 0;
    ElfW(Dyn) *d;

    for (d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:
            symtab = (ElfW(Sym) *)dyn_fix(base, d->d_un.d_ptr);
            break;
        case DT_STRTAB:
            strtab = (const char *)dyn_fix(base, d->d_un.d_ptr);
            break;
        case DT_JMPREL:
            jmprel = (void *)dyn_fix(base, d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            pltrelsz = (size_t)d->d_un.d_val;
            break;
        default:
            break;
        }
    }
    if (!symtab || !strtab || !jmprel || !pltrelsz) {
        TVB_LOG("plt table missing sym=%p str=%p jmp=%p sz=%zu", (void *)symtab,
                (const void *)strtab, jmprel, pltrelsz);
        return -1;
    }

#if defined(__LP64__)
    {
        size_t n = pltrelsz / sizeof(Elf64_Rela);
        Elf64_Rela *rel = (Elf64_Rela *)jmprel;
        size_t i;
        for (i = 0; i < n; i++) {
            const char *name;
            if (ELF64_R_TYPE(rel[i].r_info) != R_AARCH64_JUMP_SLOT) {
                continue;
            }
            name = strtab + symtab[ELF64_R_SYM(rel[i].r_info)].st_name;
            if (strcmp(name, "memcmp") != 0) {
                continue;
            }
            return patch_got((void **)(base + rel[i].r_offset), (void *)hooked_memcmp);
        }
    }
#else
    {
        size_t n = pltrelsz / sizeof(Elf32_Rel);
        Elf32_Rel *rel = (Elf32_Rel *)jmprel;
        size_t i;
        for (i = 0; i < n; i++) {
            const char *name;
            if (ELF32_R_TYPE(rel[i].r_info) != R_ARM_JUMP_SLOT) {
                continue;
            }
            name = strtab + symtab[ELF32_R_SYM(rel[i].r_info)].st_name;
            if (strcmp(name, "memcmp") != 0) {
                continue;
            }
            return patch_got((void **)(base + rel[i].r_offset), (void *)hooked_memcmp);
        }
    }
#endif
    TVB_LOG("memcmp PLT not found");
    return -1;
}

/* ---------- JNI 绑定 ---------- */

struct onload_arg {
    JavaVM *vm;
    void *handle;
    jint ver;
    int have;
};

static void onload_body(void *p)
{
    struct onload_arg *a = (struct onload_arg *)p;
    typedef jint (*onload_fn)(JavaVM *, void *);
    onload_fn onload = (onload_fn)dlsym(a->handle, "JNI_OnLoad");
    if (!onload) {
        a->have = 0;
        return;
    }
    a->ver = onload(a->vm, NULL);
    a->have = 1;
}

static int run_soft(void (*fn)(void *), void *arg, int *crashed_out)
{
    struct sig_guard sg;
    int ok = 0;
    if (crashed_out) {
        *crashed_out = 0;
    }
    guard_begin(&sg);
    if (sigsetjmp(g_dl_jmp, 1) == 0) {
        fn(arg);
        ok = 1;
    } else if (crashed_out) {
        *crashed_out = 1;
    }
    guard_end(&sg);
    return ok;
}

/** Berberis / native bridge：getSignHashCode 入口改成 ret，跳过 processMapFile 读 maps。 */
static void stub_signhash(void *handle)
{
    void *fn = dlsym(handle, "_Z15getSignHashCodeP7_JNIEnvP8_jobjectRSs");
    long ps;
    uintptr_t page;
    uint32_t ret = 0xD65F03C0u; /* arm64: ret */
    if (!fn) {
        TVB_LOG("getSignHashCode symbol missing");
        return;
    }
    ps = sysconf(_SC_PAGESIZE);
    if (ps < 4096) {
        ps = 4096;
    }
    page = (uintptr_t)fn & ~(uintptr_t)(ps - 1);
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
        TVB_LOG("stub mprotect RW fail");
        return;
    }
    memcpy(fn, &ret, sizeof(ret));
    __builtin___clear_cache((char *)fn, (char *)fn + 4);
    (void)mprotect((void *)page, (size_t)ps, PROT_READ | PROT_EXEC);
    TVB_LOG("stub getSignHashCode (native bridge)");
}

/** 把 tvcore 导出的 Java_* 绑到 com/tvbus/engine/TVCore（不经 System.load）。 */
static int register_tvcore(JNIEnv *env, void *handle)
{
    jclass cls = (*env)->FindClass(env, "com/tvbus/engine/TVCore");
    void *err2;
    JNINativeMethod m[32];
    size_t n = 0;
    size_t i;
    int missing = 0;

    if (!cls) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        TVB_LOG("FindClass com/tvbus/engine/TVCore fail");
        return -1;
    }
#undef TVB_BIND
#define TVB_BIND(nm, sig)                                                  \
    do {                                                                   \
        m[n].name = (char *)(nm);                                          \
        m[n].signature = (char *)(sig);                                     \
        m[n].fnPtr = dlsym(handle, "Java_com_tvbus_engine_TVCore_" nm);     \
        if (!m[n].fnPtr) {                                                 \
            TVB_LOG("missing native %s", nm);                              \
            missing = 1;                                                   \
        }                                                                  \
        n++;                                                               \
    } while (0)

    TVB_BIND("initialise", "()J");
    TVB_BIND("setListener", "(JLcom/tvbus/engine/TVListener;)V");
    TVB_BIND("setPlayPort", "(JI)V");
    TVB_BIND("setServPort", "(JI)V");
    TVB_BIND("setInboundConns", "(JI)V");
    TVB_BIND("setTotalConns", "(JI)V");
    TVB_BIND("setRunningMode", "(JI)V");
    TVB_BIND("setDomainSuffix", "(JLjava/lang/String;)V");
    TVB_BIND("setOption", "(JLjava/lang/String;Ljava/lang/String;)V");
    TVB_BIND("setAuthUrl", "(JLjava/lang/String;)V");
    TVB_BIND("setUsername", "(JLjava/lang/String;)V");
    TVB_BIND("setPassword", "(JLjava/lang/String;)V");
    TVB_BIND("setMKBroker", "(JLjava/lang/String;)V");
    TVB_BIND("start", "(JLjava/lang/String;)V");
    TVB_BIND("init", "(JLandroid/content/Context;)I");
    TVB_BIND("run", "(J)I");
    TVB_BIND("description", "(J)Ljava/lang/String;");
    TVB_BIND("diagnose", "(J)V");
    TVB_BIND("getVersion", "(J)Ljava/lang/String;");
    TVB_BIND("getPlayPort", "(J)I");
    TVB_BIND("getSockPath", "(J)Ljava/lang/String;");
    TVB_BIND("stop", "(J)V");
    TVB_BIND("stop2", "(JI)V");
    TVB_BIND("quit", "(J)V");
#undef TVB_BIND

    /* err2String 在部分版本被 C++ mangling，单独兜底。 */
    err2 = dlsym(handle, "Java_com_tvbus_engine_TVCore_err2String");
    if (!err2) {
        err2 = dlsym(handle,
                     "_Z39Java_com_tvbus_engine_TVCore_err2StringP7_JNIEnvP8_jobjectxPFPivE");
    }
    if (err2) {
        m[n].name = (char *)"err2String";
        m[n].signature = (char *)"(JI)Ljava/lang/String;";
        m[n].fnPtr = err2;
        n++;
    } else {
        TVB_LOG("missing native err2String");
        missing = 1;
    }

    if (missing) {
        TVB_LOG("some natives missing, skip RegisterNatives");
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    if ((*env)->RegisterNatives(env, cls, m, (jint)n) != 0) {
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        TVB_LOG("RegisterNatives fail");
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    for (i = 0; i < n; i++) {
        (void)m[i];
    }
    return 0;
}

static int finish_handle(JavaVM *vm, void *h)
{
    uintptr_t base;
    ElfW(Dyn) *dyn;

    if (!vm || !h) {
        return 0;
    }
    g_init = (uintptr_t)dlsym(h, k_tvb_init_sym);
    if (!g_init) {
        TVB_LOG("dlsym TVCore_initialise fail");
        return 0;
    }
    base = base_of((void *)g_init);
    dyn = base ? elf_dynamic(base) : 0;
    if (!base || !dyn) {
        TVB_LOG("base/dynamic fail base=%lx", (unsigned long)base);
        return 0;
    }
    /* hook 必须在 JNI_OnLoad 之前：签名校验在引擎初始化早期跑。 */
    if (patch_memcmp(base, dyn) != 0) {
        TVB_LOG("memcmp hook fail (continue)");
    }
    if (under_native_bridge()) {
        stub_signhash(h);
    }
    {
        struct onload_arg oa;
        int crashed = 0;
        memset(&oa, 0, sizeof(oa));
        oa.vm = vm;
        oa.handle = h;
        if (!run_soft(onload_body, &oa, &crashed) || crashed) {
            TVB_LOG("JNI_OnLoad crash");
            return 0;
        }
        if (!oa.have || oa.ver < JNI_VERSION_1_6) {
            TVB_LOG("JNI_OnLoad fail have=%d ver=0x%x", oa.have, (unsigned)oa.ver);
            return 0;
        }
        TVB_LOG("JNI_OnLoad ok ver=0x%x", (unsigned)oa.ver);
    }
    /* 先置位：FindClass(TVCore) 会跑 <clinit>，里面可能查 ms_tvb_ok()。 */
    g_tvb_ok = 1;
    {
        JNIEnv *env = NULL;
        if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
            TVB_LOG("GetEnv fail after OnLoad");
            g_tvb_ok = 0;
            return 0;
        }
        if (register_tvcore(env, h) != 0) {
            g_tvb_ok = 0;
            return 0;
        }
    }
    TVB_LOG("tvbus ready init=+0x%lx bridge=%d", (unsigned long)(g_init - base),
            under_native_bridge());
    return 1;
}

int ms_tvb_load_file(JavaVM *vm, const char *path)
{
    int crashed = 0;
    void *h;

    if (g_tvb_ok) {
        return 1;
    }
    if (!path || !path[0]) {
        return 0;
    }
    ms_guard_ensure();
    if (!ms_guard_ok()) {
        TVB_LOG("load_file skip (guard fail)");
        return 0;
    }
    h = dlopen_soft(path, RTLD_NOW | RTLD_GLOBAL, &crashed);
    if (!h) {
        const char *why = dlerror();
        TVB_LOG("dlopen fail crash=%d %s path=%.120s", crashed, why ? why : "", path);
        return 0;
    }
    TVB_LOG("dlopen ok %.120s", path);
    return finish_handle(vm, h);
}
