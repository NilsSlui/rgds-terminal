/* Dual-screen terminal for the Anbernic RG DS: shell on the top screen,
 * touch keyboard on the bottom screen.
 *
 * Hardware buttons:
 *   D-pad: arrow keys    A: Enter    B: Backspace    X: Tab    Y: Ctrl+C
 *   L1/R1: scroll back   L2/R2: smaller/larger text  SELECT+START: quit
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "dsterm.h"

static const int font_sizes[] = { 14, 16, 18, 20, 22, 24, 28, 32, 36 };
#define DEFAULT_FONT_INDEX 4 /* 22 px: 78x29 cells on a 1024x768 screen */
#define REPEAT_DELAY_MS 450
#define REPEAT_RATE_MS 50
#define EXIT_CONFIRM_MS 2500
#define MAX_FINGERS 10

typedef struct {
    bool active, from_finger;
    SDL_FingerID finger;
    int key;
    Button button;
    Uint32 next;
} Repeat;

typedef struct {
    char dir[PATH_MAX];
    SDL_Window *top_win, *bot_win;
    SDL_Renderer *top_ren, *bot_ren;
    Term *term;
    Keyboard kb;
    Buttons buttons;
    Haptics haptics;
    int pty;
    pid_t pid;
    char *out;
    size_t out_len, out_cap;
    int font_index;
    bool haptics_on;
    struct { SDL_FingerID id; int key; } fingers[MAX_FINGERS];
    int nfingers;
    Repeat repeat;
    Uint32 exit_armed;
    bool running;
    Uint32 started;
    bool debug_input_sent, debug_snapshot_taken;
} App;

static volatile sig_atomic_t got_signal;

static void on_signal(int sig)
{
    got_signal = sig;
}

/* ---- settings ----------------------------------------------------------- */

static void settings_path(App *a, char *buf, size_t len)
{
    snprintf(buf, len, "%s/settings.conf", a->dir);
}

static void load_settings(App *a)
{
    char path[PATH_MAX + 16], line[128];
    a->font_index = DEFAULT_FONT_INDEX;
    a->haptics_on = true;
    settings_path(a, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "font_size=%d", &v) == 1) {
            for (size_t i = 0; i < SDL_arraysize(font_sizes); i++)
                if (font_sizes[i] == v)
                    a->font_index = i;
        } else if (sscanf(line, "haptics=%d", &v) == 1) {
            a->haptics_on = v != 0;
        }
    }
    fclose(f);
}

static void save_settings(App *a)
{
    char path[PATH_MAX + 16];
    settings_path(a, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "font_size=%d\nhaptics=%d\n", font_sizes[a->font_index], a->haptics_on);
    fclose(f);
}

/* ---- shell -------------------------------------------------------------- */

static void flush_output(App *a)
{
    while (a->out_len) {
        ssize_t n = write(a->pty, a->out, a->out_len);
        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR)
                a->out_len = 0;
            return;
        }
        memmove(a->out, a->out + n, a->out_len - n);
        a->out_len -= n;
    }
}

static void app_write(const char *buf, size_t len, void *user)
{
    App *a = user;
    if (a->out_len + len > a->out_cap) {
        a->out_cap = (a->out_len + len) * 2;
        a->out = realloc(a->out, a->out_cap);
    }
    memcpy(a->out + a->out_len, buf, len);
    a->out_len += len;
    flush_output(a);
}

static struct winsize window_size(const Term *t)
{
    return (struct winsize){ .ws_row = t->rows, .ws_col = t->cols,
                             .ws_xpixel = t->cols * t->cw, .ws_ypixel = t->rows * t->ch };
}

static bool spawn_shell(App *a)
{
    struct winsize ws = window_size(a->term);
    a->pid = forkpty(&a->pty, NULL, NULL, &ws);
    if (a->pid < 0)
        return false;
    if (a->pid == 0) {
        char terminfo[PATH_MAX + 16], rcfile[PATH_MAX + 16];
        snprintf(terminfo, sizeof terminfo, "%s/terminfo", a->dir);
        snprintf(rcfile, sizeof rcfile, "%s/bashrc", a->dir);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        for (const char **v = (const char *[]){ "SDL_VIDEODRIVER", "SDL_VIDEO_DOUBLE_BUFFER",
                                                "SDL_RENDER_VSYNC", "PYSDL2_DLL_PATH", NULL };
             *v; v++)
            unsetenv(*v);
        setenv("TERM", "xterm-256color", 1);
        setenv("TERMINFO", terminfo, 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("SHELL", "/bin/bash", 1);
        const char *home = getenv("HOME");
        if (!home || !*home) {
            home = "/root";
            setenv("HOME", home, 1);
        }
        if (chdir(home) != 0 && chdir("/") != 0)
            _exit(126);
        execl("/bin/bash", "bash", "--rcfile", rcfile, "-i", (char *)NULL);
        _exit(127);
    }
    fcntl(a->pty, F_SETFL, fcntl(a->pty, F_GETFL) | O_NONBLOCK);
    fcntl(a->pty, F_SETFD, FD_CLOEXEC);
    return true;
}

static void read_shell(App *a)
{
    char buf[65536];
    size_t total = 0;
    while (total < 512 * 1024) {
        ssize_t n = read(a->pty, buf, sizeof buf);
        if (n > 0) {
            term_feed(a->term, buf, n);
            total += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        } else {
            a->running = false;  /* EOF or EIO: the shell exited */
            return;
        }
    }
}

/* ---- actions ------------------------------------------------------------ */

static void buzz(App *a, int pulse)
{
    if (a->haptics_on)
        haptics_pulse(&a->haptics, pulse);
}

static void on_bell(void *user)
{
    buzz(user, PULSE_MOD);
}

static void change_font(App *a, int step)
{
    int idx = SDL_clamp(a->font_index + step, 0, (int)SDL_arraysize(font_sizes) - 1);
    if (idx == a->font_index || !term_set_font(a->term, font_sizes[idx]))
        return;
    a->font_index = idx;
    struct winsize ws = window_size(a->term);
    ioctl(a->pty, TIOCSWINSZ, &ws);
    save_settings(a);
}

static void page(App *a, int direction)
{
    term_scroll(a->term, direction * SDL_max(1, a->term->rows - 2));
}

static void tool(App *a, const char *name)
{
    if (name[0] == '^') {  /* ^C ^D ^Z ^L */
        term_char(a->term, name[1] - 'A' + 'a', VTERM_MOD_CTRL);
    } else if (!strcmp(name, "FONT-")) {
        change_font(a, -1);
    } else if (!strcmp(name, "FONT+")) {
        change_font(a, 1);
    } else if (!strcmp(name, "PAGEUP")) {
        page(a, 1);
    } else if (!strcmp(name, "PAGEDOWN")) {
        page(a, -1);
    } else if (!strcmp(name, "HAPTIC")) {
        a->haptics_on = !a->haptics_on;
        save_settings(a);
        buzz(a, PULSE_MOD);
        a->kb.dirty = true;
    } else if (!strcmp(name, "EXIT")) {
        Uint32 now = SDL_GetTicks();
        if (a->exit_armed && now - a->exit_armed < EXIT_CONFIRM_MS) {
            a->running = false;
        } else {
            buzz(a, PULSE_WARN);
            a->exit_armed = now ? now : 1;
        }
        a->kb.dirty = true;
    }
}

static void press_key(App *a, int idx)
{
    const Key *k = &a->kb.keys[idx];
    if (k->kind == K_MOD) {
        keyboard_tap_mod(&a->kb, k->mod);
        return;
    }
    if (k->kind == K_TOOL) {
        tool(a, k->name);
        return;
    }
    KeyOutput o = keyboard_resolve(&a->kb, k);
    VTermModifier mod = keyboard_vterm_mods(&a->kb);
    if (o.ch)  /* Shift is already applied to the character itself */
        term_char(a->term, o.ch, mod & ~VTERM_MOD_SHIFT);
    else if (o.key != VTERM_KEY_NONE)
        term_key(a->term, o.key, mod);
    keyboard_release_oneshots(&a->kb);
    a->kb.dirty = true;
}

static void button_action(App *a, Button b)
{
    Term *t = a->term;
    switch (b) {
    case PAD_SELECT:
    case PAD_START:
        if (a->buttons.held[PAD_SELECT] && a->buttons.held[PAD_START])
            a->running = false;
        break;
    case PAD_A: term_key(t, VTERM_KEY_ENTER, VTERM_MOD_NONE); break;
    case PAD_B: term_key(t, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE); break;
    case PAD_X: term_key(t, VTERM_KEY_TAB, VTERM_MOD_NONE); break;
    case PAD_Y: term_char(t, 'c', VTERM_MOD_CTRL); break;
    case PAD_UP: term_key(t, VTERM_KEY_UP, VTERM_MOD_NONE); break;
    case PAD_DOWN: term_key(t, VTERM_KEY_DOWN, VTERM_MOD_NONE); break;
    case PAD_LEFT: term_key(t, VTERM_KEY_LEFT, VTERM_MOD_NONE); break;
    case PAD_RIGHT: term_key(t, VTERM_KEY_RIGHT, VTERM_MOD_NONE); break;
    case PAD_L1: page(a, 1); break;
    case PAD_R1: page(a, -1); break;
    case PAD_L2: change_font(a, -1); break;
    case PAD_R2: change_font(a, 1); break;
    default: break;
    }
}

static void on_button(Button b, bool down, void *user)
{
    App *a = user;
    if (!down) {
        if (a->repeat.active && !a->repeat.from_finger && a->repeat.button == b)
            a->repeat.active = false;
        return;
    }
    button_action(a, b);
    if (b == PAD_B || b == PAD_UP || b == PAD_DOWN || b == PAD_LEFT || b == PAD_RIGHT ||
        b == PAD_L1 || b == PAD_R1)
        a->repeat = (Repeat){ .active = true, .button = b,
                              .next = SDL_GetTicks() + REPEAT_DELAY_MS };
}

/* ---- touch and external keyboards --------------------------------------- */

static void finger_down(App *a, SDL_FingerID id, float x, float y)
{
    /* Touches on the bottom panel arrive on whichever window the compositor picked,
     * so only the normalised position is used. */
    int w, h;
    SDL_GetRendererOutputSize(a->bot_ren, &w, &h);
    int idx = keyboard_hit(&a->kb, x * w, y * h);
    if (idx < 0)
        return;
    if (a->nfingers < MAX_FINGERS) {
        a->fingers[a->nfingers].id = id;
        a->fingers[a->nfingers].key = idx;
        a->nfingers++;
    }
    a->kb.pressed[idx]++;
    a->kb.dirty = true;

    const Key *k = &a->kb.keys[idx];
    bool repeats = keyboard_repeats(&a->kb, k);
    /* Only the initial touch buzzes, not auto-repeat. Exit and the haptics
     * toggle pick their own pulse. */
    if (!(k->kind == K_TOOL && (!strcmp(k->name, "EXIT") || !strcmp(k->name, "HAPTIC"))))
        buzz(a, k->kind == K_CHAR || k->kind == K_NAMED ? PULSE_KEY : PULSE_MOD);
    press_key(a, idx);
    if (repeats)
        a->repeat = (Repeat){ .active = true, .from_finger = true, .finger = id, .key = idx,
                              .next = SDL_GetTicks() + REPEAT_DELAY_MS };
}

static void finger_up(App *a, SDL_FingerID id)
{
    for (int i = 0; i < a->nfingers; i++) {
        if (a->fingers[i].id != id)
            continue;
        a->kb.pressed[a->fingers[i].key]--;
        a->fingers[i] = a->fingers[--a->nfingers];
        a->kb.dirty = true;
        break;
    }
    if (a->repeat.active && a->repeat.from_finger && a->repeat.finger == id)
        a->repeat.active = false;
}

static uint32_t us_shift(uint32_t c)
{
    static const char plain[] = "`1234567890-=[]\\;',./";
    static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    const char *p = strchr(plain, (int)c);
    return p ? (uint32_t)shifted[p - plain] : c;
}

static void sdl_key(App *a, const SDL_KeyboardEvent *e)
{
    SDL_Keycode sym = e->keysym.sym;
    Uint16 m = e->keysym.mod;
    VTermModifier mod = VTERM_MOD_NONE;
    if (m & KMOD_SHIFT)
        mod |= VTERM_MOD_SHIFT;
    if (m & KMOD_CTRL)
        mod |= VTERM_MOD_CTRL;
    if (m & KMOD_ALT)
        mod |= VTERM_MOD_ALT;

    VTermKey key = VTERM_KEY_NONE;
    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: key = VTERM_KEY_ENTER; break;
    case SDLK_TAB: key = VTERM_KEY_TAB; break;
    case SDLK_BACKSPACE: key = VTERM_KEY_BACKSPACE; break;
    case SDLK_ESCAPE: key = VTERM_KEY_ESCAPE; break;
    case SDLK_UP: key = VTERM_KEY_UP; break;
    case SDLK_DOWN: key = VTERM_KEY_DOWN; break;
    case SDLK_LEFT: key = VTERM_KEY_LEFT; break;
    case SDLK_RIGHT: key = VTERM_KEY_RIGHT; break;
    case SDLK_HOME: key = VTERM_KEY_HOME; break;
    case SDLK_END: key = VTERM_KEY_END; break;
    case SDLK_PAGEUP: key = VTERM_KEY_PAGEUP; break;
    case SDLK_PAGEDOWN: key = VTERM_KEY_PAGEDOWN; break;
    case SDLK_INSERT: key = VTERM_KEY_INS; break;
    case SDLK_DELETE: key = VTERM_KEY_DEL; break;
    default:
        if (sym >= SDLK_F1 && sym <= SDLK_F12)
            key = VTERM_KEY_FUNCTION(sym - SDLK_F1 + 1);
    }
    if (key != VTERM_KEY_NONE) {
        term_key(a->term, key, mod);
    } else if (sym >= 32 && sym < 127) {
        uint32_t c = sym;
        bool shift = m & KMOD_SHIFT, caps = m & KMOD_CAPS;
        if (c >= 'a' && c <= 'z')
            c = (shift != caps) ? c - 32 : c;
        else if (shift)
            c = us_shift(c);
        term_char(a->term, c, mod & ~VTERM_MOD_SHIFT);
    }
}

static bool poll_sdl(App *a)
{
    SDL_Event e;
    bool force = false;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_FINGERDOWN: finger_down(a, e.tfinger.fingerId, e.tfinger.x, e.tfinger.y); break;
        case SDL_FINGERUP: finger_up(a, e.tfinger.fingerId); break;
        case SDL_KEYDOWN: sdl_key(a, &e.key); break;
        case SDL_QUIT: a->running = false; break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                force = true;
            break;
        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET: force = true; break;
        default: break;
        }
    }
    return force;
}

/* ---- debugging over SSH ------------------------------------------------- */

/* DSTERM_INPUT types text into the shell ("\r" for Enter) and DSTERM_SNAPSHOT=<dir>
 * saves both screens as BMPs DSTERM_SNAPSHOT_AT seconds (default 4) after start. */
static void debug_hooks(App *a, Uint32 now, char *top_snap, char *bot_snap, size_t len)
{
    const char *input = getenv("DSTERM_INPUT");
    if (input && !a->debug_input_sent && now - a->started > 1500) {
        a->debug_input_sent = true;
        for (const char *p = input; *p; p++) {
            if (p[0] == '\\' && p[1] == 'r') {
                app_write("\r", 1, a);
                p++;
            } else {
                app_write(p, 1, a);
            }
        }
    }
    const char *dir = getenv("DSTERM_SNAPSHOT");
    const char *at = getenv("DSTERM_SNAPSHOT_AT");
    if (dir && !a->debug_snapshot_taken && now - a->started > (at ? atof(at) : 4.0) * 1000) {
        a->debug_snapshot_taken = true;
        snprintf(top_snap, len, "%s/top.bmp", dir);
        snprintf(bot_snap, len, "%s/bottom.bmp", dir);
    }
}

/* ---- main --------------------------------------------------------------- */

static bool create_window(int display, const char *title, SDL_Window **win, SDL_Renderer **ren)
{
    *win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED_DISPLAY(display),
                            SDL_WINDOWPOS_UNDEFINED_DISPLAY(display), 0, 0,
                            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
    if (!*win) {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    *ren = SDL_CreateRenderer(*win, -1, SDL_RENDERER_ACCELERATED);
    if (!*ren) {
        SDL_Log("SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }
    return true;
}

static bool lcd_swapped(void)
{
    const char *env = getenv("DSTERM_SWAP");
    if (env)
        return strcmp(env, "1") == 0;
    FILE *f = fopen("/sys/class/anbernic_misc/lcdswap", "r");
    int c = f ? fgetc(f) : 0;
    if (f)
        fclose(f);
    return c == '1';
}

static bool app_init(App *a)
{
    ssize_t n = readlink("/proc/self/exe", a->dir, sizeof a->dir - 1);
    if (n <= 0)
        return false;
    a->dir[n] = 0;
    *strrchr(a->dir, '/') = 0;
    load_settings(a);

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, "0", SDL_HINT_OVERRIDE);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init: %s", TTF_GetError());
        return false;
    }
    if (SDL_GetNumVideoDisplays() < 2) {
        SDL_Log("this app needs two screens");
        return false;
    }
    int top = lcd_swapped() ? 1 : 0;
    if (!create_window(top, "Terminal", &a->top_win, &a->top_ren) ||
        !create_window(!top, "Keyboard", &a->bot_win, &a->bot_ren))
        return false;
    SDL_PumpEvents();

    a->term = term_new(a->top_ren, font_sizes[a->font_index], app_write, a);
    if (!a->term || !keyboard_init(&a->kb, a->bot_ren))
        return false;
    a->term->on_bell = on_bell;
    buttons_open(&a->buttons);
    haptics_init(&a->haptics, a->buttons.fd);
    if (!spawn_shell(a)) {
        SDL_Log("forkpty: %s", strerror(errno));
        return false;
    }
    a->running = true;
    a->started = SDL_GetTicks();
    return true;
}

static void app_run(App *a)
{
    bool force = true;
    while (a->running && !got_signal) {
        struct pollfd fds[2] = {
            { .fd = a->pty, .events = POLLIN | (a->out_len ? POLLOUT : 0) },
            { .fd = a->buttons.fd, .events = POLLIN },
        };
        int nfds = a->buttons.fd >= 0 ? 2 : 1;
        if (poll(fds, nfds, 10) < 0 && errno != EINTR)
            break;
        if (fds[0].revents & POLLOUT)
            flush_output(a);
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
            read_shell(a);
        if (nfds > 1 && (fds[1].revents & POLLIN))
            buttons_read(&a->buttons, on_button, a);
        force = poll_sdl(a) || force;

        Uint32 now = SDL_GetTicks();
        char top_snap[PATH_MAX] = "", bot_snap[PATH_MAX] = "";
        debug_hooks(a, now, top_snap, bot_snap, sizeof top_snap);
        if (a->repeat.active && SDL_TICKS_PASSED(now, a->repeat.next)) {
            if (a->repeat.from_finger)
                press_key(a, a->repeat.key);
            else
                button_action(a, a->repeat.button);
            a->repeat.next = now + REPEAT_RATE_MS;
        }
        if (a->exit_armed && now - a->exit_armed >= EXIT_CONFIRM_MS) {
            a->exit_armed = 0;
            a->kb.dirty = true;
        }
        if (a->term->dirty || force || top_snap[0])
            term_draw(a->term, top_snap[0] ? top_snap : NULL);
        if (a->kb.dirty || force || bot_snap[0])
            keyboard_draw(&a->kb, a->exit_armed != 0, a->haptics_on, bot_snap[0] ? bot_snap : NULL);
        force = false;
    }
}

static void app_close(App *a)
{
    if (a->pid > 0) {
        kill(-a->pid, SIGHUP);
        waitpid(a->pid, NULL, WNOHANG);
    }
    haptics_close(&a->haptics);
    buttons_close(&a->buttons);
    keyboard_free(&a->kb);
    term_free(a->term);
    if (a->top_ren)
        SDL_DestroyRenderer(a->top_ren);
    if (a->bot_ren)
        SDL_DestroyRenderer(a->bot_ren);
    if (a->top_win)
        SDL_DestroyWindow(a->top_win);
    if (a->bot_win)
        SDL_DestroyWindow(a->bot_win);
    TTF_Quit();
    SDL_Quit();
    free(a->out);
}

int main(void)
{
    static App app = { .pty = -1, .buttons = { .fd = -1 }, .haptics = { .fd = -1, .ids = { -1, -1, -1 } } };
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    bool ok = app_init(&app);
    if (ok)
        app_run(&app);
    app_close(&app);
    return ok ? 0 : 1;
}
