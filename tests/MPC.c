/* fake MPC: reads 4 blocks of 128 frames, then a plugin-style reader checks the tap */
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <stdio.h>
#include "nova_tap.h"
int main(int argc, char **argv) {
    int mode = argc > 1; snd_pcm_t *p; snd_pcm_open(&p, "hw:CARD=Audio,DEV=0", SND_PCM_STREAM_CAPTURE, 0);
    int hw = mode; snd_pcm_hw_params(p, (snd_pcm_hw_params_t*)&hw);
    static int32_t ibuf[256]; static int16_t l[128], r[128]; void *nb[2] = {l, r};
    for (int b = 0; b < 4; b++) { if (mode) snd_pcm_readn(p, nb, 128); else snd_pcm_readi(p, ibuf, 128); }
    nova_audio_tap_fn fn = (nova_audio_tap_fn)dlsym(RTLD_DEFAULT, "nova_audio_tap");
    if (!fn) { puts("FAIL: nova_audio_tap not visible to a plugin"); return 1; }
    nova_tap_t *t = fn();
    printf("%s: ok=%d ch=%u rate=%u wpos=%llu  frame0 L=%d R=%d  frame300 L=%d R=%d  (MPC's own buffer untouched: %s)\n",
        mode ? "S16 non-interleaved" : "S32 interleaved", t->ok, t->ch, t->rate, (unsigned long long)t->wpos,
        t->ring[0][0] >> 16, t->ring[0][1] >> 16, t->ring[300][0] >> 16, t->ring[300][1] >> 16,
        mode ? (l[0] == 384 ? "yes" : "NO") : (ibuf[0] == (384 << 16) ? "yes" : "NO"));
    snd_pcm_close(p);
    printf("after close: ok=%d\n", t->ok);
    return 0; }
