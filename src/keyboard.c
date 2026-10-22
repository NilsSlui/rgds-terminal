/* On-screen keyboard: layout, modifier state and drawing of the bottom screen. */
#include <string.h>

#include <SDL2_gfxPrimitives.h>

#include "dsterm.h"

#define UI_FONT "/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define UI_BOLD_FONT "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
#define TOOLBAR_HEIGHT 72
#define PAD 6
#define ROW_UNITS 15.0f
#define DOUBLE_TAP_MS 450

/* Each row is 15 units wide. */
#define C(n, s) { K_CHAR, n, s, 1, { 0 }, 0 }
#define CW(n, s, w) { K_CHAR, n, s, w, { 0 }, 0 }
#define N(n, w) { K_NAMED, n, NULL, w, { 0 }, 0 }
#define M(n, m, w) { K_MOD, n, NULL, w, { 0 }, m }
#define T(n, w) { K_TOOL, n, NULL, w, { 0 }, 0 }
#define GAP(w) { K_GAP, NULL, NULL, w, { 0 }, 0 }
#define END { K_END, NULL, NULL, 0, { 0 }, 0 }

static const Key toolbar[] = {
    T("^C", 1.25), T("^D", 1.25), T("^Z", 1.25), T("^L", 1.25), GAP(0.5),
    T("FONT-", 1.25), T("FONT+", 1.25), GAP(0.5), T("PAGEUP", 1.25), T("PAGEDOWN", 1.25),
    GAP(0.25), T("HAPTIC", 1.5), GAP(0.25), T("EXIT", 2), END,
};
static const Key row0[] = {
    C("`", "~"), C("1", "!"), C("2", "@"), C("3", "#"), C("4", "$"), C("5", "%"), C("6", "^"),
    C("7", "&"), C("8", "*"), C("9", "("), C("0", ")"), C("-", "_"), C("=", "+"), N("BKSP", 2), END,
};
static const Key row1[] = {
    N("TAB", 1.5), C("q", "Q"), C("w", "W"), C("e", "E"), C("r", "R"), C("t", "T"), C("y", "Y"),
    C("u", "U"), C("i", "I"), C("o", "O"), C("p", "P"), C("[", "{"), C("]", "}"),
    CW("\\", "|", 1.5), END,
};
static const Key row2[] = {
    M("CTRL", MOD_CTRL, 1.75), C("a", "A"), C("s", "S"), C("d", "D"), C("f", "F"), C("g", "G"),
    C("h", "H"), C("j", "J"), C("k", "K"), C("l", "L"), C(";", ":"), C("'", "\""),
    N("ENTER", 2.25), END,
};
static const Key row3[] = {
    M("SHIFT", MOD_SHIFT, 2), C("z", "Z"), C("x", "X"), C("c", "C"), C("v", "V"), C("b", "B"),
    C("n", "N"), C("m", "M"), C(",", "<"), C(".", ">"), C("/", "?"), C("|", "|"), N("UP", 1),
    N("DEL", 1), END,
};
static const Key row4[] = {
    N("ESC", 1.5), M("FN", MOD_FN, 1.5), M("ALT", MOD_ALT, 1.5), N("SPACE", 7.5), N("LEFT", 1),
    N("DOWN", 1), N("RIGHT", 1), END,
};
static const Key *const rows[] = { row0, row1, row2, row3, row4 };

/* Named keys: identifier, vterm key, label, and what they become with Fn held. */
typedef struct {
    const char *name;
    VTermKey key;
    const char *label;
    const char *fn;
} Named;

static const Named named[] = {
    { "BKSP", VTERM_KEY_BACKSPACE, "⌫", "DEL" },
    { "TAB", VTERM_KEY_TAB, "Tab", NULL },
    { "ENTER", VTERM_KEY_ENTER, "Enter", NULL },
    { "ESC", VTERM_KEY_ESCAPE, "Esc", NULL },
    { "SPACE", VTERM_KEY_NONE, "", NULL },
    { "UP", VTERM_KEY_UP, "↑", "PGUP" },
    { "DOWN", VTERM_KEY_DOWN, "↓", "PGDN" },
    { "LEFT", VTERM_KEY_LEFT, "←", "HOME" },
    { "RIGHT", VTERM_KEY_RIGHT, "→", "END" },
    { "DEL", VTERM_KEY_DEL, "Del", NULL },
    { "PGUP", VTERM_KEY_PAGEUP, "PgUp", NULL },
    { "PGDN", VTERM_KEY_PAGEDOWN, "PgDn", NULL },
    { "HOME", VTERM_KEY_HOME, "Home", NULL },
    { "END", VTERM_KEY_END, "End", NULL },
};

static const char *const fkey_labels[13] = {
    "", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
};

static const struct { const char *name, *label; } tool_labels[] = {
    { "FONT-", "A−" }, { "FONT+", "A+" }, { "PAGEUP", "▲" }, { "PAGEDOWN", "▼" },
    { "HAPTIC", "Buzz" }, { "EXIT", "Exit" },
    { "SHIFT", "⇧ Shift" }, { "CTRL", "Ctrl" }, { "ALT", "Alt" }, { "FN", "Fn" },
};

static const RGB KB_BG = { 12, 14, 18 };
static const RGB KEY_BG = { 46, 52, 62 };
static const RGB KEY_DARK = { 30, 34, 41 };
static const RGB KEY_TOOL = { 36, 44, 58 };
static const RGB KEY_PRESSED = { 74, 144, 226 };
static const RGB MOD_ONESHOT_BG = { 52, 110, 170 };
static const RGB MOD_LOCKED_BG = { 226, 160, 58 };
static const RGB EXIT_ARMED_BG = { 190, 50, 50 };
static const RGB LABEL = { 235, 238, 242 };
static const RGB LABEL_DIM = { 150, 158, 170 };

static const Named *find_named(const char *name)
{
    for (size_t i = 0; i < SDL_arraysize(named); i++)
        if (strcmp(named[i].name, name) == 0)
            return &named[i];
    return NULL;
}

static const char *tool_label(const char *name)
{
    for (size_t i = 0; i < SDL_arraysize(tool_labels); i++)
        if (strcmp(tool_labels[i].name, name) == 0)
            return tool_labels[i].label;
    return name;
}

/* F1-F12 on the number row while Fn is active; 0 if the key has none. */
static int fn_number(const char *ch)
{
    if (ch[0] >= '1' && ch[0] <= '9')
        return ch[0] - '0';
    switch (ch[0]) {
    case '0': return 10;
    case '-': return 11;
    case '=': return 12;
    default: return 0;
    }
}

static void layout_row(Keyboard *kb, const Key *specs, float y, float h, float unit)
{
    float x = PAD / 2.0f;
    for (const Key *s = specs; s->kind != K_END; s++) {
        if (s->kind != K_GAP && kb->nkeys < MAX_KEYS) {
            Key *k = &kb->keys[kb->nkeys++];
            *k = *s;
            k->rect = (SDL_Rect){ (int)(x + PAD / 2.0f), (int)(y + PAD / 2.0f),
                                  (int)(s->width * unit - PAD), (int)(h - PAD) };
        }
        x += s->width * unit;
    }
}

bool keyboard_init(Keyboard *kb, SDL_Renderer *ren)
{
    memset(kb, 0, sizeof *kb);
    kb->ren = ren;
    kb->text = text_cache_new(ren);
    kb->font = TTF_OpenFont(UI_FONT, 30);
    kb->font_small = TTF_OpenFont(UI_FONT, 18);
    kb->font_bold = TTF_OpenFont(UI_BOLD_FONT, 24);
    if (!kb->font || !kb->font_small || !kb->font_bold) {
        SDL_Log("cannot open keyboard fonts: %s", TTF_GetError());
        return false;
    }
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    float unit = (w - PAD) / ROW_UNITS;
    float row_h = (float)(h - TOOLBAR_HEIGHT) / SDL_arraysize(rows);
    layout_row(kb, toolbar, 0, TOOLBAR_HEIGHT, unit);
    for (size_t i = 0; i < SDL_arraysize(rows); i++)
        layout_row(kb, rows[i], TOOLBAR_HEIGHT + i * row_h, row_h, unit);
    kb->dirty = true;
    return true;
}

void keyboard_free(Keyboard *kb)
{
    text_cache_free(kb->text);
    if (kb->font)
        TTF_CloseFont(kb->font);
    if (kb->font_small)
        TTF_CloseFont(kb->font_small);
    if (kb->font_bold)
        TTF_CloseFont(kb->font_bold);
}

int keyboard_hit(const Keyboard *kb, int x, int y)
{
    for (int i = 0; i < kb->nkeys; i++) {
        const SDL_Rect *r = &kb->keys[i].rect;
        if (x >= r->x - 3 && x < r->x + r->w + 3 && y >= r->y - 3 && y < r->y + r->h + 3)
            return i;
    }
    return -1;
}

/* Tap: off -> one-shot; a quick second tap locks; another tap releases. */
void keyboard_tap_mod(Keyboard *kb, int mod)
{
    Uint32 now = SDL_GetTicks();
    switch (kb->mods[mod]) {
    case MOD_OFF:
        kb->mods[mod] = MOD_ONESHOT;
        break;
    case MOD_ONESHOT:
        kb->mods[mod] = now - kb->last_tap[mod] < DOUBLE_TAP_MS ? MOD_LOCKED : MOD_OFF;
        break;
    default:
        kb->mods[mod] = MOD_OFF;
    }
    kb->last_tap[mod] = now;
    kb->dirty = true;
}

void keyboard_release_oneshots(Keyboard *kb)
{
    for (int m = 0; m < MOD_COUNT; m++) {
        if (kb->mods[m] == MOD_ONESHOT) {
            kb->mods[m] = MOD_OFF;
            kb->dirty = true;
        }
    }
}

VTermModifier keyboard_vterm_mods(const Keyboard *kb)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (kb->mods[MOD_SHIFT])
        mod |= VTERM_MOD_SHIFT;
    if (kb->mods[MOD_CTRL])
        mod |= VTERM_MOD_CTRL;
    if (kb->mods[MOD_ALT])
        mod |= VTERM_MOD_ALT;
    return mod;
}

KeyOutput keyboard_resolve(const Keyboard *kb, const Key *k)
{
    KeyOutput out = { 0, VTERM_KEY_NONE, "" };
    bool fn = kb->mods[MOD_FN] != MOD_OFF;
    if (k->kind == K_CHAR) {
        int f = fn ? fn_number(k->name) : 0;
        if (f) {
            out.key = VTERM_KEY_FUNCTION(f);
            out.label = fkey_labels[f];
        } else {
            out.label = kb->mods[MOD_SHIFT] ? k->shifted : k->name;
            out.ch = (unsigned char)out.label[0];
        }
    } else if (k->kind == K_NAMED) {
        const Named *n = find_named(k->name);
        if (fn && n->fn)
            n = find_named(n->fn);
        out.key = n->key;
        out.label = n->label;
        if (strcmp(n->name, "SPACE") == 0)
            out.ch = ' ';
    } else {
        out.label = tool_label(k->name);
    }
    return out;
}

bool keyboard_repeats(const Keyboard *kb, const Key *k)
{
    if (k->kind == K_CHAR)
        return true;
    if (k->kind == K_TOOL)
        return strcmp(k->name, "PAGEUP") == 0 || strcmp(k->name, "PAGEDOWN") == 0;
    if (k->kind != K_NAMED)
        return false;
    KeyOutput o = keyboard_resolve(kb, k);
    return o.ch == ' ' || (o.key >= VTERM_KEY_UP && o.key <= VTERM_KEY_RIGHT) ||
           (o.key >= VTERM_KEY_INS && o.key <= VTERM_KEY_PAGEDOWN) ||
           o.key == VTERM_KEY_BACKSPACE;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_box(Keyboard *kb, const SDL_Rect *r, RGB c)
{
    roundedBoxRGBA(kb->ren, r->x, r->y, r->x + r->w - 1, r->y + r->h - 1, 10, c.r, c.g, c.b, 255);
}

static bool is_arrow_label(const char *s)
{
    return !strcmp(s, "↑") || !strcmp(s, "↓") || !strcmp(s, "←") || !strcmp(s, "→") ||
           !strcmp(s, "⌫");
}

void keyboard_draw(Keyboard *kb, bool exit_armed, bool haptics_on, const char *snapshot)
{
    SDL_Renderer *r = kb->ren;
    SDL_SetRenderDrawColor(r, KB_BG.r, KB_BG.g, KB_BG.b, 255);
    SDL_RenderClear(r);

    for (int i = 0; i < kb->nkeys; i++) {
        const Key *k = &kb->keys[i];
        bool is_exit = k->kind == K_TOOL && !strcmp(k->name, "EXIT");
        bool is_haptic = k->kind == K_TOOL && !strcmp(k->name, "HAPTIC");
        RGB bg = k->kind == K_CHAR ? KEY_BG : k->kind == K_TOOL ? KEY_TOOL : KEY_DARK;
        if (kb->pressed[i])
            bg = KEY_PRESSED;
        else if (k->kind == K_MOD && kb->mods[k->mod] == MOD_LOCKED)
            bg = MOD_LOCKED_BG;
        else if (k->kind == K_MOD && kb->mods[k->mod] == MOD_ONESHOT)
            bg = MOD_ONESHOT_BG;
        else if (is_exit && exit_armed)
            bg = EXIT_ARMED_BG;
        else if (is_haptic && haptics_on)
            bg = MOD_ONESHOT_BG;
        draw_box(kb, &k->rect, bg);

        int cx = k->rect.x + k->rect.w / 2, cy = k->rect.y + k->rect.h / 2;
        KeyOutput o = keyboard_resolve(kb, k);
        if (k->kind == K_TOOL || k->kind == K_MOD) {
            const char *label = is_exit && exit_armed ? "Sure?" : o.label;
            RGB fg = is_haptic && !haptics_on ? LABEL_DIM : LABEL;
            text_draw_centered(kb->text, kb->font_bold, label, cx, cy, fg);
            continue;
        }
        bool single = o.ch && o.label[1] == 0;
        if (k->kind == K_CHAR && single && strcmp(k->name, k->shifted) != 0 &&
            !(k->name[0] >= 'a' && k->name[0] <= 'z')) {
            /* Symbol keys show the other layer small in the corner. */
            const char *other = kb->mods[MOD_SHIFT] ? k->name : k->shifted;
            text_draw_centered(kb->text, kb->font_small, other, k->rect.x + 16, k->rect.y + 16,
                               LABEL_DIM);
        }
        TTF_Font *font = single || is_arrow_label(o.label) ? kb->font : kb->font_bold;
        text_draw_centered(kb->text, font, o.label, cx, cy, LABEL);
    }
    if (snapshot)
        save_frame(r, snapshot);
    SDL_RenderPresent(r);
    kb->dirty = false;
}
