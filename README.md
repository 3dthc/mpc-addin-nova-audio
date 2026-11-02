# Nova Audio

An [addin](https://open-mpc.github.io/) for Akai MPC OS standalone devices that lets a **plugin play the MPC's audio
inputs as its own sound**, so a hardware synth becomes one ordinary MPC track: played, monitored, recorded, mixed and
resampled on the plugin's own track, with no separate audio track and no Monitor switching.

It was written for [Nova Editor](https://github.com/3dthc/nova-editor), an editor and librarian for the Novation
MiniNova, but nothing in it is MiniNova-specific: any plugin can read the tap.

## Why it is needed

MPC opens its audio inputs exclusively, so a plugin cannot open them itself (`snd_pcm_open` returns `EBUSY`). This
library is loaded into MPC's own process through `LD_PRELOAD` and hooks the calls MPC uses to read its inputs
(`snd_pcm_readi` / `snd_pcm_readn` on the capture stream of MPC's audio card). Every frame MPC reads is copied into a
ring buffer; **what MPC gets back is exactly what the hardware gave it**, untouched.

A plugin in the same process finds the ring with `dlsym(RTLD_DEFAULT, "nova_audio_tap")` and plays it out.

- Added latency: about one audio block (~3 ms at 44.1 kHz, 128-frame periods).
- The audio thread takes no locks, allocates nothing and makes no syscalls beyond the real call.
- Outside `/usr/bin/MPC` every hook is a plain pass-through.
- Formats handled: S16_LE, S24_3LE, S24_LE, S32_LE and FLOAT_LE, interleaved or not, up to 8 channels.

## Using it from a plugin

```c
#include "nova_tap.h"                     /* src/nova_tap.h, two small structs, no dependencies */

nova_audio_tap_fn fn = (nova_audio_tap_fn)dlsym(RTLD_DEFAULT, "nova_audio_tap");
nova_tap_t *tap = fn ? fn() : NULL;
if (tap && tap->magic == NOVA_TAP_MAGIC && tap->version == NOVA_TAP_VERSION) {
    /* frames [rpos, tap->wpos) are available; samples are int32 full scale, tap->ch channels */
}
```

Keep your own read position, start it a block or two behind `wpos`, and re-sync if it falls outside the ring. MPC's
input read and your `render()` run on the same clock, so there is no drift and no resampling.

## Install

See `INSTALL.md` in a release zip. In short: copy the unpacked folder to the device and run its `install.sh` as root
over SSH. It installs to `/data/mpc-addins/nova-audio/` and adds the library to MPC's `LD_PRELOAD`, keeping
everything already in it.

**Hakai and other launchers:** where a launcher script sets `LD_PRELOAD` itself, the installer's systemd drop-in never
reaches MPC. On Hakai, append the library to each `export LD_PRELOAD=` line of `/usr/bin/az01-launch-MPC` (back the
file up first, and the root filesystem has to be remounted writable). `docs/ADDINS.md` in the catalog repo covers the
general case. Check it took with:

```sh
grep -c nova_audio /proc/$(pidof MPC)/maps     # above 0 means it is loaded
cat /data/mpc-addins/nova-audio/nova-audio.log
```

## Build

```sh
tools/build_armhf.sh            # Docker, arm32v7/gcc:11-bullseye: builds within the catalog's glibc ceiling
tools/test_host.sh              # offline: a fake libasound and a fake MPC, both sample layouts, under UBSan
```

## Status

Runs on one device: an MPC Live II (Gen 1) on MPC OS 3.9.1 with Hakai, tapping 2 channels at 44.1 kHz, S32_LE
interleaved. Other models, channel counts and sample formats are handled in code but have not been run on hardware.

## Credit and licence

MIT. The hooking approach (stream recognition in `snd_pcm_open`/`hw_params`/`close`, resolution with
`dlsym(RTLD_NEXT)`, and the audio-thread rules) follows
[mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio) by Jacob Sabella, MIT.
It builds against the ALSA headers (`libasound2-dev`, installed by the build script in the container) and loads
libasound itself at runtime with `dlopen`, so nothing is linked against it.
