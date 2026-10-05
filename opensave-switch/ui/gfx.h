/* A tiny software renderer: a 32-bit pixel buffer, antialiased rounded shapes
 * and text. Portable on purpose — the console draws into the screen's
 * framebuffer with it, and the PC tests draw into memory and write pictures, so
 * the interface can be looked at without a console.
 *
 * Pixels are 0xAABBGGRR, which is RGBA8888 in memory on a little-endian CPU:
 * what libnx's framebuffer expects and what the PC test writes out. */
#ifndef OPENSAVE_GFX_H
#define OPENSAVE_GFX_H

#include <stddef.h>
#include <stdint.h>

#define GFX_RGBA(r, g, b, a) \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))
#define GFX_RGB(r, g, b) GFX_RGBA(r, g, b, 255)

typedef struct {
    uint32_t *px;
    int w, h;
    int stride; /* in pixels */
} gfx;

typedef struct {
    uint32_t off; /* into the font's bitmap blob */
    uint8_t w, h;
    int8_t bx;  /* pixels right of the pen to the glyph's left edge */
    int8_t by;  /* pixels above the baseline to the glyph's top edge */
    uint16_t adv16; /* advance, in 1/16 pixel */
} gfx_glyph;

typedef struct {
    int px;     /* nominal size */
    int ascent; /* baseline's distance below the line's top */
    int line;   /* line height */
    const gfx_glyph *glyphs; /* printable ASCII, 32..126 */
    const uint8_t *bits;     /* 8-bit coverage */
} gfx_font;

extern const gfx_font font_small, font_body, font_strong, font_title;

void gfx_init(gfx *g, uint32_t *px, int w, int h, int stride);
void gfx_clear(gfx *g, uint32_t c);
/* Vertical blend from top to bottom colour over the whole buffer. */
void gfx_vgradient(gfx *g, uint32_t top, uint32_t bottom);
/* Alpha-blends c (its own alpha) over a rectangle. */
void gfx_rect(gfx *g, int x, int y, int w, int h, uint32_t c);
void gfx_rrect(gfx *g, int x, int y, int w, int h, int r, uint32_t c);
/* A ring: the rounded rectangle's outline, thick pixels wide. */
void gfx_rrect_outline(gfx *g, int x, int y, int w, int h, int r, int thick, uint32_t c);
void gfx_circle(gfx *g, int cx, int cy, int r, uint32_t c);
/* A line with round ends, thick pixels wide. */
void gfx_line(gfx *g, float x1, float y1, float x2, float y2, float thick, uint32_t c);
/* A filled triangle (antialiased by supersampling). */
void gfx_tri(gfx *g, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);
/* An arc of a ring (angles in degrees, 0 = 3 o'clock, clockwise), for spinners. */
void gfx_arc(gfx *g, int cx, int cy, int r, int thick, int from_deg, int to_deg, uint32_t c);

/* Text. y is the top of the line. Anything but printable ASCII is drawn as
 * '?', with runs of it collapsed to one so a title in another script reads as
 * "?" rather than a row of them. All return the width drawn. */
int gfx_text(gfx *g, const gfx_font *f, int x, int y, const char *s, uint32_t c);
int gfx_text_width(const gfx_font *f, const char *s);
/* Draws at most max_w pixels, ending in "..." if it had to cut. */
int gfx_text_fit(gfx *g, const gfx_font *f, int x, int y, int max_w, const char *s, uint32_t c);
/* Word-wraps into max_w pixels (align 0 = left, 1 = centred on x + max_w/2); returns the number of lines drawn. maxlines<=0
 * means unlimited. With g == NULL it only counts. */
int gfx_text_wrap(gfx *g, const gfx_font *f, int x, int y, int max_w, int line_gap, const char *s, uint32_t c,
                  int maxlines);
int gfx_text_wrap_ex(gfx *g, const gfx_font *f, int x, int y, int max_w, int line_gap, const char *s, uint32_t c,
                     int maxlines, int align);

uint32_t gfx_mix(uint32_t a, uint32_t b, int t256); /* a + (b-a)*t */
uint32_t gfx_alpha(uint32_t c, int a);              /* c with alpha a (0..255) */

#endif
