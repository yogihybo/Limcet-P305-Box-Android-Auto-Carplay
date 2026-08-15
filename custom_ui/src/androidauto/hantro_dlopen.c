/*
 * Real, minimal, purpose-built ELF loader providing __wrap_dlopen/
 * __wrap_dlsym/__wrap_dlclose/__wrap_dlerror for androidauto-sidecar
 * ONLY -- loads exactly one file, /usr/lib/libmfc.so (the real Hantro
 * G1 video-decoder shared library this device ships, see
 * hantro_h264_decoder.h/.cpp).
 *
 * Why this exists instead of using tools/nss-stub/nss_stub_busybox.c's
 * dlopen stub (as every OTHER aasdk-based binary in this project's
 * Makefile does -- aasdk-test, wireless-probe-test, bt-rfcomm-test,
 * bw-aap-test): libcrypto.a's own internal dlopen/getaddrinfo
 * references pull in glibc's static-dlopen-NSS init machinery, which
 * crashes at process startup on this toolchain/kernel combination
 * regardless of whether the code path is ever exercised (confirmed on
 * real hardware -- see nss_stub_busybox.c's own comment). Wrapping
 * dlopen to a no-op stub was the fix for that. But this binary
 * genuinely needs a REAL, working dlopen for libmfc.so, and the two
 * requirements directly conflict: `--wrap` intercepts every REFERENCE
 * to the symbol name at link time, so the moment __wrap_dlopen calls
 * through to the real glibc dlopen() (__real_dlopen) for even one
 * case, the linker pulls glibc's real dlopen object file back into
 * the link -- with its crashing static constructor -- regardless of
 * whether that call path is ever actually taken at runtime. So
 * __real_dlopen can never be called, for anything, ever, in this
 * binary.
 *
 * The fix: implement dlopen/dlsym/dlclose OURSELVES, without going
 * through glibc's dynamic-loading machinery at all. This is
 * deliberately NOT a general-purpose dlopen() replacement -- it knows
 * nothing about SONAME-based search paths, lazy binding, or TLS
 * relocations. It supports exactly what libmfc.so's own ELF actually
 * needs (confirmed via `readelf` against the real deployed file,
 * firmware_source/mtd6_rootfs/usr/lib/libmfc.so, 2026-08-15): 2 LOAD
 * segments, 4 relocation types (R_ARM_RELATIVE/R_ARM_ABS32/
 * R_ARM_GLOB_DAT/R_ARM_JUMP_SLOT), and 33 imported libc functions + 1
 * imported data symbol (stdout) -- all of which this binary's own
 * statically-linked glibc already provides real, working
 * implementations of. kImportTable below resolves libmfc.so's imports
 * directly against this process's own already-linked functions,
 * instead of a real dynamic linker's symbol search across shared
 * objects.
 */
#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

/* Not declared by any standard header in a plain C translation unit --
 * only takes its address below (for libmfc.so's own C++-runtime
 * import), never calls it directly, so a minimal extern declaration
 * is enough. */
extern void __cxa_finalize(void *d);

/* ---- getaddrinfo-family stubs -----------------------------------
 * Same content/reasoning as tools/nss-stub/nss_stub_busybox.c's own
 * copy -- duplicated here (not shared) because --wrap requires
 * exactly one definition of each __wrap_* symbol per final binary,
 * and this file's dlopen family below must differ from that shared
 * file's, so androidauto-sidecar can't link both objects together.
 */
int __wrap_getaddrinfo(const char *node, const char *service,
                        const void *hints, void *res) {
    (void)node; (void)service; (void)hints; (void)res;
    return EAI_NONAME;
}
void __wrap_freeaddrinfo(void *res) { (void)res; }
const char *__wrap_gai_strerror(int errcode) {
    (void)errcode;
    return "name resolution disabled in this static build";
}
struct hostent *__wrap_gethostbyname(const char *name) { (void)name; return NULL; }
struct hostent *__wrap_gethostbyname2(const char *name, int af) {
    (void)name; (void)af;
    return NULL;
}
struct hostent *__wrap_gethostbyaddr(const void *addr, socklen_t len, int type) {
    (void)addr; (void)len; (void)type;
    return NULL;
}
struct servent *__wrap_getservbyname(const char *name, const char *proto) {
    (void)name; (void)proto;
    return NULL;
}
struct servent *__wrap_getservbyport(int port, const char *proto) {
    (void)port; (void)proto;
    return NULL;
}

/* 2026-08-15: alsa-lib's dmix/userfile plugins (statically linked here
 * too, see alsa_output.h's own comment) reference these two reentrant
 * pwd/grp lookups -- same broken static-NSS-init trigger family as
 * dlopen/getaddrinfo, confirmed via this exact build's own "requires
 * at runtime" linker warnings. Matches tools/nss-stub/nss_stub.c's own
 * getpwuid_r stub; getgrnam_r is new (nss_stub.c only had the by-gid
 * lookup, getgrgid_r). */
int __wrap_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
                       size_t buflen, struct passwd **result) {
    (void)uid; (void)pwd; (void)buf; (void)buflen;
    *result = NULL;
    return 0;
}
int __wrap_getgrnam_r(const char *name, struct group *grp, char *buf,
                       size_t buflen, struct group **result) {
    (void)name; (void)grp; (void)buf; (void)buflen;
    *result = NULL;
    return 0;
}

/* ---- the real loader ---------------------------------------------- */

/* Exactly the file this loader supports -- see hantro_h264_decoder.cpp's
 * kLibPath. Anything else is refused (matches the old stub's "not
 * supported" behavior for a codepath this project never actually uses). */
static const char *const kSupportedPath = "/usr/lib/libmfc.so";

/* One entry per distinct external symbol libmfc.so's own dynsym table
 * imports from "libc.so.6" (readelf -W --dyn-syms libmfc.so, filtered
 * to UND entries) -- resolved against this ALREADY-statically-linked
 * process's own real functions, not a second copy of libc. */
struct import_entry {
    const char *name;
    void *addr;
};

static void *stdout_addr(void) { return (void *)stdout; }

#define FN(sym) { #sym, (void *)&sym }
static const struct import_entry kImportTable[] = {
    FN(__assert_fail),
    FN(calloc),
    FN(close),
    FN(__cxa_finalize),
    FN(__errno_location),
    FN(fclose),
    FN(fcntl),
    FN(fflush),
    FN(fopen),
    FN(fprintf),
    FN(free),
    FN(fwrite),
    FN(getpagesize),
    FN(ioctl),
    FN(malloc),
    FN(memcpy),
    FN(memmove),
    FN(memset),
    FN(mmap),
    FN(munmap),
    FN(open),
    FN(printf),
    FN(pthread_mutex_lock),
    FN(pthread_mutex_unlock),
    FN(puts),
    FN(raise),
    FN(semctl),
    FN(semget),
    FN(semop),
    FN(sigaction),
    FN(strerror),
    FN(syscall),
    FN(usleep),
};
#undef FN

static void *resolve_import(const char *name) {
    if (strcmp(name, "stdout") == 0) return stdout_addr();
    size_t i;
    for (i = 0; i < sizeof(kImportTable) / sizeof(kImportTable[0]); ++i) {
        if (strcmp(kImportTable[i].name, name) == 0) return kImportTable[i].addr;
    }
    return NULL;
}

/* Handle returned to the caller -- opaque to hantro_h264_decoder.cpp,
 * which only ever passes it back into __wrap_dlsym/__wrap_dlclose. */
struct loaded_lib {
    void *map_base;      /* mmap() base of the whole reserved region */
    size_t map_size;
    unsigned long bias;  /* == (unsigned long)map_base, since vaddrs start at 0 */
    const Elf32_Sym *dynsym;
    unsigned int dynsym_count;
    const char *dynstr;
    void (**fini_array)(void);
    unsigned int fini_array_count;
};

static char g_error_buf[256];
static int g_error_pending = 0;

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error_buf, sizeof(g_error_buf), fmt, ap);
    va_end(ap);
    g_error_pending = 1;
}

char *__wrap_dlerror(void) {
    if (!g_error_pending) return NULL;
    g_error_pending = 0;
    return g_error_buf;
}

/* Reads the whole file into a malloc'd buffer -- libmfc.so is ~430KB,
 * small enough that reading it fully up front is simpler and safer
 * than mmap'ing the file itself and juggling file-backed vs anonymous
 * mappings for the loaded image. This buffer is scratch space only
 * (freed before returning) -- the actual executable image lives in a
 * separate anonymous mapping built below. */
static unsigned char *read_whole_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_error("open(%s) failed: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_error("fstat(%s) failed: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
    if (!buf) {
        set_error("malloc(%ld) failed for %s", (long)st.st_size, path);
        close(fd);
        return NULL;
    }
    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + total, (size_t)st.st_size - total);
        if (n <= 0) {
            set_error("read(%s) failed at offset %zu: %s", path, total, strerror(errno));
            free(buf);
            close(fd);
            return NULL;
        }
        total += (size_t)n;
    }
    close(fd);
    *out_size = total;
    return buf;
}

static const Elf32_Dyn *find_dynamic(const unsigned char *file, const Elf32_Ehdr *eh) {
    const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);
    int i;
    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            return (const Elf32_Dyn *)(file + ph[i].p_offset);
        }
    }
    return NULL;
}

void *__wrap_dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename || strcmp(filename, kSupportedPath) != 0) {
        set_error("dlopen: '%s' not supported by this build's minimal loader "
                  "(only %s is)", filename ? filename : "(null)", kSupportedPath);
        return NULL;
    }

    size_t file_size = 0;
    unsigned char *file = read_whole_file(filename, &file_size);
    if (!file) return NULL;  /* set_error() already called */

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;
    if (file_size < sizeof(Elf32_Ehdr) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM ||
        eh->e_type != ET_DYN) {
        set_error("dlopen: %s is not a 32-bit ARM ET_DYN ELF this loader understands",
                  filename);
        free(file);
        return NULL;
    }

    const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);
    unsigned long span_lo = (unsigned long)-1, span_hi = 0;
    int i;
    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < span_lo) span_lo = ph[i].p_vaddr;
        unsigned long end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > span_hi) span_hi = end;
    }
    if (span_lo == (unsigned long)-1) {
        set_error("dlopen: %s has no PT_LOAD segments", filename);
        free(file);
        return NULL;
    }
    /* Round the reservation up to a page so mmap's own page-rounding
     * of individual segments below never runs past what we reserved. */
    long page = sysconf(_SC_PAGESIZE);
    unsigned long map_size = ((span_hi - span_lo) + (unsigned long)page - 1) &
                              ~((unsigned long)page - 1);

    void *map_base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base == MAP_FAILED) {
        set_error("dlopen: mmap(%lu) failed: %s", map_size, strerror(errno));
        free(file);
        return NULL;
    }
    unsigned long bias = (unsigned long)map_base;

    /* Copy each segment's file content in, zero the rest (.bss etc.) --
     * everything writable for now; permissions get locked down after
     * relocations are applied below. */
    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        unsigned char *dst = (unsigned char *)map_base + ph[i].p_vaddr;
        memcpy(dst, file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    const Elf32_Dyn *dyn = find_dynamic(file, eh);
    if (!dyn) {
        set_error("dlopen: %s has no PT_DYNAMIC segment", filename);
        munmap(map_base, map_size);
        free(file);
        return NULL;
    }

    const Elf32_Sym *dynsym = NULL;
    const char *dynstr = NULL;
    const Elf32_Rel *rel = NULL, *jmprel = NULL;
    unsigned int relsz = 0, pltrelsz = 0;
    void (**init_array)(void) = NULL;
    unsigned int init_array_count = 0;
    void (**fini_array)(void) = NULL;
    unsigned int fini_array_count = 0;
    unsigned int syment = sizeof(Elf32_Sym);

    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
            case DT_SYMTAB: dynsym = (const Elf32_Sym *)((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_STRTAB: dynstr = (const char *)((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_SYMENT: syment = (unsigned int)dyn->d_un.d_val; break;
            case DT_REL:    rel = (const Elf32_Rel *)((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_RELSZ:  relsz = (unsigned int)dyn->d_un.d_val; break;
            case DT_JMPREL: jmprel = (const Elf32_Rel *)((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = (unsigned int)dyn->d_un.d_val; break;
            case DT_INIT_ARRAY: init_array = (void (**)(void))((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_INIT_ARRAYSZ: init_array_count = (unsigned int)dyn->d_un.d_val / sizeof(void *); break;
            case DT_FINI_ARRAY: fini_array = (void (**)(void))((unsigned char *)map_base + dyn->d_un.d_ptr); break;
            case DT_FINI_ARRAYSZ: fini_array_count = (unsigned int)dyn->d_un.d_val / sizeof(void *); break;
            default: break;
        }
    }
    if (!dynsym || !dynstr) {
        set_error("dlopen: %s missing SYMTAB/STRTAB", filename);
        munmap(map_base, map_size);
        free(file);
        return NULL;
    }
    /* dynsym has no explicit count in .dynamic -- derive it from the
     * section header table instead (only needed once, at load time,
     * for __wrap_dlsym's linear scan later). */
    unsigned int dynsym_count = 0;
    if (eh->e_shoff && eh->e_shnum) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(file + eh->e_shoff);
        int j;
        for (j = 0; j < eh->e_shnum; ++j) {
            if (sh[j].sh_type == SHT_DYNSYM) {
                dynsym_count = sh[j].sh_size / syment;
                break;
            }
        }
    }

    /* Apply .rel.dyn then .rel.plt -- both plain Elf32_Rel (implicit,
     * in-place addends), just at different offsets/sizes; ARM uses the
     * same relocation types and processing for both. */
    const struct { const Elf32_Rel *tab; unsigned int size; } rel_tables[2] = {
        { rel, relsz }, { jmprel, pltrelsz },
    };
    int table_idx;
    int failed = 0;
    for (table_idx = 0; table_idx < 2 && !failed; ++table_idx) {
        const Elf32_Rel *tab = rel_tables[table_idx].tab;
        unsigned int count = tab ? rel_tables[table_idx].size / sizeof(Elf32_Rel) : 0;
        unsigned int k;
        for (k = 0; k < count; ++k) {
            const Elf32_Rel *r = &tab[k];
            unsigned int type = ELF32_R_TYPE(r->r_info);
            unsigned int symidx = ELF32_R_SYM(r->r_info);
            unsigned char *target = (unsigned char *)map_base + r->r_offset;
            unsigned long *target32 = (unsigned long *)target;

            unsigned long sym_value = 0;
            if (symidx != 0) {
                const Elf32_Sym *sym = &dynsym[symidx];
                const char *name = dynstr + sym->st_name;
                if (sym->st_shndx != SHN_UNDEF) {
                    sym_value = bias + sym->st_value;
                } else {
                    void *resolved = resolve_import(name);
                    if (resolved) {
                        sym_value = (unsigned long)resolved;
                    } else if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                        /* Real hardware caught this: __gmon_start__,
                         * _ITM_(de)registerTMCloneTable, and
                         * _Jv_RegisterClasses are all WEAK undefined
                         * symbols GCC's crtbegin.o unconditionally
                         * references (gprof hook, transactional-memory
                         * clone tables, old GCJ Java class registration
                         * -- none of which this build actually uses).
                         * A real dynamic linker leaves an unresolved
                         * WEAK symbol as NULL rather than failing; the
                         * generated code already null-checks before
                         * calling through these. Only a non-weak
                         * (STB_GLOBAL) unresolved import is a real
                         * problem. */
                        sym_value = 0;
                    } else {
                        set_error("dlopen: %s: unresolved import '%s' (relocation type %u) -- "
                                  "this build of libmfc.so needs a symbol this loader's import "
                                  "table doesn't know about", filename, name, type);
                        failed = 1;
                        break;
                    }
                }
            }

            switch (type) {
                case R_ARM_RELATIVE:
                    *target32 = *target32 + bias;
                    break;
                case R_ARM_ABS32:
                    *target32 = *target32 + sym_value;
                    break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
                    *target32 = sym_value;
                    break;
                default:
                    set_error("dlopen: %s: unsupported relocation type %u at offset 0x%x",
                              filename, type, (unsigned int)r->r_offset);
                    failed = 1;
                    break;
            }
        }
    }
    if (failed) {
        munmap(map_base, map_size);
        free(file);
        return NULL;
    }

    /* Lock down final permissions per segment, flush the instruction
     * cache over anything executable (ARM has separate I/D caches --
     * code bytes were just written via memcpy above as plain data
     * writes, and the CPU's I$ can still hold stale/no entries for
     * this range, so this is required for correctness, not just
     * performance). */
    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        int prot = PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        unsigned char *seg = (unsigned char *)map_base + ph[i].p_vaddr;
        /* mprotect requires page-aligned addresses -- p_vaddr for a
         * PT_LOAD segment is always page-aligned relative to p_offset
         * by construction (same alignment field for both), and our
         * mapping's base is itself page-aligned (mmap guarantees
         * this), so seg is already page-aligned here. */
        mprotect(seg, ph[i].p_memsz, prot);
        if (ph[i].p_flags & PF_X) {
            __builtin___clear_cache((char *)seg, (char *)seg + ph[i].p_memsz);
        }
    }

    free(file);  /* scratch copy no longer needed -- the loaded image is independent */

    for (i = 0; i < (int)init_array_count; ++i) {
        if (init_array[i]) init_array[i]();
    }

    struct loaded_lib *lib = (struct loaded_lib *)malloc(sizeof(struct loaded_lib));
    if (!lib) {
        set_error("dlopen: malloc failed for handle");
        munmap(map_base, map_size);
        return NULL;
    }
    lib->map_base = map_base;
    lib->map_size = map_size;
    lib->bias = bias;
    lib->dynsym = dynsym;
    lib->dynsym_count = dynsym_count;
    lib->dynstr = dynstr;
    lib->fini_array = fini_array;
    lib->fini_array_count = fini_array_count;
    return lib;
}

void *__wrap_dlsym(void *handle, const char *symbol) {
    if (!handle || !symbol) {
        set_error("dlsym: null handle or symbol name");
        return NULL;
    }
    struct loaded_lib *lib = (struct loaded_lib *)handle;
    unsigned int i;
    for (i = 0; i < lib->dynsym_count; ++i) {
        const Elf32_Sym *sym = &lib->dynsym[i];
        if (sym->st_shndx == SHN_UNDEF) continue;
        const char *name = lib->dynstr + sym->st_name;
        if (strcmp(name, symbol) == 0) {
            return (void *)(lib->bias + sym->st_value);
        }
    }
    set_error("dlsym: '%s' not found", symbol);
    return NULL;
}

int __wrap_dlclose(void *handle) {
    if (!handle) return 0;
    struct loaded_lib *lib = (struct loaded_lib *)handle;
    int i;
    for (i = (int)lib->fini_array_count - 1; i >= 0; --i) {
        if (lib->fini_array[i]) lib->fini_array[i]();
    }
    munmap(lib->map_base, lib->map_size);
    free(lib);
    return 0;
}
