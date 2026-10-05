#include "gfx.h"

#include <math.h>
#include <string.h>

void gfx_init(gfx *g, uint32_t *px, int w, int h, int stride) {
    g->px = px;
    g->w = w;
    g->h = h;
    g->stride = stride;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t gfx_alpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | ((uint32_t)clampi(a, 0, 255) << 24); }

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    int i;
    uint32_t out = 0xFF000000u;
    for (i = 0; i < 24; i += 8) {
        int ca = (int)((a >> i) & 255), cb = (int)((b >> i) & 255);
        out |= (uint32_t)(ca + (cb - ca) * t / 256) << i;
    }
    return out;
}

/* Blends colour c at coverage cov (0..255) into one pixel. */
static void blend(uint32_t *p, uint32_t c, int cov) {
    int a = (int)(c >> 24) * cov / 255;
    uint32_t d;
    int i;
    if (a <= 0) return;
    if (a >= 255) {
        *p = c | 0xFF000000u;
        return;
    }
    d = *p;
    for (i = 0; i < 24; i += 8) {
        int sc = (int)((c >> i) & 255), dc = (int)((d >> i) & 255);
        d = (d & ~(255u << i)) | ((uint32_t)((sc * a + dc * (255 - a) + 127) / 255) << i);
    }
    *p = d | 0xFF000000u;
}

void gfx_clear(gfx *g, uint32_t c) {
    int x, y;
    for (y = 0; y < g->h; y++)
        for (x = 0; x < g->w; x++) g->px[y * g->stride + x] = c | 0xFF000000u;
}

void gfx_vgradient(gfx *g, uint32_t top, uint32_t bottom) {
    int x, y;
    for (y = 0; y < g->h; y++) {
        uint32_t c = gfx_mix(top, bottom, y * 256 / (g->h > 1 ? g->h - 1 : 1));
        for (x = 0; x < g->w; x++) g->px[y * g->stride + x] = c;
    }
}

void gfx_rect(gfx *g, int x, int y, int w, int h, uint32_t c) {
    int x0 = clampi(x, 0, g->w), x1 = clampi(x + w, 0, g->w), y0 = clampi(y, 0, g->h), y1 = clampi(y + h, 0, g->h), i, j;
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++) blend(&g->px[j * g->stride + i], c, 255);
}

/* Coverage (0..255) of pixel (px,py) by a rounded rectangle, using the distance
 * to the corner circle for a one-pixel antialiased edge. */
static int rr_cover(int px, int py, int x, int y, int w, int h, int r) {
    float fx = (float)px + 0.5f, fy = (float)py + 0.5f, cx, cy, d;
    if (fx < x || fx > x + w || fy < y || fy > y + h) return 0;
    cx = fx < x + r ? x + r : fx > x + w - r ? x + w - r : fx;
    cy = fy < y + r ? y + r : fy > y + h - r ? y + h - r : fy;
    d = sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
    if (d <= r - 0.5f) return 255;
    if (d >= r + 0.5f) return 0;
    return (int)((r + 0.5f - d) * 255.0f);
}

void gfx_rrect(gfx *g, int x, int y, int w, int h, int r, uint32_t c) {
    int i, j, x0 = clampi(x, 0, g->w), x1 = clampi(x + w, 0, g->w), y0 = clampi(y, 0, g->h), y1 = clampi(y + h, 0, g->h);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++) {
            int cov;
            /* Only the corners need the distance test. */
            if ((i >= x + r && i < x + w - r) || (j >= y + r && j < y + h - r)) cov = 255;
            else cov = rr_cover(i, j, x, y, w, h, r);
            if (cov) blend(&g->px[j * g->stride + i], c, cov);
        }
}

void gfx_rrect_outline(gfx *g, int x, int y, int w, int h, int r, int thick, uint32_t c) {
    int i, j, x0 = clampi(x, 0, g->w), x1 = clampi(x + w, 0, g->w), y0 = clampi(y, 0, g->h), y1 = clampi(y + h, 0, g->h);
    int ri = r - thick;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    ri = r - thick;
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++) {
            int cov;
            int in_corner = (i < x + r || i >= x + w - r) && (j < y + r || j >= y + h - r);
            if (!in_corner) {
                /* Straight edges and the interior: no distance needed. */
                cov = (i < x + thick || i >= x + w - thick || j < y + thick || j >= y + h - thick) ? 255 : 0;
            } else {
                int outer = rr_cover(i, j, x, y, w, h, r);
                int inner = (w - 2 * thick > 0 && h - 2 * thick > 0)
                                ? rr_cover(i, j, x + thick, y + thick, w - 2 * thick, h - 2 * thick, ri > 0 ? ri : 0)
                                : 0;
                cov = outer - inner;
            }
            if (cov > 0) blend(&g->px[j * g->stride + i], c, cov);
        }
}

void gfx_circle(gfx *g, int cx, int cy, int r, uint32_t c) { gfx_rrect(g, cx - r, cy - r, 2 * r, 2 * r, r, c); }

void gfx_line(gfx *g, float x1, float y1, float x2, float y2, float thick, uint32_t c) {
    float r = thick / 2, dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
    int i, j, x0 = (int)(fminf(x1, x2) - r - 1), x3 = (int)(fmaxf(x1, x2) + r + 2), y0 = (int)(fminf(y1, y2) - r - 1),
           y3 = (int)(fmaxf(y1, y2) + r + 2);
    for (j = y0; j < y3; j++)
        for (i = x0; i < x3; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f, t = len2 > 0 ? ((px - x1) * dx + (py - y1) * dy) / len2 : 0, d;
            float qx, qy;
            if (i < 0 || j < 0 || i >= g->w || j >= g->h) continue;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            qx = x1 + t * dx;
            qy = y1 + t * dy;
            d = sqrtf((px - qx) * (px - qx) + (py - qy) * (py - qy));
            if (d < r + 0.5f) blend(&g->px[j * g->stride + i], c, d <= r - 0.5f ? 255 : (int)((r + 0.5f - d) * 255.0f));
        }
}

void gfx_tri(gfx *g, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c) {
    int i, j, a, b, x0 = (int)fminf(x1, fminf(x2, x3)) - 1, x4 = (int)fmaxf(x1, fmaxf(x2, x3)) + 2,
              y0 = (int)fminf(y1, fminf(y2, y3)) - 1, y4 = (int)fmaxf(y1, fmaxf(y2, y3)) + 2;
    float d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
    if (d == 0) return;
    for (j = y0; j < y4; j++)
        for (i = x0; i < x4; i++) {
            int hit = 0;
            if (i < 0 || j < 0 || i >= g->w || j >= g->h) continue;
            for (b = 0; b < 4; b++)
                for (a = 0; a < 4; a++) {
                    float px = (float)i + (a + 0.5f) / 4, py = (float)j + (b + 0.5f) / 4;
                    float l1 = ((y2 - y3) * (px - x3) + (x3 - x2) * (py - y3)) / d;
                    float l2 = ((y3 - y1) * (px - x3) + (x1 - x3) * (py - y3)) / d;
                    if (l1 >= 0 && l2 >= 0 && 1 - l1 - l2 >= 0) hit++;
                }
            if (hit) blend(&g->px[j * g->stride + i], c, hit * 255 / 16);
        }
}

void gfx_arc(gfx *g, int cx, int cy, int r, int thick, int from_deg, int to_deg, uint32_t c) {
    int i, j;
    for (j = cy - r - 1; j <= cy + r + 1; j++)
        for (i = cx - r - 1; i <= cx + r + 1; i++) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy, d = sqrtf(dx * dx + dy * dy), a, cov;
            int deg;
            if (i < 0 || j < 0 || i >= g->w || j >= g->h) continue;
            if (d > r + 0.5f || d < r - thick - 0.5f) continue;
            a = atan2f(dy, dx) * 57.29578f;
            deg = (int)(a < 0 ? a + 360 : a);
            {
                int f = ((from_deg % 360) + 360) % 360, t = ((to_deg % 360) + 360) % 360;
                int in = f <= t ? (deg >= f && deg <= t) : (deg >= f || deg <= t);
                if (!in) continue;
            }
            cov = 255.0f;
            if (d > r - 0.5f) cov = (r + 0.5f - d) * 255.0f;
            else if (d < r - thick + 0.5f) cov = (d - (r - thick - 0.5f)) * 255.0f;
            if (cov > 0) blend(&g->px[j * g->stride + i], c, clampi((int)cov, 0, 255));
        }
}

/* ------------------------------------------------------------------ text */

/* The glyph for the next character, advancing *s past it. Non-ASCII bytes
 * (whole UTF-8 sequences) become one '?', and runs of them collapse. */
static const gfx_glyph *next_glyph(const gfx_font *f, const char **s, int *prev_unknown) {
    unsigned char c = (unsigned char)**s;
    if (c >= 32 && c <= 126) {
        (*s)++;
        *prev_unknown = 0;
        return &f->glyphs[c - 32];
    }
    if (c >= 0x80) {
        while ((unsigned char)**s >= 0x80) (*s)++;
        if (*prev_unknown) return NULL;
        *prev_unknown = 1;
        return &f->glyphs['?' - 32];
    }
    (*s)++; /* control character: skipped */
    return NULL;
}

static void draw_glyph(gfx *g, const gfx_font *f, const gfx_glyph *gl, int x, int y, uint32_t c) {
    int i, j, gx = x + gl->bx, gy = y + f->ascent - gl->by;
    const uint8_t *b = f->bits + gl->off;
    for (j = 0; j < gl->h; j++) {
        int py = gy + j;
        if (py < 0 || py >= g->h) continue;
        for (i = 0; i < gl->w; i++) {
            int px = gx + i, cov = b[j * gl->w + i];
            if (px < 0 || px >= g->w || !cov) continue;
            blend(&g->px[py * g->stride + px], c, cov);
        }
    }
}

int gfx_text(gfx *g, const gfx_font *f, int x, int y, const char *s, uint32_t c) {
    int pen16 = x * 16, unk = 0;
    while (*s) {
        const gfx_glyph *gl = next_glyph(f, &s, &unk);
        if (!gl) continue;
        if (g) draw_glyph(g, f, gl, (pen16 + 8) / 16, y, c);
        pen16 += gl->adv16;
    }
    return (pen16 + 8) / 16 - x;
}

int gfx_text_width(const gfx_font *f, const char *s) { return gfx_text(NULL, f, 0, 0, s, 0); }

int gfx_text_fit(gfx *g, const gfx_font *f, int x, int y, int max_w, const char *s, uint32_t c) {
    char buf[512];
    size_t n = 0;
    int dots;
    if (gfx_text_width(f, s) <= max_w) return gfx_text(g, f, x, y, s, c);
    dots = gfx_text_width(f, "...");
    {
        /* Keep characters while they still fit beside the ellipsis. */
        const char *p = s;
        int unk = 0, pen16 = 0;
        while (*p && n < sizeof buf - 8) {
            const char *start = p;
            const gfx_glyph *gl = next_glyph(f, &p, &unk);
            size_t len = (size_t)(p - start);
            if (!gl) continue;
            if ((pen16 + gl->adv16 + 8) / 16 + dots > max_w || n + len >= sizeof buf - 5) break;
            memcpy(buf + n, start, len);
            n += len;
            pen16 += gl->adv16;
        }
    }
    memcpy(buf + n, "...", 4);
    return gfx_text(g, f, x, y, buf, c);
}

static int prefix_w(const gfx_font *f, const char *p, size_t len) {
    char b[420];
    if (len >= sizeof b) len = sizeof b - 1;
    memcpy(b, p, len);
    b[len] = '\0';
    return gfx_text_width(f, b);
}

int gfx_text_wrap(gfx *g, const gfx_font *f, int x, int y, int max_w, int line_gap, const char *s, uint32_t c,
                  int maxlines) {
    return gfx_text_wrap_ex(g, f, x, y, max_w, line_gap, s, c, maxlines, 0);
}

int gfx_text_wrap_ex(gfx *g, const gfx_font *f, int x, int y, int max_w, int line_gap, const char *s, uint32_t c,
                     int maxlines, int align) {
    int lines = 0;
    const char *p = s;
    while (*p) {
        size_t n = 0; /* bytes of this line, from p */
        char line[420];
        for (;;) {
            const char *w = p + n, *e;
            size_t cand;
            while (*w == ' ') w++;
            e = w;
            while (*e && *e != ' ' && *e != '\n') e++;
            if (e == w) break; /* end of text, or a newline */
            cand = (size_t)(e - p);
            if (prefix_w(f, p, cand) <= max_w) {
                n = cand;
                continue;
            }
            if (n == 0) { /* one word wider than the line: cut it where it stops fitting */
                size_t k = 1, base = (size_t)(w - p);
                while (w + k <= e && prefix_w(f, p, base + k) <= max_w) k++;
                n = base + k - 1;
                if (n <= base) n = base + 1;
            }
            break;
        }
        if (n == 0) {
            if (*p == '\n') { /* a blank line */
                p++;
                lines++;
                continue;
            }
            break;
        }
        if (maxlines > 0 && lines + 1 == maxlines && p[n] != '\0' && p[n + strspn(p + n, " ")] != '\0') {
            /* The last line allowed and there is more: end it with an ellipsis. */
            if (g) gfx_text_fit(g, f, x, y + lines * (f->line + line_gap), max_w, p, c);
            return lines + 1;
        }
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if (g) {
            int lx = align == 1 ? x + (max_w - gfx_text_width(f, line)) / 2 : x;
            gfx_text(g, f, lx, y + lines * (f->line + line_gap), line, c);
        }
        lines++;
        p += n;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
        if (maxlines > 0 && lines >= maxlines) break;
    }
    return lines;
}
