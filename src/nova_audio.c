/* nova-audio: an MPC addin (LD_PRELOAD) that lets the Nova Editor plugin play the MiniNova's audio on its own track.
 *
 * MPC opens its audio inputs exclusively, so a plugin can't read them. This library, loaded into MPC's process, hooks
 * the calls MPC itself uses to read its inputs (snd_pcm_readi / snd_pcm_readn on the capture stream of its audio card)
 * and copies every frame MPC reads into a ring buffer. MPC's own audio is untouched: the buffer MPC gets back is
 * exactly what the hardware gave it. Nova Editor, running in the same process, finds the ring with
 * dlsym(RTLD_DEFAULT, "nova_audio_tap") and plays it out as its own sound.
 *
 * Patterns from mpc-addin-usb-audio (MIT, Jacob Sabella): stream recognition in snd_pcm_open/hw_params/close, RTLD_NEXT
 * resolution. Audio-thread rule (readi/readn): no locks, no allocation, no syscalls beyond the real call, no logging.
 * Outside /usr/bin/MPC every hook is a plain pass-through. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nova_tap.h"

#define EXPORT __attribute__((visibility("default")))

/* ---- the tap Nova Editor reads ---- */
static nova_tap_t g_tap = {.magic = NOVA_TAP_MAGIC, .version = NOVA_TAP_VERSION};
EXPORT nova_tap_t *nova_audio_tap(void) { return &g_tap; }

/* ---- real functions ---- */
static int (*real_open)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
static int (*real_close)(snd_pcm_t *);
static int (*real_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static snd_pcm_sframes_t (*real_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
static snd_pcm_sframes_t (*real_readn)(snd_pcm_t *, void **, snd_pcm_uframes_t);
static size_t (*f_info_sizeof)(void);
static int (*f_info)(snd_pcm_t *, snd_pcm_info_t *);
static int (*f_info_get_card)(const snd_pcm_info_t *);
static int (*f_get_format)(const snd_pcm_hw_params_t *, snd_pcm_format_t *);
static int (*f_get_channels)(const snd_pcm_hw_params_t *, unsigned *);
static int (*f_get_rate)(const snd_pcm_hw_params_t *, unsigned *, int *);
static int (*f_get_access)(const snd_pcm_hw_params_t *, snd_pcm_access_t *);

static int g_active;                     /* 1 inside MPC */
static _Atomic(snd_pcm_t *) g_cap;       /* MPC's capture stream on its audio card */
static int g_fmt = -1, g_inter;          /* sample format (ours), interleaved? */
static unsigned g_ch;
static char g_dir[256];

enum { F_S16, F_S24_4, F_S24_3, F_S32, F_FLOAT };

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char p[300];
    snprintf(p, sizeof p, "%s/nova-audio.log", g_dir[0] ? g_dir : "/tmp");
    FILE *f = fopen(p, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void resolve(void) {
    if (real_open) return;
    *(void **)&real_open = dlsym(RTLD_NEXT, "snd_pcm_open");
    *(void **)&real_close = dlsym(RTLD_NEXT, "snd_pcm_close");
    *(void **)&real_hw_params = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    *(void **)&real_readi = dlsym(RTLD_NEXT, "snd_pcm_readi");
    *(void **)&real_readn = dlsym(RTLD_NEXT, "snd_pcm_readn");
    *(void **)&f_info_sizeof = dlsym(RTLD_NEXT, "snd_pcm_info_sizeof");
    *(void **)&f_info = dlsym(RTLD_NEXT, "snd_pcm_info");
    *(void **)&f_info_get_card = dlsym(RTLD_NEXT, "snd_pcm_info_get_card");
    *(void **)&f_get_format = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
    *(void **)&f_get_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
    *(void **)&f_get_rate = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
    *(void **)&f_get_access = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_access");
}

static int exe_is_mpc(void) {
    char p[256];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return 0;
    p[n] = 0;
    const char *b = strrchr(p, '/');
    return !strcmp(b ? b + 1 : p, "MPC");
}

static void self_dir(void) {
    Dl_info di;
    g_dir[0] = 0;
    if (dladdr((void *)&self_dir, &di) && di.dli_fname) {
        snprintf(g_dir, sizeof g_dir, "%s", di.dli_fname);
        char *s = strrchr(g_dir, '/');
        if (s) *s = 0; else g_dir[0] = 0;
    }
}

__attribute__((constructor)) static void init(void) {
    resolve();
    g_active = exe_is_mpc();
    if (!g_active) return;
    self_dir();
    logf_("nova-audio %s loaded in MPC", NOVA_AUDIO_VERSION);
}

/* glibc 2.38 and later redirect strtol (and so atoi) to __isoc23_strtol, which would raise this library's glibc
 * requirement above what MPC OS 2.x and the plugin catalog accept. A whole decimal is all that is parsed here. */
static int nv_atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* MPC's own audio card: id "Audio" (the Live II's internal interface), else the platform codec */
static int is_mpc_card(int card) {
#ifdef NOVA_AUDIO_TEST
    if (getenv("NOVA_AUDIO_TEST_CARD")) return card == nv_atoi(getenv("NOVA_AUDIO_TEST_CARD"));
#endif
    char p[64], id[64] = "";
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    if (!fgets(id, sizeof id, f)) id[0] = 0;
    fclose(f);
    id[strcspn(id, "\r\n ")] = 0;
    if (!strcmp(id, "Audio")) return 1;
    char t[64];
    ssize_t n = readlink("/dev/snd/by-path/platform-sound", t, sizeof t - 1);
    if (n > 0) { t[n] = 0; const char *s = strstr(t, "controlC"); if (s && nv_atoi(s + 8) == card) return 1; }
    return 0;
}

EXPORT int snd_pcm_open(snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream, int mode) {
    resolve();
    if (!real_open) return -ENOSYS;
    int r = real_open(pcmp, name, stream, mode);
    if (r < 0 || !g_active || stream != SND_PCM_STREAM_CAPTURE || !f_info_sizeof || !f_info || !f_info_get_card) return r;
    snd_pcm_info_t *info = alloca(f_info_sizeof());
    memset(info, 0, f_info_sizeof());
    if (f_info(*pcmp, info) < 0) return r;
    int card = f_info_get_card(info);
    if (!is_mpc_card(card)) return r;
    atomic_store(&g_tap.ok, 0);
    atomic_store(&g_cap, *pcmp);
    logf_("capture stream '%s' on card %d", name ? name : "?", card);
    return r;
}

EXPORT int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
    resolve();
    if (!real_hw_params) return -ENOSYS;
    int mine = g_active && pcm && pcm == atomic_load(&g_cap);
    if (mine) atomic_store(&g_tap.ok, 0);
    int r = real_hw_params(pcm, params);
    if (!mine || r < 0 || !f_get_format || !f_get_channels || !f_get_rate || !f_get_access) return r;
    snd_pcm_format_t fmt;
    snd_pcm_access_t acc;
    unsigned ch = 0, rate = 0;
    int dir = 0;
    f_get_format(params, &fmt); f_get_channels(params, &ch); f_get_rate(params, &rate, &dir); f_get_access(params, &acc);
    g_fmt = fmt == SND_PCM_FORMAT_S16_LE ? F_S16 : fmt == SND_PCM_FORMAT_S24_LE ? F_S24_4 :
            fmt == SND_PCM_FORMAT_S24_3LE ? F_S24_3 : fmt == SND_PCM_FORMAT_S32_LE ? F_S32 :
            fmt == SND_PCM_FORMAT_FLOAT_LE ? F_FLOAT : -1;
    g_inter = acc == SND_PCM_ACCESS_RW_INTERLEAVED;
    int ok = g_fmt >= 0 && ch >= 1 && ch <= NOVA_TAP_MAX_CH &&
             (acc == SND_PCM_ACCESS_RW_INTERLEAVED || acc == SND_PCM_ACCESS_RW_NONINTERLEAVED);
    g_ch = ch;
    g_tap.ch = ch;
    g_tap.rate = rate;
    logf_("capture: %u ch, format %d, %u Hz, %s%s", ch, (int)fmt, rate, g_inter ? "interleaved" : "non-interleaved",
          ok ? "" : ": not supported, not tapping");
    if (ok) atomic_store_explicit(&g_tap.ok, 1, memory_order_release);
    return r;
}

EXPORT int snd_pcm_close(snd_pcm_t *pcm) {
    resolve();
    if (!real_close) return -ENOSYS;
    if (g_active && pcm && pcm == atomic_load(&g_cap)) {
        atomic_store(&g_tap.ok, 0);
        atomic_store(&g_cap, NULL);
        logf_("capture stream closed");
    }
    return real_close(pcm);
}

/* ---- audio thread ---- */
static inline int32_t sample(const void *base, size_t i) {
    switch (g_fmt) {
    case F_S16: return (int32_t)((const int16_t *)base)[i] * 65536;
    case F_S24_4: return (((const int32_t *)base)[i] << 8);
    case F_S24_3: {
        const uint8_t *p = (const uint8_t *)base + i * 3;
        return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
    }
    case F_S32: return ((const int32_t *)base)[i];
    default: {
        float f = ((const float *)base)[i];
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int32_t)(f * 2147483647.0f);
    }
    }
}

static void tap(const void *buf, void **bufs, uint32_t frames) {
    unsigned ch = g_ch;
    uint64_t w = atomic_load_explicit(&g_tap.wpos, memory_order_relaxed);
    for (uint32_t f = 0; f < frames; f++) {
        int32_t *dst = g_tap.ring[(w + f) & (NOVA_TAP_FRAMES - 1)];
        for (unsigned c = 0; c < ch; c++)
            dst[c] = buf ? sample(buf, (size_t)f * ch + c) : sample(bufs[c], f);
    }
    atomic_store_explicit(&g_tap.wpos, w + frames, memory_order_release);
}

EXPORT snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n) {
    if (!real_readi) { resolve(); if (!real_readi) return -ENOSYS; }
    snd_pcm_sframes_t r = real_readi(pcm, buf, n);
    if (r > 0 && buf && pcm == atomic_load_explicit(&g_cap, memory_order_relaxed) && g_inter &&
        atomic_load_explicit(&g_tap.ok, memory_order_acquire))
        tap(buf, NULL, (uint32_t)r);
    return r;
}

EXPORT snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t n) {
    if (!real_readn) { resolve(); if (!real_readn) return -ENOSYS; }
    snd_pcm_sframes_t r = real_readn(pcm, bufs, n);
    if (r > 0 && bufs && pcm == atomic_load_explicit(&g_cap, memory_order_relaxed) && !g_inter &&
        atomic_load_explicit(&g_tap.ok, memory_order_acquire))
        tap(NULL, bufs, (uint32_t)r);
    return r;
}
