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

/* switchres monitor preset. This is what decides whether a PC video mode reaches the
   display as it was rendered or as something the display can actually scan: a 640x480
   70Hz VGA mode is 31kHz progressive, which an arcade_15 monitor cannot show at all and a
   PC monitor shows natively. Default arcade_15. */
extern char groovy_mister_monitor[32];

/* What to do when the preset only admits the mode as an interlaced one.

   GROOVY_MISTER_INTERLACE_SPLIT is the useful setting and the default: 86Box renders a
   whole progressive frame, so the frame is sent intact and the core splits it into fields.
   It must never be sent as interlace mode 1 ("fields from client"), which makes the core
   expect half the lines per blit against a buffer holding all of them.

   GROOVY_MISTER_INTERLACE_NEVER refuses interlaced modelines outright, for a monitor that
   can scan the mode natively - pair it with a preset that reaches 31kHz. */
#define GROOVY_MISTER_INTERLACE_NEVER 0
#define GROOVY_MISTER_INTERLACE_SPLIT 2
extern int  groovy_mister_interlace;

/* Called once at startup and shutdown; safe to call when disabled. */
extern void groovy_mister_init(void);
extern void groovy_mister_close(void);

/* Called from blit_thread with a completed frame. `buf` is 86Box's target_buffer for
 * that monitor; x/y/w/h is the region the card rendered. Returns immediately when
 * disabled or not connected. */
extern void groovy_mister_blit(int x, int y, int w, int h, int monitor_index);

/* One tick of mixed emulated audio, from sound.c's sound_thread.
 *
 * `buf` is 86Box's finished output buffer - float or int16 depending on which sound
 * backend is in use, which is what sound_is_float says - and `samples` is the count of
 * interleaved stereo samples in it (frames * 2).
 *
 * This only queues. The datagram itself goes out from the blit thread, because the
 * client's Windows RIO send path is not thread-safe and a send issued from another
 * thread is dropped without an error; the blit thread owns the socket, so it is the one
 * that drains this. */
extern void groovy_mister_audio_frame(const void *buf, int samples, int is_float);

#ifdef __cplusplus
}
#endif

#endif /* EMU_GROOVY_MISTER_H */
