/*
 * 86Box GroovyMiSTer output.
 *
 * Streams 86Box's rendered frames to a MiSTer FPGA running the GroovyNLC core, so the
 * emulated machine drives a real CRT or arcade monitor over the network instead of a
 * host display. The modeline is computed from the emulated video mode by switchres,
 * which is vendored alongside the Groovy client in src/switchres.
 *
 * Everything here runs on 86Box's blit thread (video.c's blit_thread), which is the
 * one place every video card hands over a completed frame, so no per-card changes are
 * needed and the work stays off the emulation thread.
 */

#ifndef EMU_GROOVY_MISTER_H
#define EMU_GROOVY_MISTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set from the config file; see config.c. */
extern int  groovy_mister_enabled;
extern char groovy_mister_host[64];
extern int  groovy_mister_codec;      /* 0 raw, 1 LZ4, 7 NLC (Lz4FramesCode) */
extern int  groovy_mister_near_level; /* NLC near-lossless level, 0 = lossless */
extern int  groovy_mister_audio;      /* mirror emulated audio to the MiSTer */
extern int  groovy_mister_mtu;

/* Called once at startup and shutdown; safe to call when disabled. */
extern void groovy_mister_init(void);
extern void groovy_mister_close(void);

/* Called from blit_thread with a completed frame. `buf` is 86Box's target_buffer for
 * that monitor; x/y/w/h is the region the card rendered. Returns immediately when
 * disabled or not connected. */
extern void groovy_mister_blit(int x, int y, int w, int h, int monitor_index);

/* Emulated audio, s16 stereo interleaved, from the sound thread. */
extern void groovy_mister_audio_frame(const int16_t *samples, int count);

#ifdef __cplusplus
}
#endif

#endif /* EMU_GROOVY_MISTER_H */
