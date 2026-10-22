/* Text rendering helpers: a hash map from (font, string) to a white texture
 * that is tinted with SDL_SetTextureColorMod when drawn. */
#include <stdlib.h>
#include <string.h>

#include "dsterm.h"

typedef struct {
    TTF_Font *font;
    char *key;
    SDL_Texture *tex;
    int w, h;
} TextEntry;

struct TextCache {
    SDL_Renderer *ren;
    TextEntry *entries;
    size_t cap, count;
};

void fill_rect(SDL_Renderer *ren, RGB c, int x, int y, int w, int h)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(ren, &(SDL_Rect){ x, y, w, h });
}

/* Debug helper: write the current back buffer to a BMP file. */
void save_frame(SDL_Renderer *ren, const char *path)
{
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return;
    SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

static uint64_t hash_key(const TTF_Font *font, const char *s)
{
    uint64_t h = 1469598103934665603ULL ^ (uintptr_t)font;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static TextEntry *find_slot(TextEntry *entries, size_t cap, const TTF_Font *font, const char *s)
{
    size_t i = hash_key(font, s) & (cap - 1);
    while (entries[i].key && !(entries[i].font == font && strcmp(entries[i].key, s) == 0))
        i = (i + 1) & (cap - 1);
    return &entries[i];
}

TextCache *text_cache_new(SDL_Renderer *ren)
{
    TextCache *c = calloc(1, sizeof *c);
    c->ren = ren;
    c->cap = 1024;
    c->entries = calloc(c->cap, sizeof *c->entries);
    return c;
}

static void grow(TextCache *c)
{
    size_t ncap = c->cap * 2;
    TextEntry *n = calloc(ncap, sizeof *n);
    for (size_t i = 0; i < c->cap; i++)
        if (c->entries[i].key)
            *find_slot(n, ncap, c->entries[i].font, c->entries[i].key) = c->entries[i];
    free(c->entries);
    c->entries = n;
    c->cap = ncap;
}

void text_cache_clear(TextCache *c)
{
    for (size_t i = 0; i < c->cap; i++) {
        if (c->entries[i].tex)
            SDL_DestroyTexture(c->entries[i].tex);
        free(c->entries[i].key);
    }
    memset(c->entries, 0, c->cap * sizeof *c->entries);
    c->count = 0;
}

void text_cache_free(TextCache *c)
{
    if (!c)
        return;
    text_cache_clear(c);
    free(c->entries);
    free(c);
}

bool text_get(TextCache *c, TTF_Font *font, const char *s, SDL_Texture **tex, int *w, int *h)
{
    if (!font || !*s)
        return false;
    TextEntry *e = find_slot(c->entries, c->cap, font, s);
    if (!e->key) {
        if ((c->count + 1) * 2 > c->cap) {
            grow(c);
            e = find_slot(c->entries, c->cap, font, s);
        }
        e->key = strdup(s);
        e->font = font;
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, (SDL_Color){ 255, 255, 255, 255 });
        if (surf) {
            e->tex = SDL_CreateTextureFromSurface(c->ren, surf);
            e->w = surf->w;
            e->h = surf->h;
            SDL_FreeSurface(surf);
        }
        c->count++;
    }
    if (!e->tex)
        return false;
    *tex = e->tex;
    *w = e->w;
    *h = e->h;
    return true;
}

void text_draw(TextCache *c, TTF_Font *font, const char *s, int x, int y, RGB color)
{
    SDL_Texture *tex;
    int w, h;
    if (!text_get(c, font, s, &tex, &w, &h))
        return;
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_RenderCopy(c->ren, tex, NULL, &(SDL_Rect){ x, y, w, h });
}

void text_draw_centered(TextCache *c, TTF_Font *font, const char *s, int cx, int cy, RGB color)
{
    SDL_Texture *tex;
    int w, h;
    if (!text_get(c, font, s, &tex, &w, &h))
        return;
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_RenderCopy(c->ren, tex, NULL, &(SDL_Rect){ cx - w / 2, cy - h / 2, w, h });
}
