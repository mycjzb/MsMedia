# Release hardening for libaliiptv/libcntv: strip, hidden visibility, no exported
# static-archive symbols, stack protector, anti-debug (MS_GUARD_ENABLED),
# full RELRO, Bsymbolic, builtin mem/str (raise PLT/GOT hook cost).

option(MS_CLIENT_HARDEN "Apply release hardening to Android native libs" ON)
set(_MS_ALIGN_ELF_16K "${CMAKE_CURRENT_LIST_DIR}/../scripts/align_elf_16k.py")
set(_MS_BUILTIN_MEM_H "${CMAKE_CURRENT_LIST_DIR}/ms_builtin_mem.h")

function(ms_is_hardened out_var)
    set(${out_var} OFF PARENT_SCOPE)
    if(NOT MS_CLIENT_HARDEN)
        return()
    endif()
    # AGP release 实际是 RelWithDebInfo，不是 Release；两者都开硬化
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(${out_var} ON PARENT_SCOPE)
    endif()
endfunction()

function(ms_apply_compile_hardening target)
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(_type ${target} TYPE)
    get_target_property(_alias ${target} ALIASED_TARGET)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR _alias)
        return()
    endif()
    ms_is_hardened(_h)
    target_compile_options(${target} PRIVATE
        -ffunction-sections
        -fdata-sections
        -fvisibility=hidden
        -fstack-protector-strong)
    if(_h)
        target_compile_options(${target} PRIVATE
            -Os
            -gline-tables-only
            -DNDEBUG
            -fno-asynchronous-unwind-tables)
        target_compile_definitions(${target} PRIVATE MS_GUARD_ENABLED=1)
        # C only: macros break libc++ (std::memcpy → std::__builtin_memcpy).
        if(EXISTS "${_MS_BUILTIN_MEM_H}")
            target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:C>:-include;${_MS_BUILTIN_MEM_H}>")
        endif()
    endif()
endfunction()

function(ms_apply_link_hardening target)
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(_type ${target} TYPE)
    get_target_property(_alias ${target} ALIASED_TARGET)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR _alias)
        return()
    endif()
    ms_is_hardened(_h)
    target_link_options(${target} PRIVATE
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL)
    # 硬性：16KB 页对齐只给 arm64-v8a。armeabi-v7a 保持链接器默认 4KB，
    # 禁止「顺便」给 v7a 加 max-page-size / align_elf_16k（审计勿改偏）。
    if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
        # NDK r21 lld：max-page-size → p_align=16384；POST_BUILD align_elf_16k.py
        # 再垫文件页，避免 RX/RW 共占 16KB（vaddr 不动）。
        target_link_options(${target} PRIVATE
            -Wl,-z,max-page-size=16384
            -Wl,-z,common-page-size=16384)
        ms_align_so_16k(${target})
    endif()
    if(_h)
        # No --strip-all: obj/<abi>/*.so keeps symbols + line tables for
        # scripts/symbolize.sh; AGP strip*DebugSymbols strips the APK copy.
        # relro+now = full RELRO (GOT 起完只读)；Bsymbolic = 库内引用不优先走外置符号。
        target_link_options(${target} PRIVATE
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
            -Wl,-Bsymbolic)
    endif()
endfunction()

function(ms_align_so_16k target)
    if(NOT EXISTS "${_MS_ALIGN_ELF_16K}")
        message(WARNING "align_elf_16k.py missing; 16KB file-page pad skipped")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND python3 "${_MS_ALIGN_ELF_16K}" "$<TARGET_FILE:${target}>"
        COMMENT "16KB-align LOAD file pages of $<TARGET_FILE_NAME:${target}>"
        VERBATIM)
endfunction()

function(ms_harden_static_deps)
    if(NOT ANDROID)
        return()
    endif()
    foreach(_t IN ITEMS cjson ms_client)
        if(TARGET ${_t})
            ms_apply_compile_hardening(${_t})
        endif()
    endforeach()
endfunction()

# OLLVM / Hikari / Arkari: only when compiler understands -mllvm -fla.
# Whitelist sensitive TUs; never flatten p2p/kcp/ffmpeg hot paths.
# No OLLVM clang → silent skip (plain NDK still builds).
set(_MS_OLLVM_WHITELIST_RE
    "(guard\\.c|obfdec\\.c|apk_sign\\.c|blob_crypto\\.c|session\\.c|icon_auth\\.c|tracker_crypto\\.c|ysp_tea\\.cpp|ms_jni\\.c)$")

function(ms_ollvm_available out_var)
    set(${out_var} OFF PARENT_SCOPE)
    if(NOT CMAKE_C_COMPILER)
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -mllvm -fla -x c -c /dev/null -o /dev/null
        RESULT_VARIABLE _rc
        OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        set(${out_var} ON PARENT_SCOPE)
    endif()
endfunction()

function(ms_apply_obfuscation target)
    if(NOT TARGET ${target})
        return()
    endif()
    ms_is_hardened(_h)
    if(NOT _h)
        return()
    endif()
    ms_ollvm_available(_ollvm)
    if(NOT _ollvm)
        return()
    endif()
    message(STATUS "OLLVM: flattening whitelist for ${target}")
    get_target_property(_srcs ${target} SOURCES)
    if(NOT _srcs)
        return()
    endif()
    foreach(_s IN LISTS _srcs)
        get_filename_component(_bn "${_s}" NAME)
        if(_bn MATCHES "${_MS_OLLVM_WHITELIST_RE}")
            set_source_files_properties("${_s}" PROPERTIES
                COMPILE_OPTIONS "-mllvm;-fla;-mllvm;-sub;-mllvm;-bcf;-mllvm;-bcf_prob=20")
        endif()
    endforeach()
endfunction()
