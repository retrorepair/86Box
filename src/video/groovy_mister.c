/*
 * 86Box GroovyMiSTer output. See include/86box/groovy_mister.h.
 *
 * The emulated machine's frames go to a MiSTer running the GroovyNLC core instead of a
 * host display. switchres turns the emulated video mode into a CRT modeline; the Groovy
 * client ships the frames.
 *
 * All of this runs on the blit thread, so the only emulation-side cost is the existing
 * handover that video.c already does.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
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

static int      gm_connected   = 0;
static int      gm_sr_live     = 0;
static uint32_t gm_frame       = 0;

/* The mode currently programmed into the core. A new one is only sent when the emulated
 * machine actually changes mode, because CMD_SWITCHRES resets the core's frame ordering. */
static int    gm_mode_w     = 0;
static int    gm_mode_h     = 0;
static int    gm_mode_hz_x100 = 0;   /* refresh to 2dp, so 59.92 and 60.00 are different modes */
static int    gm_mode_valid = 0;

/* Refresh is measured rather than asked for: 86Box has no per-card refresh field, it
 * counts rendered frames. An average over a second settles well inside the tolerance
 * switchres matches modes with, and costs nothing. */
static uint64_t gm_rate_t0     = 0;
static int      gm_rate_frames = 0;
static double   gm_rate_hz     = 0.0;

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

void
groovy_mister_init(void)
{
    int rc;

    gm_connected   = 0;
    gm_mode_valid  = 0;
    gm_frame       = 0;
    gm_rate_t0     = 0;
    gm_rate_frames = 0;
    gm_rate_hz     = 0.0;

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

    /* RGB888: 86Box's target_buffer is 32-bit, so this is a straight 4->3 byte repack
     * with no colour loss. RGB565 would halve the wire bytes but the NLC encoder does
     * not take it, and falls back to sending raw - which is bigger, not smaller. */
    /* soundRate/soundChan are enum codes, not a rate and a count. */
    rc = gmw_init(groovy_mister_host, (uint8_t) groovy_mister_codec, RATE_48000, CHAN_STEREO,
                  RGB_888, (uint16_t) groovy_mister_mtu);
    if (rc != 0) {
        gm_log("could not connect to %s (gmw_init = %d); output disabled for this session",
               groovy_mister_host, rc);
        return;
    }

    gm_connected = 1;
    gm_log("connected to %s", groovy_mister_host);

    /* switchres as a pure modeline calculator: the "dummy" display opens no host display
     * and creates no custom video backend, so it only ever computes timings. Order is
     * load-bearing - the preset is resolved inside sr_init_disp, so it must be chosen
     * before that call. */
    sr_init();
    sr_set_monitor("arcade_15");
    sr_init_disp("dummy", NULL);
    gm_sr_live = 1;
}

void
groovy_mister_close(void)
{
    if (gm_connected) {
        gmw_send_close();
        plat_delay_ms(2);
        gmw_close();
        gm_connected = 0;
    }
    if (gm_sr_live) {
        sr_deinit();
        gm_sr_live = 0;
    }
    gm_mode_valid = 0;
}

/* Ask switchres for a modeline and program it into the core. Returns 1 when the core is
 * ready to take frames of this size. */
static int
gm_set_mode(int w, int h, double hz)
{
    sr_mode mode;
    int     rc;

    memset(&mode, 0, sizeof(mode));

    if (!sr_add_mode(w, h, hz, 0, &mode) || (mode.width <= 0) || (mode.height <= 0)) {
        gm_log("switchres could not find a mode for %dx%d @ %.2fHz", w, h, hz);
        return 0;
    }

    rc = gmw_switchres((double) mode.pclock / 1000000.0,
                       (uint16_t) mode.width, (uint16_t) mode.hbegin, (uint16_t) mode.hend, (uint16_t) mode.htotal,
                       (uint16_t) mode.height, (uint16_t) mode.vbegin, (uint16_t) mode.vend, (uint16_t) mode.vtotal,
                       (uint8_t) mode.interlace);
    if (rc != 0) {
        /* The core zeroes its modeline on CMD_INIT and discards every video packet until
         * a CMD_SWITCHRES lands, so an unacknowledged one is not cosmetic - it is a dead
         * session. Leave the mode unset and retry on the next frame. */
        gm_log("core did not acknowledge the modeline for %dx%d; retrying next frame", w, h);
        return 0;
    }

    gm_log("mode %dx%d @ %.2fHz -> %.4fMHz h(%d %d %d) v(%d %d %d) interlace=%d",
           w, h, hz, (double) mode.pclock / 1000000.0,
           mode.hbegin, mode.hend, mode.htotal,
           mode.vbegin, mode.vend, mode.vtotal, mode.interlace);

    return 1;
}

void
groovy_mister_blit(int x, int y, int w, int h, int monitor_index)
{
    const monitor_t *mon = &monitors[monitor_index];
    const bitmap_t  *buf;
    gmw_fpgaStatus   st;
    char            *dst;
    uint64_t         now;
    int              hz_x100;
    int              row;
    int              col;

    /* One-shot trace of the first frame handed over, so a silent output can be told
     * apart from one that is never called at all. */
    static int first_seen = 0;
    if (!first_seen) {
        first_seen = 1;
        gm_log("first frame offered: %dx%d at (%d,%d) monitor=%d buffer=%p connected=%d",
               w, h, x, y, monitor_index, (void *) mon->target_buffer, gm_connected);
    }

    if (!gm_connected || (w <= 0) || (h <= 0))
        return;

    /* One monitor goes to the MiSTer; a second head stays on the host. */
    if (monitor_index != 0)
        return;

    buf = mon->target_buffer;
    if ((buf == NULL) || (buf->dat == NULL))
        return;

    /* Measure the refresh rate over a one-second window. */
    now = plat_get_ticks();
    if (gm_rate_t0 == 0)
        gm_rate_t0 = now;
    gm_rate_frames++;
    if ((now - gm_rate_t0) >= 1000) {
        gm_rate_hz     = (double) gm_rate_frames * 1000.0 / (double) (now - gm_rate_t0);
        gm_rate_t0     = now;
        gm_rate_frames = 0;
    }
    if (gm_rate_hz <= 0.0)
        return; /* nothing sent until the rate is known - the first second settles it */

    hz_x100 = (int) (gm_rate_hz * 100.0 + 0.5);

    /* Anything this small is a transient during a mode change, not a video mode worth
     * programming a CRT for - the BIOS offers an 80x400 region on the way up. */
    if ((w < 160) || (h < 100))
        return;

    /* Only reprogram on a real mode change. A measured refresh wanders by a few Hz while
     * the emulated machine settles (70.8 -> 65.6 -> 64.6 during POST), and every mode set
     * resets the core's frame ordering, so the rate has to move by more than that wander
     * to count. The size is exact, and that is what actually changes between modes. */
    if (!gm_mode_valid || (w != gm_mode_w) || (h != gm_mode_h) ||
        (abs(hz_x100 - gm_mode_hz_x100) > 300)) {
        gm_mode_valid = gm_set_mode(w, h, gm_rate_hz);
        if (!gm_mode_valid)
            return;
        gm_mode_w       = w;
        gm_mode_h       = h;
        gm_mode_hz_x100 = hz_x100;
    }

    dst = gmw_get_pBufferBlit(0);
    if (dst == NULL)
        return;

    /* 86Box stores each row as 32-bit xRGB and gives per-row pointers, so the rendered
     * region is copied out directly rather than reconstructed from a stride. The core
     * wants packed BGR888. */
    for (row = 0; row < h; row++) {
        const uint32_t *src = buf->line[y + row] + x;
        for (col = 0; col < w; col++) {
            const uint32_t px = src[col];
            *dst++            = (char) (px & 0xff);         /* B */
            *dst++            = (char) ((px >> 8) & 0xff);  /* G */
            *dst++            = (char) ((px >> 16) & 0xff); /* R */
        }
    }

    /* Frame numbers must stay ahead of the core's own free-running counter. The protocol
     * wants a monotonically increasing number, and the client's watchdog reconnects when
     * it sees no ACK advance - which is what happens if we restart at 0 after a mode set
     * while the core is still echoing frame 66. Adopt the core's position whenever it
     * leads, the same resync RetroArch and RPCS3 do. */
    gmw_getStatus(&st);
    ++gm_frame;
    if (st.frame > gm_frame)
        gm_frame = st.frame + 1;

    gmw_blit(gm_frame, 0, 1, 0, 0);
    gmw_waitSync();
}

void
groovy_mister_audio_frame(const int16_t *samples, int count)
{
    char *abuf;
    int   bytes;

    if (!gm_connected || !groovy_mister_audio || (samples == NULL) || (count <= 0))
        return;

    /* soundSize is a uint16_t and the client splits by MTU, so keep a frame's worth
     * well inside it. */
    bytes = count * (int) sizeof(int16_t);
    if (bytes > 60000)
        bytes = 60000;

    abuf = gmw_get_pBufferAudio();
    if (abuf == NULL)
        return;

    memcpy(abuf, samples, (size_t) bytes);
    gmw_audio((uint16_t) bytes);
}
