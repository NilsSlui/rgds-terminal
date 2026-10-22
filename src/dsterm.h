/* Dual-screen terminal for the Anbernic RG DS: shell on the top screen,
 * touch keyboard on the bottom screen. */
#ifndef DSTERM_H
#define DSTERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <vterm.h>

typedef struct { uint8_t r, g, b; } RGB;

void fill_rect(SDL_Renderer *ren, RGB c, int x, int y, int w, int h);
void save_frame(SDL_Renderer *ren, const char *path);

/* ---- text.c: cache of white text textures, tinted at draw time ---------- */

typedef struct TextCache TextCache;

TextCache *text_cache_new(SDL_Renderer *ren);
void text_cache_clear(TextCache *c);
void text_cache_free(TextCache *c);
bool text_get(TextCache *c, TTF_Font *font, const char *utf8, SDL_Texture **tex, int *w, int *h);
void text_draw(TextCache *c, TTF_Font *font, const char *utf8, int x, int y, RGB color);
void text_draw_centered(TextCache *c, TTF_Font *font, const char *utf8, int cx, int cy, RGB color);
int utf8_encode(uint32_t cp, char *out);

/* ---- term.c: libvterm screen, scrollback and top-screen drawing -------- */

#define SCROLLBACK_LINES 2000

typedef struct {
    int cols;
    VTermScreenCell *cells;
} SbLine;

typedef void (*TermWriteFn)(const char *buf, size_t len, void *user);

typedef struct Term {
    VTerm *vt;
    VTermScreen *screen;
    VTermState *state;
    VTermScreenCell blank;
    int rows, cols;
    VTermPos cursor;
    bool cursor_visible;
    bool altscreen;
    bool dirty;

    SbLine sb[SCROLLBACK_LINES];
    int sb_start, sb_count;
    int scroll_offset;

    TermWriteFn write;
    void (*on_bell)(void *user);
    void *user;

    SDL_Renderer *ren;
    TextCache *text;
    TTF_Font *fonts[4];   /* regular, bold, italic, bold italic */
    TTF_Font *fallback;   /* CJK */
    int width, height, font_size, cw, ch, ox, oy;
} Term;

Term *term_new(SDL_Renderer *ren, int font_size, TermWriteFn write, void *user);
void term_free(Term *t);
bool term_set_font(Term *t, int size);
void term_feed(Term *t, const char *buf, size_t len);
void term_key(Term *t, VTermKey key, VTermModifier mod);
void term_char(Term *t, uint32_t ch, VTermModifier mod);
void term_scroll(Term *t, int delta);
void term_draw(Term *t, const char *snapshot);

/* ---- keyboard.c: on-screen keyboard ------------------------------------ */

typedef enum { K_END, K_GAP, K_CHAR, K_NAMED, K_MOD, K_TOOL } KeyKind;
enum { MOD_SHIFT, MOD_CTRL, MOD_ALT, MOD_FN, MOD_COUNT };
enum { MOD_OFF, MOD_ONESHOT, MOD_LOCKED };

typedef struct {
    KeyKind kind;
    const char *name;     /* char keys: the unshifted character */
    const char *shifted;  /* char keys: the shifted character */
    float width;          /* in units; each row is 15 units */
    SDL_Rect rect;
    int mod;              /* MOD_* for modifier keys */
} Key;

/* What a key press produces right now: a character or a special key. */
typedef struct {
    uint32_t ch;          /* nonzero for characters */
    VTermKey key;
    const char *label;
} KeyOutput;

#define MAX_KEYS 96

typedef struct {
    Key keys[MAX_KEYS];
    int nkeys;
    int mods[MOD_COUNT];
    Uint32 last_tap[MOD_COUNT];
    int pressed[MAX_KEYS];  /* number of fingers on each key */
    bool dirty;

    SDL_Renderer *ren;
    TextCache *text;
    TTF_Font *font, *font_small, *font_bold;
} Keyboard;

bool keyboard_init(Keyboard *kb, SDL_Renderer *ren);
void keyboard_free(Keyboard *kb);
int keyboard_hit(const Keyboard *kb, int x, int y);
void keyboard_tap_mod(Keyboard *kb, int mod);
KeyOutput keyboard_resolve(const Keyboard *kb, const Key *k);
VTermModifier keyboard_vterm_mods(const Keyboard *kb);
void keyboard_release_oneshots(Keyboard *kb);
bool keyboard_repeats(const Keyboard *kb, const Key *k);
void keyboard_draw(Keyboard *kb, bool exit_armed, bool haptics_on, const char *snapshot);

/* ---- device.c: built-in buttons and rumble motor ----------------------- */

typedef enum {
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_L1, PAD_R1, PAD_L2, PAD_R2,
    PAD_SELECT, PAD_START, PAD_MENU, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_COUNT
} Button;

typedef struct {
    int fd;
    bool held[PAD_COUNT];
} Buttons;

typedef void (*ButtonFn)(Button b, bool down, void *user);

void buttons_open(Buttons *b);
void buttons_read(Buttons *b, ButtonFn fn, void *user);
void buttons_close(Buttons *b);

enum { PULSE_KEY, PULSE_MOD, PULSE_WARN, PULSE_COUNT };

typedef struct {
    int fd;
    int ids[PULSE_COUNT];
} Haptics;

void haptics_init(Haptics *h, int fd);
void haptics_pulse(Haptics *h, int pulse);
void haptics_close(Haptics *h);

#endif
