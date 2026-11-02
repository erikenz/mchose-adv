/*
 * mchose-adv — bring MCHOSE hall-effect keyboard "advanced key" output to Linux.
 *
 * Supported features: SOCD, RS (rapid switch / rappy-snappy), DKS, MT, TGL.
 * Tested on the MCHOSE Ace 68 -III (USB 3837:3003).
 *
 * The problem
 * -----------
 * The keyboard is a 3-interface composite HID device:
 *
 *   interface 0  (…:1.0)  normal boot keyboard        -> works out of the box
 *   interface 1  (…:1.1)  vendor / configuration      -> 0 keys
 *   interface 2  (…:1.2)  mouse + consumer + advanced -> Linux ignores its keys
 *
 * Keys resolved by the advanced-key features are NOT sent on interface 0.
 * The firmware sends them on interface 2 as:
 *
 *     report id 0x01, 16 bytes
 *     byte 0      = report id (0x01)
 *     bytes 1..15 = 15-byte NKRO bitmap,
 *                   bit(usage) = byte[1 + usage/8], bit (usage % 8)
 *
 * Verified on hardware:
 *     [  = usage 0x2F = 47 -> byte 6, bit 7 -> 0x80
 *     ]  = usage 0x30 = 48 -> byte 7, bit 0 -> 0x01
 *
 * (byte 1 is the first bitmap byte and covers the unassigned usages 0..7,
 *  which is why it is always 0x00.)
 *
 * Linux binds that interface and even advertises KEY_LEFTBRACE/KEY_RIGHTBRACE
 * on the resulting event device, but never emits an event for the report
 * above.  Windows parses the same collection correctly, which is why the
 * keyboard's SOCD works in a Windows guest and appears dead on Linux.
 *
 * This program reads that report and re-emits the keys through uinput.
 * It is strictly read-only with respect to the keyboard: it never writes to
 * the device and never grabs it, so the vendor web driver and other readers
 * keep working.  See docs/PROTOCOL.md for the full reverse-engineering notes.
 *
 * Build:  make
 * Run:    ./mchose-adv [-v] [--retry SECONDS] [hidraw-device]
 * Docs:   https://github.com/<your-user>/mchose-adv
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#define VENDOR_ID   0x3837
#define PRODUCT_ID  0x3003
#define REPORT_ID   0x01
#define BUF_LEN     16
#define BITMAP_OFF  1                              /* first bitmap byte          */
#define BITMAP_LEN  (BUF_LEN - BITMAP_OFF)         /* 15 bytes = usages 0..119   */
#define MAX_USAGE   (BITMAP_LEN * 8)
#define DEFAULT_RETRY_SEC 2

/* Standard HID keyboard usage -> evdev key code (same mapping as Linux's
 * drivers/hid/hid-input.c for usages 0x00-0x77). Named constants so the
 * kernel headers guarantee the values. */
static const unsigned char hid2ev[256] = {
    [0x04] = KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S,
    KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    [0x1E] = KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    [0x28] = KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
    KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_BACKSLASH,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
    KEY_CAPSLOCK,
    [0x3A] = KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    [0x46] = KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE, KEY_INSERT, KEY_HOME,
    KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT,
    KEY_DOWN, KEY_UP,
    [0x53] = KEY_NUMLOCK, KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS,
    KEY_KPENTER, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7,
    KEY_KP8, KEY_KP9, KEY_KP0, KEY_KPDOT, KEY_102ND, KEY_COMPOSE, KEY_POWER,
    KEY_KPEQUAL, KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
    KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_OPEN, KEY_HELP,
    KEY_PROPS, KEY_FRONT,
};

static volatile sig_atomic_t stop;
static int verbose;
static int retry_sec = DEFAULT_RETRY_SEC;

/* Bitmap of the last frame we acted on. Reset whenever we (re)connect, so a
 * key that was held while the device vanished does not stay stuck. */
static unsigned char prev[BUF_LEN];

static void on_signal(int sig) { (void)sig; stop = 1; }

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("mchose-adv: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void emit(int fd, unsigned short type, unsigned short code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(fd, &ev, sizeof ev) < 0 && errno != EAGAIN)
        logmsg("uinput write failed: %s", strerror(errno));
}

/* Read a whole (small) sysfs file. Caller frees. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = malloc(4096);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, 4095, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Locate the hidraw node for USB interface 2 of the keyboard.
 * Always matched by identity (vendor:product + interface number), never by a
 * hardcoded /dev/hidrawN, so it survives replug and cannot latch onto another
 * device. */
static int find_iface2(char *out, size_t outlen)
{
    DIR *d = opendir("/dev");
    if (!d) return -1;

    char want[64];
    snprintf(want, sizeof want, "HID_ID=0003:%08X:%08X", VENDOR_ID, PRODUCT_ID);

    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;

        char upath[PATH_MAX], rpath[PATH_MAX];
        snprintf(upath, sizeof upath, "/sys/class/hidraw/%s/device/uevent", de->d_name);
        char *ue = slurp(upath);
        if (!ue) continue;
        int match = strstr(ue, want) != NULL;
        free(ue);
        if (!match) continue;

        snprintf(upath, sizeof upath, "/sys/class/hidraw/%s/device", de->d_name);
        if (!realpath(upath, rpath)) continue;
        if (!strstr(rpath, ":1.2/")) continue;

        if (strlen(de->d_name) + 6 >= outlen) continue;
        snprintf(out, outlen, "/dev/%s", de->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static int setup_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        logmsg("cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP);
    for (int c = 1; c < 256; c++)
        ioctl(fd, UI_SET_KEYBIT, c);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor  = VENDOR_ID;
    us.id.product = PRODUCT_ID;
    snprintf(us.name, sizeof us.name, "MCHOSE advanced keys");

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        logmsg("UI_DEV_SETUP failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Release everything we believe is held. Also used on shutdown. */
static void release_all(int fd)
{
    int any = 0;
    for (int usage = 0; usage < MAX_USAGE; usage++) {
        int b = BITMAP_OFF + usage / 8;
        if ((prev[b] >> (usage % 8)) & 1) {
            unsigned char code = hid2ev[usage];
            if (code) { emit(fd, EV_KEY, code, 0); any = 1; }
        }
    }
    if (any) emit(fd, EV_SYN, SYN_REPORT, 0);
    memset(prev, 0, sizeof prev);
}

/* Read reports until the device goes away or we are asked to stop.
 * Returns 0 if we should keep retrying, 1 if the program should exit. */
static int session(int hfd, int ufd)
{
    memset(prev, 0, sizeof prev);

    while (!stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(hfd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(hfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            logmsg("select failed: %s", strerror(errno));
            return 0;
        }
        if (r == 0) continue;

        unsigned char buf[BUF_LEN];
        ssize_t n = read(hfd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            logmsg("device disappeared (%s); waiting for it to come back",
                   strerror(errno));
            return 0;
        }
        if (n < BITMAP_OFF + 1) continue;
        if (buf[0] != REPORT_ID) {
            if (verbose)
                logmsg("ignoring report id 0x%02X (len %zd)", buf[0], (ssize_t)n);
            continue;
        }
        if (n != BUF_LEN) {
            logmsg("dropping malformed report (len %zd, expected %d)",
                   n, BUF_LEN);
            continue;
        }

        int changed = 0;
        for (int usage = 0; usage < MAX_USAGE; usage++) {
            int b = BITMAP_OFF + usage / 8;
            if (b >= n) break;
            int now = (buf[b] >> (usage % 8)) & 1;
            int was = (prev[b] >> (usage % 8)) & 1;
            if (now == was) continue;

            unsigned char code = hid2ev[usage];
            if (code) {
                emit(ufd, EV_KEY, code, now);
                changed = 1;
                if (verbose)
                    logmsg("usage 0x%02X -> key %3d %s", usage, code,
                           now ? "down" : "up");
            } else if (now) {
                logmsg("unmapped HID usage 0x%02X (ignored)", usage);
            }
        }
        if (changed) emit(ufd, EV_SYN, SYN_REPORT, 0);
        memcpy(prev, buf, sizeof buf);
    }
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-v] [--retry SECONDS] [hidraw-device]\n"
        "\n"
        "Bridges MCHOSE hall-effect keyboard advanced-key output (SOCD, RS, DKS,\n"
        "MT, TGL) from USB interface 2 into real input events, because Linux does\n"
        "not translate that report on its own.\n"
        "\n"
        "  -v, --verbose     log each key change\n"
        "      --retry SEC   seconds to wait before reconnecting (default %d)\n"
        "  -h, --help        this text\n"
        "\n"
        "With no device argument, the correct hidraw node is discovered\n"
        "automatically by USB id %04X:%04X and interface number 2.\n",
        argv0, DEFAULT_RETRY_SEC, VENDOR_ID, PRODUCT_ID);
}

int main(int argc, char **argv)
{
    const char *forced = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--retry")) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            retry_sec = atoi(argv[i]);
            if (retry_sec < 1) retry_sec = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        } else {
            forced = argv[i];
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int ufd = setup_uinput();
    if (ufd < 0)
        return 1;
    logmsg("virtual keyboard created (\"MCHOSE advanced keys\")");

    while (!stop) {
        char auto_path[64];
        const char *path = forced;

        if (!path) {
            if (find_iface2(auto_path, sizeof auto_path) == 0) {
                path = auto_path;
            } else if (!stop) {
                logmsg("keyboard not found (looking for %04X:%04X interface 2); "
                       "retrying in %ds", VENDOR_ID, PRODUCT_ID, retry_sec);
                sleep(retry_sec);
                continue;
            }
        }
        if (stop) break;

        int hfd = open(path, O_RDONLY | O_NONBLOCK);
        if (hfd < 0) {
            if (!stop) {
                logmsg("cannot open %s: %s; retrying in %ds",
                       path, strerror(errno), retry_sec);
                sleep(retry_sec);
            }
            if (forced) continue;      /* explicit path: only that one */
            continue;
        }

        logmsg("connected: %s", path);
        int rc = session(hfd, ufd);
        release_all(ufd);
        close(hfd);

        if (rc == 1) break;            /* told to stop */
        if (!stop) sleep(retry_sec);
    }

    logmsg("shutting down");
    release_all(ufd);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}
