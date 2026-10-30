/*
 * Home key -> myTV. Injected by ezinject into the two daemons that turn remote input into
 * /dev/uinput events: lginput2 (Magic Remote) and micomservice (IR). Both send a key with
 * libc write() through their GOT, so swapping that one GOT slot is enough: no inline hook, no
 * Frida. A Home press is cut before it reaches uinput, so LG Home never starts, and myTV is
 * launched instead.
 *
 * The library cannot be unloaded (installed GOT entries point into it), so every way of
 * turning it off is a file the hook checks on each Home press; see home_is_ours().
 */
#ifndef _GNU_SOURCE   /* ezinject's build passes it on the command line */
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ezinject_module.h"

#define KEY_LG_HOME 773   /* LG's own code, above the kernel's KEY_MAX */
#define APP_ID      "com.viiees.mytvlauncher"
#define APP_DIR     "/media/developer/apps/usr/palm/applications/" APP_ID
#define F_DISABLED  "/var/lib/mytv/homehook.disabled"   /* user turned the feature off */
#define F_BYPASS    "/var/lib/mytv/homehook.bypass"     /* watcher: myTV keeps crashing */
#define F_PRESSED   "/var/lib/mytv/homehook.pressed"    /* mtime = last Home taken; watch.sh checks myTV came up */

typedef ssize_t write_fn(int, const void *, size_t);
static write_fn *real_write;

/* Decided on the press and kept for its repeats and release, so LG never sees half a key. */
static volatile bool passing_home;

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_uinput(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)
        && major(st.st_rdev) == 10 && minor(st.st_rdev) == 223;
}

/* myTV opens LG Home on purpose by launching it, not by key, so the hook needs no exception
 * for that: only the off switches below. */
static bool home_is_ours(void) {
    return exists(APP_DIR) && !exists(F_DISABLED) && !exists(F_BYPASS);
}

/* Double fork: the daemon never waits on luna-send and is left no zombie. Only
 * async-signal-safe calls after fork, since the daemon is multithreaded. */
static void launch_mytv(void) {
    /* root-only dir; NOFOLLOW and NONBLOCK still keep a planted link or FIFO from stalling input */
    int mark = open(F_PRESSED, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (mark >= 0) { futimens(mark, NULL); close(mark); }

    pid_t child = fork();
    if (child < 0) return;
    if (child == 0) {
        if (fork() == 0) {
            setsid();
            execl("/usr/bin/luna-send", "luna-send", "-n", "1",
                  "luna://com.webos.applicationManager/launch", "{\"id\":\"" APP_ID "\"}",
                  (char *)NULL);
        }
        _exit(0);
    }
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static ssize_t hook_write(int fd, const void *buf, size_t n) {
    const size_t ev = sizeof(struct input_event);
    if (n < ev || n % ev != 0 || n > 64 * ev || !is_uinput(fd))
        return real_write(fd, buf, n);

    int saved_errno = errno;
    struct input_event in[64], out[64];
    size_t count = n / ev, kept = 0;
    bool had_home = false;
    memcpy(in, buf, n);   /* the caller's buffer may be unaligned */
    for (size_t i = 0; i < count; i++) {
        if (in[i].type == EV_KEY && in[i].code == KEY_LG_HOME) {
            if (in[i].value == 1) {
                passing_home = !home_is_ours();
                if (!passing_home) launch_mytv();
            }
            if (!passing_home) { had_home = true; continue; }
        }
        out[kept++] = in[i];
    }
    errno = saved_errno;
    if (!had_home) return real_write(fd, buf, n);
    if (kept == 0) return (ssize_t)n;
    ssize_t w = real_write(fd, out, kept * ev);
    return w == (ssize_t)(kept * ev) ? (ssize_t)n : w;
}

/* Swap every "write" import of one module for hook_write. ARM uses REL relocations. The GOT
 * can sit in the RELRO segment, read-only after start: open it for the store, then close it.
 * A GOT outside RELRO stays writable for lazy binding and must not be closed. */
struct patch_ctx { uintptr_t self_base; int patched; };

static int patch_module(struct dl_phdr_info *info, size_t size, void *arg) {
    struct patch_ctx *ctx = arg;
    if (info->dlpi_addr == ctx->self_base || strstr(info->dlpi_name, "vdso")) return 0;

    const ElfW(Dyn) *dyn = NULL;
    uintptr_t relro_lo = 0, relro_hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_DYNAMIC) dyn = (const void *)(info->dlpi_addr + ph->p_vaddr);
        if (ph->p_type == PT_GNU_RELRO) {
            relro_lo = info->dlpi_addr + ph->p_vaddr;
            relro_hi = relro_lo + ph->p_memsz;
        }
    }
    if (!dyn) return 0;

    /* glibc rewrites these to absolute addresses at load; guard against an unrelocated one. */
    #define DPTR(d) ((const void *)((d)->d_un.d_ptr < info->dlpi_addr \
                     ? info->dlpi_addr + (d)->d_un.d_ptr : (d)->d_un.d_ptr))
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const ElfW(Rel) *tables[2] = {0};
    size_t sizes[2] = {0};
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_SYMTAB:   symtab = DPTR(dyn); break;
        case DT_STRTAB:   strtab = DPTR(dyn); break;
        case DT_JMPREL:   tables[0] = DPTR(dyn); break;
        case DT_PLTRELSZ: sizes[0] = dyn->d_un.d_val; break;
        case DT_REL:      tables[1] = DPTR(dyn); break;
        case DT_RELSZ:    sizes[1] = dyn->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) return 0;

    long page = sysconf(_SC_PAGESIZE);
    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; tables[t] && i < sizes[t] / sizeof(ElfW(Rel)); i++) {
            const ElfW(Rel) *r = &tables[t][i];
            unsigned type = ELF32_R_TYPE(r->r_info);
            if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT) continue;
            if (strcmp(strtab + symtab[ELF32_R_SYM(r->r_info)].st_name, "write") != 0) continue;

            void **slot = (void **)(info->dlpi_addr + r->r_offset);
            uintptr_t at = (uintptr_t)slot;
            bool in_relro = at >= relro_lo && at < relro_hi;
            void *pg = (void *)(at & ~(uintptr_t)(page - 1));
            if (in_relro && mprotect(pg, page, PROT_READ | PROT_WRITE) != 0) {
                ERR("mprotect %p: %s", pg, strerror(errno));
                continue;
            }
            *slot = (void *)hook_write;   /* one aligned word: atomic for other threads */
            if (in_relro) mprotect(pg, page, PROT_READ);
            ctx->patched++;
            INFO("patched write in %s", info->dlpi_name[0] ? info->dlpi_name : "main");
        }
    }
    return 0;
}

int lib_loginit(log_config_t *cfg) {
    return -1;   /* keep ezinject's log file (-l) */
}

int lib_preinit(struct injcode_user *user) {
    user->persist = true;   /* patched GOT slots point here: never dlclose */
    return 0;
}

int lib_main(int argc, char *argv[]) {
    real_write = (write_fn *)dlsym(RTLD_DEFAULT, "write");
    Dl_info self;
    if (!real_write || !dladdr((void *)hook_write, &self)) {
        ERR("cannot resolve write or own base");
        return -1;
    }
    struct patch_ctx ctx = { (uintptr_t)self.dli_fbase, 0 };
    dl_iterate_phdr(patch_module, &ctx);
    if (ctx.patched == 0) {
        ERR("no write import found");
        return -1;
    }
    INFO("mytvhook active, %d slot(s)", ctx.patched);
    return 0;
}
