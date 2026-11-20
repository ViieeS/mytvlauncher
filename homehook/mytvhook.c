/*
 * Home key -> myTV. Injected by ezinject into the two daemons that turn remote input into
 * /dev/uinput events: lginput2 (Magic Remote) and micomservice (IR). Both send a key with
 * libc write() through their GOT, so swapping that one GOT slot is enough: no inline hook, no
 * Frida. A Home press is cut before it reaches uinput, so LG Home never starts, and myTV is
 * launched instead. Remote buttons the user bound to apps in myTV are taken the same way.
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
#include <pthread.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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
/* Remote buttons bound to apps ("<code> <app id>" per line, written by watch.sh bind/unbind), and
 * the learn handshake: watch.sh learn creates F_LEARN, the next bindable press is written to
 * F_LEARNED instead of reaching the TV. */
#define F_KEYBINDS  "/var/lib/mytv/keybinds"
#define F_LEARN     "/var/lib/mytv/keys.learn"
#define F_LEARNED   "/var/lib/mytv/keys.learned"
#define LEARN_SECS  20
/* LG's own buttons (apps, assistants) sit above the kernel's key range; navigation, OK, Back
 * (412), numbers and volume are below it, so they can never be bound away. */
#define KEY_BINDABLE_MIN 1024

typedef ssize_t write_fn(int, const void *, size_t);
static write_fn *real_write;

/* Decided on the press and kept for its repeats and release, so LG never sees half a key. */
static volatile bool passing_home;
static volatile int eaten_code = -1;   /* a bindable key taken on its press */

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

static void launch_app(const char *id) {
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\"}", id);   /* before fork: not async-signal-safe */
    pid_t child = fork();
    if (child < 0) return;
    if (child == 0) {
        if (fork() == 0) {
            setsid();
            execl("/usr/bin/luna-send", "luna-send", "-n", "1",
                  "luna://com.webos.applicationManager/launch", payload, (char *)NULL);
        }
        _exit(0);
    }
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static bool valid_app_id(const char *id) {
    if (!*id) return false;
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
              || *c == '.' || *c == '_' || *c == '-')) return false;
    return true;
}

/* The table is re-read only when the file changes. Ids are checked again here: they end up in a
 * JSON payload, and the file is the hook's only input. */
static struct { int code; char id[96]; } binds[16];
static int nbinds;
static struct stat binds_st;
static pthread_mutex_t binds_mtx = PTHREAD_MUTEX_INITIALIZER;

static void binds_reload(void) {
    struct stat st;
    if (stat(F_KEYBINDS, &st) != 0) { nbinds = 0; memset(&binds_st, 0, sizeof(binds_st)); return; }
    if (st.st_mtim.tv_sec == binds_st.st_mtim.tv_sec && st.st_mtim.tv_nsec == binds_st.st_mtim.tv_nsec
        && st.st_size == binds_st.st_size && st.st_ino == binds_st.st_ino) return;
    binds_st = st;
    nbinds = 0;
    int fd = open(F_KEYBINDS, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    char buf[2048];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return;
    buf[len] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && nbinds < 16; line = strtok_r(NULL, "\n", &save)) {
        char id[96];
        int code;
        if (sscanf(line, "%d %95s", &code, id) == 2 && code >= KEY_BINDABLE_MIN && valid_app_id(id)) {
            binds[nbinds].code = code;
            memcpy(binds[nbinds].id, id, sizeof(id));
            nbinds++;
        }
    }
}

static bool learning(void) {
    struct stat st;
    return stat(F_LEARN, &st) == 0 && time(NULL) - st.st_mtime < LEARN_SECS;
}

/* A bindable press: learned, or launches its app. True when the key is taken from the TV. */
static bool take_bindable(int code) {
    if (!exists(APP_DIR) || exists(F_DISABLED)) return false;
    if (learning()) {
        int f = open(F_LEARNED, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (f >= 0) { dprintf(f, "%d\n", code); close(f); }
        unlink(F_LEARN);
        return true;
    }
    char id[96] = "";
    pthread_mutex_lock(&binds_mtx);   /* the daemon may write keys from more than one thread */
    binds_reload();
    for (int i = 0; i < nbinds; i++)
        if (binds[i].code == code) { memcpy(id, binds[i].id, sizeof(id)); break; }
    pthread_mutex_unlock(&binds_mtx);
    if (!id[0]) return false;
    launch_app(id);
    return true;
}

static ssize_t hook_write(int fd, const void *buf, size_t n) {
    const size_t ev = sizeof(struct input_event);
    if (n < ev || n % ev != 0 || n > 64 * ev || !is_uinput(fd))
        return real_write(fd, buf, n);

    int saved_errno = errno;
    struct input_event in[64], out[64];
    size_t count = n / ev, kept = 0;
    bool dropped = false;
    memcpy(in, buf, n);   /* the caller's buffer may be unaligned */
    for (size_t i = 0; i < count; i++) {
        if (in[i].type == EV_KEY && in[i].code == KEY_LG_HOME) {
            if (in[i].value == 1) {
                passing_home = !home_is_ours();
                if (!passing_home) launch_mytv();
            }
            if (!passing_home) { dropped = true; continue; }
        } else if (in[i].type == EV_KEY && in[i].code >= KEY_BINDABLE_MIN) {
            if (in[i].value == 1) eaten_code = take_bindable(in[i].code) ? in[i].code : -1;
            if (in[i].code == eaten_code) {
                if (in[i].value == 0) eaten_code = -1;
                dropped = true;
                continue;
            }
        }
        out[kept++] = in[i];
    }
    errno = saved_errno;
    if (!dropped) return real_write(fd, buf, n);
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
