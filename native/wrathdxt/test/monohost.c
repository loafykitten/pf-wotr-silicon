// Runs a managed program on the game's own arm64 Mono runtime, outside the game, so the
// game's DXTCompressor.dll can be compared with libwrathdxt under the same JIT (same
// float behaviour) the player uses.
//
// usage: monohost <Wrath.app> <program.exe> [args...]
//
// The game's compressor only needs two Unity internal calls (UnsafeUtility.MemCpy and
// MemSet, behind StbDxtSharp.CRuntime); they're provided here.
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*jit_init_fn)(const char *, const char *);
typedef void (*set_dirs_fn)(const char *, const char *);
typedef void (*set_path_fn)(const char *);
typedef void *(*assembly_open_fn)(void *, const char *);
typedef int (*jit_exec_fn)(void *, void *, int, char **);
typedef void (*add_icall_fn)(const char *, const void *);

static void mem_cpy(void *dst, void *src, long long size) { memmove(dst, src, (size_t)size); }
static void mem_set(void *dst, unsigned char value, long long size) { memset(dst, value, (size_t)size); }

static void *sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (!p) {
        fprintf(stderr, "monohost: missing %s\n", name);
        exit(2);
    }
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <Wrath.app> <program.exe> [args...]\n", argv[0]);
        return 2;
    }
    char lib_path[PATH_MAX], managed[PATH_MAX], etc[PATH_MAX], search[2 * PATH_MAX + 2], exe_dir[PATH_MAX];
    snprintf(lib_path, sizeof lib_path, "%s/Contents/Frameworks/libmonobdwgc-2.0.dylib", argv[1]);
    snprintf(managed, sizeof managed, "%s/Contents/Resources/Data/Managed", argv[1]);
    snprintf(etc, sizeof etc, "%s/Contents/MonoBleedingEdge/etc", argv[1]);
    snprintf(exe_dir, sizeof exe_dir, "%s", argv[2]);
    char *slash = strrchr(exe_dir, '/');
    if (slash) *slash = 0; else strcpy(exe_dir, ".");
    // The program's own folder first, so it can't pick up a same-named game DLL by accident;
    // the game's DLLs it references are resolved from Managed/.
    snprintf(search, sizeof search, "%s:%s", exe_dir, managed);

    void *mono = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!mono) {
        fprintf(stderr, "monohost: %s\n", dlerror());
        return 2;
    }
    ((set_dirs_fn)sym(mono, "mono_set_dirs"))(managed, etc);
    ((set_path_fn)sym(mono, "mono_set_assemblies_path"))(search);
    void *domain = ((jit_init_fn)sym(mono, "mono_jit_init_version"))("monohost", "v4.0.30319");
    if (!domain) {
        fprintf(stderr, "monohost: mono_jit_init_version failed\n");
        return 2;
    }
    add_icall_fn add = (add_icall_fn)sym(mono, "mono_add_internal_call");
    add("Unity.Collections.LowLevel.Unsafe.UnsafeUtility::MemCpy", (const void *)mem_cpy);
    add("Unity.Collections.LowLevel.Unsafe.UnsafeUtility::MemSet", (const void *)mem_set);

    void *assembly = ((assembly_open_fn)sym(mono, "mono_domain_assembly_open"))(domain, argv[2]);
    if (!assembly) {
        fprintf(stderr, "monohost: can't open %s\n", argv[2]);
        return 2;
    }
    // mono_jit_exec takes argv[0] as the program name.
    return ((jit_exec_fn)sym(mono, "mono_jit_exec"))(domain, assembly, argc - 2, argv + 2);
}
