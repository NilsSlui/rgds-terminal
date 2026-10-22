/* Terminal emulation (libvterm), scrollback and drawing of the top screen.
 *
 * The whole grid is redrawn straight to the window whenever something changed.
 * Render-target textures are avoided on purpose: on this device using them in
 * one window corrupts the other window's rendering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsterm.h"

#define FONT_DIR "/usr/share/fonts/dejavu/"
#define FALLBACK_FONT "/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Regular.otf"

static const char *mono_fonts[4] = {
    FONT_DIR "DejaVuSansMono.ttf", FONT_DIR "DejaVuSansMono-Bold.ttf",
    FONT_DIR "DejaVuSansMono-Oblique.ttf", FONT_DIR "DejaVuSansMono-BoldOblique.ttf",
};

static const RGB FG = { 229, 229, 229 };
static const RGB BG = { 16, 18, 22 };
static const RGB CURSOR = { 120, 200, 255 };
static const RGB BADGE = { 200, 140, 40 };

static const RGB ANSI16[16] = {
    { 0, 0, 0 },       { 205, 49, 49 },  { 13, 188, 121 }, { 229, 229, 16 },
    { 36, 114, 200 },  { 188, 63, 188 }, { 17, 168, 205 }, { 229, 229, 229 },
    { 102, 102, 102 }, { 241, 76, 76 },  { 35, 209, 139 }, { 245, 245, 67 },
    { 59, 142, 234 },  { 214, 112, 214 }, { 41, 184, 219 }, { 255, 255, 255 },
};

static bool rgb_eq(RGB a, RGB b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* ---- libvterm callbacks ------------------------------------------------- */

static void output_cb(const char *s, size_t len, void *user)
{
    Term *t = user;
    t->write(s, len, t->user);
}

static int cb_damage(VTermRect rect, void *user)
{
    (void)rect;
    ((Term *)user)->dirty = true;
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    (void)dest;
    (void)src;
    ((Term *)user)->dirty = true;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void)oldpos;
    (void)visible;
    Term *t = user;
    t->cursor = pos;
    t->dirty = true;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Term *t = user;
    if (prop == VTERM_PROP_CURSORVISIBLE)
        t->cursor_visible = val->boolean;
    else if (prop == VTERM_PROP_ALTSCREEN) {
        t->altscreen = val->boolean;
        t->scroll_offset = 0;
    }
    t->dirty = true;
    return 1;
}

static int cb_bell(void *user)
{
    Term *t = user;
    if (t->on_bell)
        t->on_bell(t->user);
    return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    Term *t = user;
    if (t->sb_count == SCROLLBACK_LINES) {  /* drop the oldest line */
        free(t->sb[t->sb_start].cells);
        t->sb_start = (t->sb_start + 1) % SCROLLBACK_LINES;
        t->sb_count--;
    }
    SbLine *line = &t->sb[(t->sb_start + t->sb_count) % SCROLLBACK_LINES];
    line->cols = cols;
    line->cells = malloc(cols * sizeof *cells);
    memcpy(line->cells, cells, cols * sizeof *cells);
    t->sb_count++;
    if (t->scroll_offset && t->scroll_offset < t->sb_count)
        t->scroll_offset++;  /* keep the scrolled-back view still */
    return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    Term *t = user;
    if (!t->sb_count)
        return 0;
    SbLine *line = &t->sb[(t->sb_start + t->sb_count - 1) % SCROLLBACK_LINES];
    for (int i = 0; i < cols; i++)
        cells[i] = i < line->cols ? line->cells[i] : t->blank;
    free(line->cells);
    line->cells = NULL;
    t->sb_count--;
    if (t->scroll_offset > t->sb_count)
        t->scroll_offset = t->sb_count;
    return 1;
}

static int cb_sb_clear(void *user)
{
    Term *t = user;
    for (int i = 0; i < t->sb_count; i++) {
        SbLine *line = &t->sb[(t->sb_start + i) % SCROLLBACK_LINES];
        free(line->cells);
        line->cells = NULL;
    }
    t->sb_start = t->sb_count = t->scroll_offset = 0;
    t->dirty = true;
    return 1;
}

static const VTermScreenCallbacks callbacks = {
    .damage = cb_damage,
    .moverect = cb_moverect,
    .movecursor = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell = cb_bell,
    .sb_pushline = cb_sb_pushline,
    .sb_popline = cb_sb_popline,
    .sb_clear = cb_sb_clear,
};

/* ---- setup -------------------------------------------------------------- */

bool term_set_font(Term *t, int size)
{
    TTF_Font *fonts[4];
    for (int i = 0; i < 4; i++) {
        fonts[i] = TTF_OpenFont(mono_fonts[i], size);
        if (!fonts[i]) {
            SDL_Log("cannot open %s: %s", mono_fonts[i], TTF_GetError());
            while (i--)
                TTF_CloseFont(fonts[i]);
            return false;
        }
    }
    text_cache_clear(t->text);  /* cached textures are keyed by font */
    for (int i = 0; i < 4; i++) {
        if (t->fonts[i])
            TTF_CloseFont(t->fonts[i]);
        t->fonts[i] = fonts[i];
    }
    if (t->fallback)
        TTF_CloseFont(t->fallback);
    t->fallback = TTF_OpenFont(FALLBACK_FONT, size);

    int minx, maxx, miny, maxy, advance;
    TTF_GlyphMetrics(t->fonts[0], 'M', &minx, &maxx, &miny, &maxy, &advance);
    t->font_size = size;
    t->cw = advance;
    t->ch = TTF_FontLineSkip(t->fonts[0]);
    t->cols = SDL_max(20, t->width / t->cw);
    t->rows = SDL_max(5, t->height / t->ch);
    t->ox = (t->width - t->cols * t->cw) / 2;
    t->oy = (t->height - t->rows * t->ch) / 2;
    if (t->vt) {
        vterm_set_size(t->vt, t->rows, t->cols);
        vterm_screen_flush_damage(t->screen);
    }
    t->scroll_offset = 0;
    t->dirty = true;
    return true;
}

Term *term_new(SDL_Renderer *ren, int font_size, TermWriteFn write, void *user)
{
    Term *t = calloc(1, sizeof *t);
    t->ren = ren;
    t->write = write;
    t->user = user;
    t->text = text_cache_new(ren);
    t->cursor_visible = true;
    SDL_GetRendererOutputSize(ren, &t->width, &t->height);
    if (!term_set_font(t, font_size)) {
        term_free(t);
        return NULL;
    }

    t->vt = vterm_new(t->rows, t->cols);
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, output_cb, t);
    t->state = vterm_obtain_state(t->vt);
    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &callbacks, t);
    vterm_screen_set_damage_merge(t->screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_enable_reflow(t->screen, true);
    vterm_screen_reset(t->screen, 1);

    VTermColor fg, bg;
    vterm_color_rgb(&fg, FG.r, FG.g, FG.b);
    vterm_color_rgb(&bg, BG.r, BG.g, BG.b);
    vterm_screen_set_default_colors(t->screen, &fg, &bg);
    for (int i = 0; i < 16; i++) {
        VTermColor c;
        vterm_color_rgb(&c, ANSI16[i].r, ANSI16[i].g, ANSI16[i].b);
        vterm_state_set_palette_color(t->state, i, &c);
    }
    vterm_state_set_bold_highbright(t->state, 1);

    /* A blank cell in default colours, used to pad scrollback lines. */
    memset(&t->blank, 0, sizeof t->blank);
    t->blank.width = 1;
    vterm_state_get_default_colors(t->state, &t->blank.fg, &t->blank.bg);
    return t;
}

void term_free(Term *t)
{
    if (!t)
        return;
    cb_sb_clear(t);
    if (t->vt)
        vterm_free(t->vt);
    text_cache_free(t->text);
    for (int i = 0; i < 4; i++)
        if (t->fonts[i])
            TTF_CloseFont(t->fonts[i]);
    if (t->fallback)
        TTF_CloseFont(t->fallback);
    free(t);
}

/* ---- input -------------------------------------------------------------- */

void term_feed(Term *t, const char *buf, size_t len)
{
    vterm_input_write(t->vt, buf, len);
    vterm_screen_flush_damage(t->screen);
}

static void snap_to_bottom(Term *t)
{
    if (t->scroll_offset) {
        t->scroll_offset = 0;
        t->dirty = true;
    }
}

void term_key(Term *t, VTermKey key, VTermModifier mod)
{
    snap_to_bottom(t);
    vterm_keyboard_key(t->vt, key, mod);
}

void term_char(Term *t, uint32_t ch, VTermModifier mod)
{
    snap_to_bottom(t);
    vterm_keyboard_unichar(t->vt, ch, mod);
}

void term_scroll(Term *t, int delta)
{
    if (t->altscreen)
        return;
    int off = SDL_clamp(t->scroll_offset + delta, 0, t->sb_count);
    if (off != t->scroll_offset) {
        t->scroll_offset = off;
        t->dirty = true;
    }
}

/* ---- drawing ------------------------------------------------------------ */

/* The cell at a visible position, taking the scrollback view into account. */
static bool get_cell(Term *t, int row, int col, VTermScreenCell *out)
{
    if (t->scroll_offset) {
        int idx = t->sb_count - t->scroll_offset + row;
        if (idx < t->sb_count) {
            const SbLine *line = &t->sb[(t->sb_start + idx) % SCROLLBACK_LINES];
            *out = col < line->cols ? line->cells[col] : t->blank;
            return true;
        }
        row = idx - t->sb_count;
    }
    return vterm_screen_get_cell(t->screen, (VTermPos){ .row = row, .col = col }, out);
}

static RGB to_rgb(Term *t, VTermColor c)
{
    vterm_screen_convert_color_to_rgb(t->screen, &c);
    return (RGB){ c.rgb.red, c.rgb.green, c.rgb.blue };
}

static TTF_Font *font_for(Term *t, const VTermScreenCell *cell)
{
    TTF_Font *f = t->fonts[(cell->attrs.bold ? 1 : 0) | (cell->attrs.italic ? 2 : 0)];
    uint32_t cp = cell->chars[0];
    if (cp > 0x7F && cp <= 0xFFFF && t->fallback && !TTF_GlyphIsProvided(f, cp) &&
        TTF_GlyphIsProvided(t->fallback, cp))
        return t->fallback;
    return f;
}

static void draw_glyph(Term *t, const VTermScreenCell *cell, int px, int py, RGB fg)
{
    uint32_t first = cell->chars[0];
    if (first == 0 || first == ' ' || first == (uint32_t)-1)
        return;
    char buf[VTERM_MAX_CHARS_PER_CELL * 4 + 1];
    int n = 0;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++)
        n += utf8_encode(cell->chars[i], buf + n);
    buf[n] = 0;

    SDL_Texture *tex;
    int w, h;
    if (!text_get(t->text, font_for(t, cell), buf, &tex, &w, &h))
        return;
    int cells = cell->width > 1 ? cell->width : 1;
    int gx = (w > t->cw || cells > 1) ? px + (cells * t->cw - w) / 2 : px;
    SDL_SetTextureColorMod(tex, fg.r, fg.g, fg.b);
    SDL_RenderCopy(t->ren, tex, NULL, &(SDL_Rect){ gx, py + (t->ch - h) / 2, w, h });
}

static void draw_badge(Term *t)
{
    char buf[48];
    SDL_Texture *tex;
    int w, h;
    snprintf(buf, sizeof buf, "  scrollback -%d  ", t->scroll_offset);
    if (!text_get(t->text, t->fonts[0], buf, &tex, &w, &h))
        return;
    int x = t->width - w - 8;
    fill_rect(t->ren, BADGE, x, 4, w, h);
    SDL_SetTextureColorMod(tex, 0, 0, 0);
    SDL_RenderCopy(t->ren, tex, NULL, &(SDL_Rect){ x, 4, w, h });
}

void term_draw(Term *t, const char *snapshot)
{
    SDL_Renderer *r = t->ren;
    SDL_SetRenderDrawColor(r, BG.r, BG.g, BG.b, 255);
    SDL_RenderClear(r);

    VTermScreenCell cell;
    for (int row = 0; row < t->rows; row++) {
        int py = t->oy + row * t->ch;
        for (int col = 0; col < t->cols;) {
            if (!get_cell(t, row, col, &cell)) {
                col++;
                continue;
            }
            int cells = cell.width > 1 ? cell.width : 1;
            int px = t->ox + col * t->cw;
            RGB fg = to_rgb(t, cell.fg), bg = to_rgb(t, cell.bg);
            if (cell.attrs.reverse) {
                RGB tmp = fg;
                fg = bg;
                bg = tmp;
            }
            if (cell.attrs.conceal)
                fg = bg;
            if (!rgb_eq(bg, BG))
                fill_rect(r, bg, px, py, cells * t->cw, t->ch);
            draw_glyph(t, &cell, px, py, fg);
            if (cell.attrs.underline)
                fill_rect(r, fg, px, py + t->ch - 2, cells * t->cw, 1);
            if (cell.attrs.strike)
                fill_rect(r, fg, px, py + t->ch / 2, cells * t->cw, 1);
            col += cells;
        }
    }

    if (t->cursor_visible && !t->scroll_offset &&
        vterm_screen_get_cell(t->screen, t->cursor, &cell)) {
        int px = t->ox + t->cursor.col * t->cw, py = t->oy + t->cursor.row * t->ch;
        fill_rect(r, CURSOR, px, py, (cell.width > 1 ? cell.width : 1) * t->cw, t->ch);
        draw_glyph(t, &cell, px, py, BG);
    }
    if (t->scroll_offset)
        draw_badge(t);
    if (snapshot)
        save_frame(r, snapshot);
    SDL_RenderPresent(r);
    t->dirty = false;
}
