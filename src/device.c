/* Built-in controls, read straight from evdev like the stock apps do, and the
 * rumble motor, driven through the same device's force-feedback interface. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#include "dsterm.h"

/* evdev key codes for the face/shoulder buttons (same mapping as the stock apps) */
static const struct { int code; Button button; } key_map[] = {
    { 304, PAD_A },  { 305, PAD_B },  { 306, PAD_Y },      { 307, PAD_X },
    { 308, PAD_L1 }, { 309, PAD_R1 }, { 314, PAD_L2 },     { 315, PAD_R2 },
    { 310, PAD_SELECT }, { 311, PAD_START }, { 312, PAD_MENU },
};

/* Rumble pulse lengths in milliseconds; below ~25 ms the motor barely spins up. */
static const int pulse_ms[PULSE_COUNT] = { [PULSE_KEY] = 30, [PULSE_MOD] = 45, [PULSE_WARN] = 140 };

static int open_gamepad(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;
    int fd = -1;
    struct dirent *de;
    while (fd < 0 && (de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[300], name[256] = "";
        snprintf(path, sizeof path, "/sys/class/input/%s/device/name", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(name, sizeof name, f))
            name[0] = 0;
        fclose(f);
        if (!strstr(name, "ANBERNIC"))
            continue;
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        /* Read-write so the same device can drive the rumble motor. */
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    closedir(dir);
    return fd;
}

void buttons_open(Buttons *b)
{
    memset(b, 0, sizeof *b);
    b->fd = open_gamepad();
}

static void set_held(Buttons *b, Button btn, bool down, ButtonFn fn, void *user)
{
    if (b->held[btn] == down)
        return;
    b->held[btn] = down;
    fn(btn, down, user);
}

void buttons_read(Buttons *b, ButtonFn fn, void *user)
{
    struct input_event ev[64];
    ssize_t n;
    while (b->fd >= 0 && (n = read(b->fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < n / sizeof *ev; i++) {
            if (ev[i].type == EV_KEY && (ev[i].value == 0 || ev[i].value == 1)) {
                for (size_t k = 0; k < SDL_arraysize(key_map); k++)
                    if (key_map[k].code == ev[i].code)
                        set_held(b, key_map[k].button, ev[i].value, fn, user);
            } else if (ev[i].type == EV_ABS && ev[i].code == ABS_HAT0X) {
                set_held(b, PAD_LEFT, ev[i].value < 0, fn, user);
                set_held(b, PAD_RIGHT, ev[i].value > 0, fn, user);
            } else if (ev[i].type == EV_ABS && ev[i].code == ABS_HAT0Y) {
                set_held(b, PAD_UP, ev[i].value < 0, fn, user);
                set_held(b, PAD_DOWN, ev[i].value > 0, fn, user);
            }
        }
    }
}

void buttons_close(Buttons *b)
{
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
}

/* Each pulse is uploaded once as an FF_RUMBLE effect with a fixed length, so the
 * kernel times the buzz and playing it is a single non-blocking write. */
void haptics_init(Haptics *h, int fd)
{
    h->fd = fd;
    for (int i = 0; i < PULSE_COUNT; i++) {
        struct ff_effect e = {
            .type = FF_RUMBLE,
            .id = -1,
            .replay.length = pulse_ms[i],
            .u.rumble = { .strong_magnitude = 0xFFFF, .weak_magnitude = 0xFFFF },
        };
        h->ids[i] = fd >= 0 && ioctl(fd, EVIOCSFF, &e) == 0 ? e.id : -1;
    }
}

static void play(Haptics *h, int id, int value)
{
    struct input_event ev = { .type = EV_FF, .code = id, .value = value };
    if (write(h->fd, &ev, sizeof ev) < 0 && errno != EAGAIN)
        SDL_Log("rumble: %s", strerror(errno));
}

void haptics_pulse(Haptics *h, int pulse)
{
    if (h->ids[pulse] >= 0)
        play(h, h->ids[pulse], 1);
}

void haptics_close(Haptics *h)
{
    for (int i = 0; i < PULSE_COUNT; i++) {
        if (h->ids[i] >= 0) {
            play(h, h->ids[i], 0);
            ioctl(h->fd, EVIOCRMFF, h->ids[i]);
            h->ids[i] = -1;
        }
    }
}
