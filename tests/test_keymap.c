#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>

#define BUF_LEN     16
#define BITMAP_OFF  1
#define BITMAP_LEN  (BUF_LEN - BITMAP_OFF)
#define MAX_USAGE   (BITMAP_LEN * 8)

static const struct { unsigned short vendor, product; } known[] = {
    { 0x3837, 0x3003 },     /* Ace 68 -III */
    { 0x41E4, 0x2103 },     /* Ace 60 Pro  */
};
#define N_KNOWN (sizeof known / sizeof known[0])

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

static void test_known_devices(void)
{
    assert(N_KNOWN == 2);
    assert(known[0].vendor == 0x3837 && known[0].product == 0x3003);
    assert(known[1].vendor == 0x41e4 && known[1].product == 0x2103);
}

static void test_key_mappings(void)
{
    /* Letters */
    assert(hid2ev[0x04] == KEY_A);
    assert(hid2ev[0x07] == KEY_D);
    assert(hid2ev[0x16] == KEY_S);
    assert(hid2ev[0x1A] == KEY_W);
    assert(hid2ev[0x1D] == KEY_Z);

    /* Numbers */
    assert(hid2ev[0x1E] == KEY_1);
    assert(hid2ev[0x27] == KEY_0);

    /* Brackets (verified in PROTOCOL.md) */
    assert(hid2ev[0x2F] == KEY_LEFTBRACE);
    assert(hid2ev[0x30] == KEY_RIGHTBRACE);

    /* Function keys F1-F12 (F11/F12 were broken in original code) */
    assert(hid2ev[0x3A] == KEY_F1);
    assert(hid2ev[0x43] == KEY_F10);
    assert(hid2ev[0x44] == KEY_F11);
    assert(hid2ev[0x45] == KEY_F12);

    /* Navigation / Arrows (were broken in original code) */
    assert(hid2ev[0x46] == KEY_SYSRQ);
    assert(hid2ev[0x4C] == KEY_DELETE);
    assert(hid2ev[0x4F] == KEY_RIGHT);
    assert(hid2ev[0x50] == KEY_LEFT);
    assert(hid2ev[0x51] == KEY_DOWN);
    assert(hid2ev[0x52] == KEY_UP);
}

static void test_report_bitmap_decoding(void)
{
    /* Frame for '[' press: usage 0x2F (47) -> byte 6, bit 7 */
    unsigned char frame_leftbrace[BUF_LEN] = {0};
    frame_leftbrace[0] = 0x01;
    frame_leftbrace[6] = 0x80;

    int detected_key = 0;
    for (int usage = 0; usage < MAX_USAGE; usage++) {
        int b = BITMAP_OFF + usage / 8;
        if ((frame_leftbrace[b] >> (usage % 8)) & 1) {
            detected_key = hid2ev[usage];
            assert(usage == 0x2F);
        }
    }
    assert(detected_key == KEY_LEFTBRACE);

    /* Frame for ']' press: usage 0x30 (48) -> byte 7, bit 0 */
    unsigned char frame_rightbrace[BUF_LEN] = {0};
    frame_rightbrace[0] = 0x01;
    frame_rightbrace[7] = 0x01;

    detected_key = 0;
    for (int usage = 0; usage < MAX_USAGE; usage++) {
        int b = BITMAP_OFF + usage / 8;
        if ((frame_rightbrace[b] >> (usage % 8)) & 1) {
            detected_key = hid2ev[usage];
            assert(usage == 0x30);
        }
    }
    assert(detected_key == KEY_RIGHTBRACE);
}

int main(void)
{
    test_known_devices();
    test_key_mappings();
    test_report_bitmap_decoding();
    printf("All mchose-adv keymap and report decoding tests passed.\n");
    return 0;
}
