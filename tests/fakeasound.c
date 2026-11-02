/* fake libasound: one capture PCM on card 0, S32 interleaved 2 ch, ramp data */
#include <alsa/asoundlib.h>
#include <string.h>
static int fmt_mode; /* 0 S32 interleaved, 1 S16 non-interleaved */
struct fake { int x; } F;
int snd_pcm_open(snd_pcm_t **p, const char *n, snd_pcm_stream_t s, int m) { (void)n;(void)s;(void)m; *p=(snd_pcm_t*)&F; return 0; }
int snd_pcm_close(snd_pcm_t *p) { (void)p; return 0; }
size_t snd_pcm_info_sizeof(void) { return 64; }
int snd_pcm_info(snd_pcm_t *p, snd_pcm_info_t *i) { (void)p;(void)i; return 0; }
int snd_pcm_info_get_card(const snd_pcm_info_t *i) { (void)i; return 0; }
int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h) { (void)p; fmt_mode = *(int*)h; return 0; }
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *h, snd_pcm_format_t *f) { *f = *(const int*)h ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S32_LE; return 0; }
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *h, unsigned *c) { (void)h; *c = 2; return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *h, unsigned *r, int *d) { (void)h; *r=44100; *d=0; return 0; }
int snd_pcm_hw_params_get_access(const snd_pcm_hw_params_t *h, snd_pcm_access_t *a) { *a = *(const int*)h ? SND_PCM_ACCESS_RW_NONINTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED; return 0; }
static int k;
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *p, void *b, snd_pcm_uframes_t n) { (void)p; int32_t *o=b; for (unsigned i=0;i<n;i++){o[2*i]=(k+i)<<16; o[2*i+1]=-(int32_t)((k+i)<<16);} k+=n; return n; }
snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *p, void **b, snd_pcm_uframes_t n) { (void)p; int16_t *l=b[0],*r=b[1]; for (unsigned i=0;i<n;i++){l[i]=k+i; r[i]=-(k+i);} k+=n; return n; }
