/*
 * 86Box GroovyMiSTer output. See include/86box/groovy_mister.h.
 *
 * The emulated machine's frames go to a MiSTer running the GroovyNLC core instead of a
 * host display. switchres turns the emulated video mode into a CRT modeline; the Groovy
 * client ships the frames.
 *
 * Threading, which is the whole shape of this file:
 *
 *   Blit thread    groovy_mister_blit(): works out the mode, repacks the rendered region
 *                  into a staging frame, hands it over and returns. Nothing here touches
 *                  the socket and nothing here waits.
 *
 *                  It cannot wait, because video.c's blit path is not off to one side -
 *                  video_blit_memtoscreen_monitor() calls video_wait_for_blit_monitor()
 *                  on the EMULATION thread before every frame, so whatever this function
 *                  costs is charged straight to the emulated machine. Encoding and
 *                  raster-chasing here throttled a 70Hz VGA mode to 14Hz, and because the
 *                  refresh used to be measured by counting these calls, the slowdown fed
 *                  back into the modeline and made itself worse on the next frame.
 *
 *   Sender thread  Sole owner of the Groovy video/audio socket. Applies a pending
 *                  modeline, drains the audio ring, blits, and raster-chases the CRT.
 *                  Blocking is free here. Closing belongs here too: the client's Windows
 *                  RIO send path defers sends, so a CMD_CLOSE issued from another thread
 *                  is dropped and the MiSTer holds our last frame.
 *
 *   Sound thread   groovy_mister_audio_frame(): writes the audio ring, nothing else.
 */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/sound.h>
#include <86box/thread.h>
#include <86box/video.h>
#include <86box/groovy_mister.h>

#include "../groovymister/groovymister_wrapper.h"
#include "../switchres/switchres_wrapper.h"

int  groovy_mister_enabled    = 0;
char groovy_mister_host[64]   = "";
int  groovy_mister_codec      = NLC;
int  groovy_mister_near_level = 0;
int  groovy_mister_audio      = 1;
int  groovy_mister_mtu        = 1500;
char groovy_mister_monitor[32] = "arcade_15";
int  groovy_mister_interlace   = GROOVY_MISTER_INTERLACE_SPLIT;

static int gm_connected = 0;
static int gm_sr_live   = 0;

/* ------------------------------------------------------------------------------------
 * Blit thread -> sender thread handover
 * ------------------------------------------------------------------------------------
 *
 * One slot, newest wins. Emulation must never be paced by the network - see the note at
 * the top of the file - so a frame the sender has not collected yet is overwritten rather
 * than queued behind. Dropping a frame costs one repeated image on the CRT; blocking costs
 * emulation speed.
 *
 * The staging buffer is the size of the client's own blit buffer, so a frame that passes
 * the mode gate fits here by construction.
 */
#define GM_MAX_BLIT_BYTES (2048 * 1024)

static mutex_t *gm_frame_lock     = NULL;
static event_t *gm_frame_event    = NULL;
static uint8_t *gm_frame_buf      = NULL;
static int      gm_frame_bytes    = 0; /* bytes valid in gm_frame_buf, 0 = empty */
static uint64_t gm_frames_dropped = 0;

/* Modeline waiting to be programmed into the core. Computed on the blit thread (pure
 * arithmetic, microseconds) but sent from the sender thread, because CMD_SWITCHRES is I/O
 * with an ACK round trip. */
typedef struct {
    double   pclock;
    uint16_t h_active, h_begin, h_end, h_total;
    uint16_t v_active, v_begin, v_end, v_total;
    uint8_t  interlace;
} gm_modeline_t;

static gm_modeline_t gm_pending_modeline;
static int           gm_modeline_pending = 0;

/* The modeline currently in force. Compared against before staging a new one, because the
 * measured refresh is a whole number of frames per emulated second and jitters by 1Hz
 * either side of the true rate (70/71 for a 70.09Hz VGA text mode) - and switchres maps
 * both to exactly the same timings. Re-sending them would reset the core's frame ordering
 * once a second for no change at all. */
static gm_modeline_t gm_active_modeline;

static thread_t  *gm_sender      = NULL;
static atomic_int gm_sender_quit = 0;

/* ------------------------------------------------------------------------------------
 * Audio ring: sound thread -> sender thread
 * ------------------------------------------------------------------------------------
 *
 * The sound thread must not touch the socket either. ~256KB, about 1.3 seconds at 48kHz
 * stereo. Overflow drops oldest, so a stalled sender costs stale samples rather than
 * unbounded latency and the stream self-corrects.
 */
#define GM_AUDIO_CAPACITY    (256 * 1024)
#define GM_AUDIO_FRAME_BYTES (2 * (int) sizeof(int16_t)) /* stereo s16 */

/* CMD_AUDIO carries its payload size in a uint16_t, so the cast must not be able to wrap:
 * a 65536-byte drain would cast to 0 and put an empty CMD_AUDIO on the wire, which the core
 * rejects with UDP_ERROR. Cap well inside uint16_t, at a whole number of stereo frames, and
 * small enough not to dump a large stale burst in one packet (~85ms at 48kHz stereo; steady
 * state is ~1.6KB per frame at 60Hz). */
#define GM_AUDIO_MAX_SEND (16 * 1024)

static mutex_t *gm_audio_lock = NULL;
static uint8_t *gm_audio_ring = NULL;
static int      gm_audio_read = 0;
static int      gm_audio_size = 0;
static int      gm_audio_rate = RATE_OFF; /* SoundRateCode negotiated at CMD_INIT */
static int      gm_audio_live = 0;

static int gm_logged_first_audio = 0;
static int gm_logged_audio_off   = 0;

/* The mode currently programmed into the core. A new one is only sent when the emulated
 * machine actually changes mode, because CMD_SWITCHRES resets the core's frame ordering. */
static int gm_mode_w     = 0;
static int gm_mode_h     = 0;
static int gm_mode_hz    = 0;
static int gm_mode_valid = 0;

static void
gm_log(const char *fmt, ...)
{
    va_list ap;
    char    buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pclog("GroovyMiSTer: %s\n", buf);
}

/* The client hands over a fully formatted line, and may call from any thread. */
static void
gm_lib_log(const char *msg)
{
    char   buf[512];
    size_t n;

    if (msg == NULL)
        return;

    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* the client's lines already end in \n; pclog adds its own */
    n = strlen(buf);
    while (n && ((buf[n - 1] == '\n') || (buf[n - 1] == '\r')))
        buf[--n] = '\0';

    pclog("GroovyMiSTer[lib]: %s\n", buf);
}

/* ------------------------------------------------------------------------------------
 * Audio
 * ------------------------------------------------------------------------------------ */

/* Sound thread: convert to the wire format and queue. No I/O here. */
void
groovy_mister_audio_frame(const void *buf, int samples, int is_float)
{
    int incoming;
    int write_pos;
    int first;

    if (!gm_audio_live || (buf == NULL) || (samples <= 0))
        return;

    incoming = samples * (int) sizeof(int16_t);
    if (incoming > GM_AUDIO_CAPACITY)
        return; /* a tick larger than the whole ring means something upstream is wrong */

    thread_wait_mutex(gm_audio_lock);

    if (gm_audio_ring == NULL) {
        thread_release_mutex(gm_audio_lock);
        return;
    }

    /* Drop-oldest: make room by advancing the read cursor. */
    if ((gm_audio_size + incoming) > GM_AUDIO_CAPACITY) {
        const int overflow = (gm_audio_size + incoming) - GM_AUDIO_CAPACITY;
        gm_audio_read      = (gm_audio_read + overflow) % GM_AUDIO_CAPACITY;
        gm_audio_size -= overflow;
    }

    write_pos = (gm_audio_read + gm_audio_size) % GM_AUDIO_CAPACITY;

    if (is_float) {
        /* 86Box scales its mix into [-1, 1] on the way into the float buffer, but the DC
         * filter and per-device gain can push past it, so clamp before scaling back or
         * loud passages wrap and click. */
        const float *in = (const float *) buf;
        for (int i = 0; i < samples; i++) {
            float   v = in[i];
            int16_t s;

            if (v > 1.0f)
                v = 1.0f;
            else if (v < -1.0f)
                v = -1.0f;

            s = (int16_t) (v * 32767.0f);
            memcpy(&gm_audio_ring[write_pos], &s, sizeof(s));
            write_pos = (write_pos + (int) sizeof(s)) % GM_AUDIO_CAPACITY;
        }
    } else {
        /* Already the wire format, so this is a copy - but the ring wraps, so it can be
         * two. */
        first = GM_AUDIO_CAPACITY - write_pos;
        if (first > incoming)
            first = incoming;
        memcpy(&gm_audio_ring[write_pos], buf, (size_t) first);
        if (incoming > first)
            memcpy(&gm_audio_ring[0], (const uint8_t *) buf + first, (size_t) (incoming - first));
    }

    gm_audio_size += incoming;

    thread_release_mutex(gm_audio_lock);
}

/* Sender thread: send whatever the sound thread has queued. */
static void
gm_drain_audio(void)
{
    static uint8_t scratch[GM_AUDIO_MAX_SEND];

    char *abuf;
    int   avail;
    int   first;

    if (!gm_audio_live)
        return;

    thread_wait_mutex(gm_audio_lock);

    if (gm_audio_ring == NULL) {
        thread_release_mutex(gm_audio_lock);
        return;
    }

    avail = (gm_audio_size < GM_AUDIO_MAX_SEND) ? gm_audio_size : GM_AUDIO_MAX_SEND;
    /* Whole stereo frames only; a half-frame desyncs the L/R interleave from there on. */
    avail -= (avail % GM_AUDIO_FRAME_BYTES);
    if (avail <= 0) {
        thread_release_mutex(gm_audio_lock);
        return;
    }

    first = GM_AUDIO_CAPACITY - gm_audio_read;
    if (first > avail)
        first = avail;
    memcpy(scratch, &gm_audio_ring[gm_audio_read], (size_t) first);
    if (avail > first)
        memcpy(scratch + first, &gm_audio_ring[0], (size_t) (avail - first));

    gm_audio_read = (gm_audio_read + avail) % GM_AUDIO_CAPACITY;
    gm_audio_size -= avail;

    thread_release_mutex(gm_audio_lock);

    abuf = gmw_get_pBufferAudio();
    if (abuf == NULL)
        return;

    memcpy(abuf, scratch, (size_t) avail);
    gmw_audio((uint16_t) avail);

    /* One-shot, so a silent MiSTer can be told apart from one that is never sent anything.
     * Without it, "the core has audio off", "the emulated machine is making no sound" and
     * "the ring is never drained" are all just silence. */
    if (!gm_logged_first_audio) {
        gm_logged_first_audio = 1;
        gm_log("first audio datagram sent (%d bytes)", avail);
    }
}

/* ------------------------------------------------------------------------------------
 * Sender thread
 * ------------------------------------------------------------------------------------ */

static void
gm_sender_thread(void *priv)
{
    /* Frame numbers must stay ahead of the core's own free-running counter: the protocol
     * displays frames in counter order and discards anything behind, and the client's
     * watchdog reconnects when it sees no ACK advance. */
    uint32_t frame = 0;

    (void) priv;

    while (!atomic_load(&gm_sender_quit)) {
        gmw_fpgaStatus st;
        int            bytes    = 0;
        char          *blit_buf = NULL;

        /* A timeout rather than an indefinite wait, so the ACK poll below keeps running
         * while the emulated machine is producing nothing - during a mode change, or before
         * the VM has been started. */
        thread_wait_event(gm_frame_event, 100);
        thread_reset_event(gm_frame_event);

        if (atomic_load(&gm_sender_quit))
            break;

        /* Receive pending ACKs. Not optional: the client updates fpga.frameEcho only inside
         * getACK(), and its CmdBlit watchdog force-reconnects when frameEcho stops advancing
         * for 10 blits. getStatus() only copies that cache, and gmw_blit() never receives.
         * fpga.frame is the core's own counter and free-runs at the CRT's refresh rate
         * whether or not we blit, so polling on idle ticks is what stops the first frame
         * after a quiet spell being numbered behind the core and dropped as stale. */
        gmw_getACK(0);

        /* Any modeline change must land before the frame that depends on it. */
        thread_wait_mutex(gm_frame_lock);
        if (gm_modeline_pending) {
            const gm_modeline_t m = gm_pending_modeline;
            gm_modeline_pending   = 0;
            thread_release_mutex(gm_frame_lock);

            if (gmw_switchres(m.pclock, m.h_active, m.h_begin, m.h_end, m.h_total, m.v_active,
                              m.v_begin, m.v_end, m.v_total, m.interlace)
                != 0) {
                /* The core zeroes its modeline on CMD_INIT and discards every video packet
                 * until a CMD_SWITCHRES lands, so an unacknowledged one is not cosmetic -
                 * it is a dead session. The client retried internally and replays the
                 * stashed modeline after its own reconnect, so say so and carry on. */
                gm_log("the core did not acknowledge the modeline; video may stay blank until "
                       "the next reconnect");
            }
            thread_wait_mutex(gm_frame_lock);
        }

        /* Collect the staged frame, if there is one. */
        bytes = gm_frame_bytes;
        if (bytes > 0) {
            blit_buf = gmw_get_pBufferBlit(0);
            if (blit_buf != NULL)
                memcpy(blit_buf, gm_frame_buf, (size_t) bytes);
            else
                bytes = 0;
            gm_frame_bytes = 0;
        }
        thread_release_mutex(gm_frame_lock);

        if (bytes <= 0)
            continue;

        gmw_getStatus(&st);

        /* Adopt the core's position whenever it leads. The forward jump is unbounded on
         * purpose: st.frame free-runs, so after a long quiet spell the core is legitimately
         * thousands of frames ahead, and clamping would leave every frame stale. */
        ++frame;
        if (st.frame > frame)
            frame = st.frame + 1;

        /* Audio first: the core wants it ahead of the frame it belongs to. st.audio is the
         * core's own confirmation that audio is on for this session, so an unwanted
         * CMD_AUDIO never goes out. */
        if (st.audio) {
            gm_drain_audio();
        } else if (gm_audio_live && !gm_logged_audio_off && (st.frame != 0)) {
            /* Negotiated at CMD_INIT but the core says no, which means its OSD has audio
             * switched off. Say so once: otherwise it is indistinguishable from a bug here.
             *
             * st.frame != 0 is the guard that the status cache has actually been filled by
             * an ACK. It is all zeroes until the first one lands, so without this the very
             * first frame of every session reports audio off whether it is or not. */
            gm_logged_audio_off = 1;
            gm_log("the core reports audio off for this session; check OSD -> Audio on the "
                   "MiSTer");
        }

        gmw_blit(frame, 0, 1, 0, 0);

        /* Raster-chase the CRT. Free to block: this thread is not on the emulation path,
         * and it is also what drains the client's RIO send completion queue. */
        gmw_waitSync();
    }

    /* Tell the MiSTer we are leaving so it returns to its connection-search screen instead
     * of holding the last frame. Sent three times; one lost datagram would strand it. On
     * this thread because it is the one that owns the socket. */
    if (gm_connected) {
        for (int i = 0; i < 3; i++)
            gmw_send_close();
        gmw_close();
    }
}

/* ------------------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------------------ */

void
groovy_mister_init(void)
{
    int rc;

    gm_connected          = 0;
    gm_mode_valid         = 0;
    gm_mode_w             = 0;
    gm_mode_h             = 0;
    gm_mode_hz            = 0;
    gm_logged_first_audio = 0;
    gm_logged_audio_off   = 0;
    gm_frames_dropped     = 0;
    memset(&gm_active_modeline, 0, sizeof(gm_active_modeline));
    atomic_store(&gm_sender_quit, 0);

    if (!groovy_mister_enabled || (groovy_mister_host[0] == '\0'))
        return;

    /* Log sink and codec knobs ride the CMD_INIT packing, so they must be set before
     * gmw_init, not after. */
    gmw_set_log_callback(&gm_lib_log, 0); /* 0 = errors and handshake only */

    if (groovy_mister_codec == NLC) {
        gmw_set_near_level(groovy_mister_near_level);
        gmw_set_nlc_pack(2);      /* Rice */
        gmw_set_nlc_disp_mode(2); /* autonomous decode engine */
    }

    gmw_set_auto_reconnect(1);

    /* The sample rate is baked into CMD_INIT, so it has to match what 86Box is actually
     * mixing at - it is a per-VM setting (44.1 or 48kHz), not a constant. soundRate and
     * soundChan are enum codes, not a rate and a count. */
    gm_audio_rate = RATE_OFF;
    if (groovy_mister_audio) {
        switch (sound_sample_rate) {
            case 44100:
                gm_audio_rate = RATE_44100;
                break;
            case 48000:
                gm_audio_rate = RATE_48000;
                break;
            default:
                gm_log("unsupported sound sample rate %d; audio will not be streamed",
                       sound_sample_rate);
                break;
        }
    }

    /* RGB888: 86Box's target_buffer is 32-bit, so this is a straight 4->3 byte repack with
     * no colour loss. RGB565 would halve the wire bytes but the NLC encoder does not take
     * it, and falls back to sending raw - which is bigger, not smaller. */
    rc = gmw_init(groovy_mister_host, (uint8_t) groovy_mister_codec, (uint32_t) gm_audio_rate,
                  (uint8_t) ((gm_audio_rate == RATE_OFF) ? CHAN_OFF : CHAN_STEREO), RGB_888,
                  (uint16_t) groovy_mister_mtu);
    if (rc != 0) {
        gm_log("could not connect to %s (gmw_init = %d); output disabled for this session",
               groovy_mister_host, rc);
        return;
    }

    gm_frame_lock  = thread_create_mutex();
    gm_frame_event = thread_create_event();
    gm_frame_buf   = calloc(GM_MAX_BLIT_BYTES, 1);
    if ((gm_frame_lock == NULL) || (gm_frame_event == NULL) || (gm_frame_buf == NULL)) {
        gm_log("could not allocate the frame handover; output disabled for this session");
        gmw_close();
        return;
    }

    gm_connected = 1;
    gm_log("connected to %s", groovy_mister_host);

    /* Only now, so the sound thread cannot queue into a ring that no one will drain. */
    if (gm_audio_rate != RATE_OFF) {
        gm_audio_lock = thread_create_mutex();
        gm_audio_ring = calloc(GM_AUDIO_CAPACITY, 1);

        if ((gm_audio_lock != NULL) && (gm_audio_ring != NULL)) {
            gm_audio_read = 0;
            gm_audio_size = 0;
            gm_audio_live = 1;
            gm_log("audio enabled (%dHz stereo)", sound_sample_rate);
        } else {
            gm_log("could not allocate the audio ring; audio disabled for this session");
        }
    }

    /* switchres as a pure modeline calculator: the "dummy" display opens no host display
     * and creates no custom video backend, so it only ever computes timings. Order is
     * load-bearing - the preset is resolved inside sr_init_disp, so it must be chosen
     * before that call.
     *
     * The preset is the setting that matters most here. A PC's video modes run from 15kHz
     * (320x200 doublescanned) up to 31kHz (640x480 at 60Hz, 640x350 at 70Hz), so a 15kHz
     * band turns the top of that range into interlaced modes - which is what an arcade
     * monitor needs - while a tri-sync or PC monitor would rather have them as rendered. */
    sr_init();
    sr_set_monitor(groovy_mister_monitor[0] ? groovy_mister_monitor : "arcade_15");
    sr_init_disp("dummy", NULL);
    gm_sr_live = 1;

    gm_sender = thread_create(gm_sender_thread, NULL);
}

void
groovy_mister_close(void)
{
    /* Stop the producers before the buffers they write into go away. */
    gm_audio_live = 0;

    if (gm_sender != NULL) {
        atomic_store(&gm_sender_quit, 1);
        thread_set_event(gm_frame_event);
        thread_wait(gm_sender); /* sends CMD_CLOSE and closes the socket on its way out */
        gm_sender = NULL;
    } else if (gm_connected) {
        /* Never got a sender thread, so close it here. */
        gmw_send_close();
        plat_delay_ms(2);
        gmw_close();
    }
    gm_connected = 0;

    if (gm_sr_live) {
        sr_deinit();
        gm_sr_live = 0;
    }

    if (gm_frame_lock != NULL) {
        thread_wait_mutex(gm_frame_lock);
        free(gm_frame_buf);
        gm_frame_buf   = NULL;
        gm_frame_bytes = 0;
        thread_release_mutex(gm_frame_lock);
        thread_close_mutex(gm_frame_lock);
        gm_frame_lock = NULL;
    }
    if (gm_frame_event != NULL) {
        thread_destroy_event(gm_frame_event);
        gm_frame_event = NULL;
    }

    if (gm_audio_lock != NULL) {
        thread_wait_mutex(gm_audio_lock);
        free(gm_audio_ring);
        gm_audio_ring = NULL;
        gm_audio_read = 0;
        gm_audio_size = 0;
        thread_release_mutex(gm_audio_lock);
        thread_close_mutex(gm_audio_lock);
        gm_audio_lock = NULL;
    }

    if (gm_frames_dropped)
        gm_log("%llu frames were dropped at the handover", (unsigned long long) gm_frames_dropped);

    gm_audio_rate = RATE_OFF;
    gm_mode_valid = 0;
}

/* ------------------------------------------------------------------------------------
 * Blit thread
 * ------------------------------------------------------------------------------------ */

/* Ask switchres for a modeline and stage it for the sender. Returns 1 when frames of this
 * size can be handed over. */
static int
gm_set_mode(int w, int h, int hz)
{
    gm_modeline_t ml = { 0 };
    sr_mode       mode;
    int           bytes;

    memset(&mode, 0, sizeof(mode));

    if (!sr_add_mode(w, h, (double) hz, 0, &mode) || (mode.width <= 0) || (mode.height <= 0)) {
        gm_log("switchres could not find a mode for %dx%d @ %dHz", w, h, hz);
        return 0;
    }

    ml.pclock    = (double) mode.pclock / 1000000.0;
    ml.h_active  = (uint16_t) mode.width;
    ml.h_begin   = (uint16_t) mode.hbegin;
    ml.h_end     = (uint16_t) mode.hend;
    ml.h_total   = (uint16_t) mode.htotal;
    ml.v_active  = (uint16_t) mode.height;
    ml.v_begin   = (uint16_t) mode.vbegin;
    ml.v_end     = (uint16_t) mode.vend;
    ml.v_total   = (uint16_t) mode.vtotal;

    /* 86Box always hands over a whole progressive frame, so an interlaced modeline goes out
     * as mode 2 - progressive framebuffer, core splits it into fields - and never as mode 1,
     * "fields from client", which makes the core read half as many lines per blit as the
     * buffer actually holds. */
    ml.interlace = mode.interlace ? (uint8_t) groovy_mister_interlace : 0;

    if (mode.interlace && (groovy_mister_interlace == GROOVY_MISTER_INTERLACE_NEVER)) {
        gm_log("%dx%d @ %dHz only fits an interlaced modeline on monitor preset '%s', but "
               "interlacing is switched off. Nothing will be streamed for this mode - pick a "
               "preset that reaches 31kHz, or allow interlacing.",
               w, h, hz, groovy_mister_monitor);
        return 0;
    }

    /* The client derives the stream length from the modeline with no clamp in between, so an
     * oversized mode walks off the end of a RIO-registered allocation. A property of the
     * client rather than of the display, so it is refused outright. Counted from the wire
     * interlace value, not switchres's: only mode 1 sends half a frame per blit, and that is
     * the one mode this integration never uses. */
    bytes = ml.h_active * ((ml.interlace == 1) ? (ml.v_active / 2) : ml.v_active) * 3;
    if (bytes > GM_MAX_BLIT_BYTES) {
        gm_log("refusing %dx%d: %d bytes per blit is over the %d-byte buffer", ml.h_active,
               ml.v_active, bytes, GM_MAX_BLIT_BYTES);
        return 0;
    }

    /* Same timings as what the core is already displaying: nothing to do. */
    if (gm_mode_valid && !memcmp(&ml, &gm_active_modeline, sizeof(ml)))
        return 1;

    thread_wait_mutex(gm_frame_lock);
    gm_pending_modeline = ml;
    gm_modeline_pending = 1;
    /* A staged frame belongs to the old mode; the core would read it with the new one. */
    gm_frame_bytes = 0;
    thread_release_mutex(gm_frame_lock);

    gm_active_modeline = ml;

    gm_log("mode %dx%d @ %dHz -> %.4fMHz h(%d %d %d) v(%d %d %d) interlace=%d", w, h, hz,
           (double) mode.pclock / 1000000.0, mode.hbegin, mode.hend, mode.htotal, mode.vbegin,
           mode.vend, mode.vtotal, ml.interlace);

    return 1;
}

void
groovy_mister_blit(int x, int y, int w, int h, int monitor_index)
{
    monitor_t      *mon = &monitors[monitor_index];
    const bitmap_t *buf;
    uint8_t        *dst;
    int             hz;
    int             row;
    int             col;

    /* One-shot trace of the first frame handed over, so a silent output can be told apart
     * from one that is never called at all. */
    static int first_seen = 0;
    if (!first_seen) {
        first_seen = 1;
        gm_log("first frame offered: %dx%d at (%d,%d) monitor=%d buffer=%p connected=%d", w, h, x,
               y, monitor_index, (void *) mon->target_buffer, gm_connected);
    }

    if (!gm_connected || (w <= 0) || (h <= 0))
        return;

    /* One monitor goes to the MiSTer; a second head stays on the host. */
    if (monitor_index != 0)
        return;

    buf = mon->target_buffer;
    if ((buf == NULL) || (buf->dat == NULL))
        return;

    /* 86Box's own refresh measurement, not one of ours.
     *
     * mon_renderedframes is incremented in video_blit_memtoscreen_monitor() on the
     * emulation thread and latched into mon_actualrenderedframes once per second of
     * EMULATED time, so it is frames per emulated second - the video mode's nominal
     * refresh - and stays correct when the host cannot keep up.
     *
     * Counting arrivals here instead would measure this pipeline's own throughput: a slow
     * frame would lower the rate, which would lower the modeline, which would make the next
     * frame slower still. That spiral took a 70Hz VGA mode down to 14Hz. */
    hz = atomic_load(&mon->mon_actualrenderedframes);
    if ((hz < 40) || (hz > 130))
        return; /* not latched yet, or not a rate any CRT should be asked for */

    /* Anything this small is a transient during a mode change, not a video mode worth
     * programming a CRT for - the BIOS offers an 80x400 region on the way up. */
    if ((w < 160) || (h < 100))
        return;

    /* Only reprogram on a real mode change. Every mode set resets the core's frame
     * ordering, and mon_actualrenderedframes is a whole number of frames per emulated
     * second, so it only moves when the mode really does. */
    if (!gm_mode_valid || (w != gm_mode_w) || (h != gm_mode_h) || (hz != gm_mode_hz)) {
        gm_mode_valid = gm_set_mode(w, h, hz);
        if (!gm_mode_valid)
            return;
        gm_mode_w  = w;
        gm_mode_h  = h;
        gm_mode_hz = hz;
    }

    /* Repack straight into the staging frame and hand it over. 86Box stores each row as
     * 32-bit xRGB and gives per-row pointers, so the rendered region is copied out directly
     * rather than reconstructed from a stride. The core wants packed BGR888. */
    thread_wait_mutex(gm_frame_lock);

    if (gm_frame_buf == NULL) {
        thread_release_mutex(gm_frame_lock);
        return;
    }

    if (gm_frame_bytes > 0)
        gm_frames_dropped++; /* newest wins; see the handover note above */

    dst = gm_frame_buf;
    for (row = 0; row < h; row++) {
        const uint32_t *src = buf->line[y + row] + x;
        for (col = 0; col < w; col++) {
            const uint32_t px = src[col];
            *dst++            = (uint8_t) (px & 0xff);         /* B */
            *dst++            = (uint8_t) ((px >> 8) & 0xff);  /* G */
            *dst++            = (uint8_t) ((px >> 16) & 0xff); /* R */
        }
    }
    gm_frame_bytes = (int) (dst - gm_frame_buf);

    thread_release_mutex(gm_frame_lock);

    thread_set_event(gm_frame_event);
}
