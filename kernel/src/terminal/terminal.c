#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "terminal.h"

// Scale an 8-bit colour channel value to the size the framebuffer gives the
// channel and move it into place within a pixel.
static uint32_t fb_channel(uint8_t value, uint8_t mask_size, uint8_t mask_shift) {
    uint64_t max = ((uint64_t)1 << mask_size) - 1;
    return (uint32_t)((value * max / 255) << mask_shift);
}

// Build a pixel from 8-bit red, green and blue values following the channel
// layout of the framebuffer.
static uint32_t fb_pixel(struct limine_framebuffer *fb, uint8_t red, uint8_t green, uint8_t blue) {
    return fb_channel(red, fb->red_mask_size, fb->red_mask_shift)
         | fb_channel(green, fb->green_mask_size, fb->green_mask_shift)
         | fb_channel(blue, fb->blue_mask_size, fb->blue_mask_shift);
}

// A small built-in 5x7 font. It needs no filesystem or external font files.
static void glyph_rows(char ch, uint8_t rows[7]) {
    for (size_t i = 0; i < 7; ++i) rows[i] = 0;
    switch (ch) {
        case '0': { static const uint8_t p[7] = {0b01110, 0b10011, 0b10101, 0b10101, 0b11001, 0b10001, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case '1': { static const uint8_t p[7] = {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'A': { static const uint8_t p[7] = {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'B': { static const uint8_t p[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'C': { static const uint8_t p[7] = {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'D': { static const uint8_t p[7] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'E': { static const uint8_t p[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'G': { static const uint8_t p[7] = {0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'I': { static const uint8_t p[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'K': { static const uint8_t p[7] = {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'L': { static const uint8_t p[7] = {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'M': { static const uint8_t p[7] = {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'N': { static const uint8_t p[7] = {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'O': { static const uint8_t p[7] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'R': { static const uint8_t p[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'S': { static const uint8_t p[7] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'T': { static const uint8_t p[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'U': { static const uint8_t p[7] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'V': { static const uint8_t p[7] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case '.': { static const uint8_t p[7] = {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'F': { static const uint8_t p[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'H': { static const uint8_t p[7] = {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'P': { static const uint8_t p[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case 'Y': { static const uint8_t p[7] = {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case '>': { static const uint8_t p[7] = {0b10000, 0b01000, 0b00100, 0b00010, 0b00100, 0b01000, 0b10000}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case '[': { static const uint8_t p[7] = {0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case ']': { static const uint8_t p[7] = {0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case '-': { static const uint8_t p[7] = {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        case ':': { static const uint8_t p[7] = {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}; for (size_t i = 0; i < 7; ++i) rows[i] = p[i]; break; }
        default: break; // Spaces and unsupported symbols are blank.
    }
}

static void put_pixel(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
    volatile uint32_t *pixels = (volatile uint32_t *)fb->address;
    pixels[y * (fb->pitch / 4) + x] = color;
}

static void fill_screen(struct limine_framebuffer *fb, uint32_t color) {
    for (uint64_t y = 0; y < fb->height; ++y)
        for (uint64_t x = 0; x < fb->width; ++x)
            put_pixel(fb, x, y, color);
}

static void draw_text(struct limine_framebuffer *fb, const char *text,
                      uint64_t x, uint64_t y, uint64_t scale, uint32_t color) {
    for (; *text; ++text, x += 6 * scale) {
        uint8_t rows[7];
        glyph_rows(*text, rows);
        for (uint64_t row = 0; row < 7; ++row)
            for (uint64_t col = 0; col < 5; ++col)
                if (rows[row] & (1u << (4 - col)))
                    for (uint64_t sy = 0; sy < scale; ++sy)
                        for (uint64_t sx = 0; sx < scale; ++sx)
                            put_pixel(fb, x + col * scale + sx,
                                      y + row * scale + sy, color);
    }
}


static struct limine_framebuffer *screen;
static uint64_t cursor_x, cursor_y;
static uint32_t ink, background, accent, muted;
enum { MARGIN = 24, SCALE = 2, ADVANCE = 12, LINE_HEIGHT = 20 };

static void scroll_if_needed(void) {
    if (!screen || cursor_y + 7 * SCALE + MARGIN <= screen->height) return;
    volatile uint32_t *pixels = (volatile uint32_t *)screen->address;
    uint64_t stride = screen->pitch / 4;
    uint64_t bottom = screen->height > MARGIN ? screen->height - MARGIN : 0;
    for (uint64_t y = MARGIN; y + LINE_HEIGHT < bottom; ++y)
        for (uint64_t x = 0; x < screen->width; ++x)
            pixels[y * stride + x] = pixels[(y + LINE_HEIGHT) * stride + x];
    for (uint64_t y = bottom > LINE_HEIGHT ? bottom - LINE_HEIGHT : 0; y < screen->height; ++y)
        for (uint64_t x = 0; x < screen->width; ++x)
            pixels[y * stride + x] = background;
    cursor_y = bottom > LINE_HEIGHT ? bottom - LINE_HEIGHT : MARGIN;
}

void terminal_init(struct limine_framebuffer *fb) {
    screen = fb;
    background = fb_pixel(fb, 10, 13, 18);
    ink = fb_pixel(fb, 220, 226, 234);
    accent = fb_pixel(fb, 105, 230, 192);
    muted = fb_pixel(fb, 144, 156, 169);
    terminal_clear();
}

void terminal_clear(void) {
    if (!screen) return;
    fill_screen(screen, background);
    cursor_x = MARGIN;
    cursor_y = MARGIN;
}

void terminal_newline(void) {
    if (!screen) return;
    cursor_x = MARGIN;
    cursor_y += LINE_HEIGHT;
    scroll_if_needed();
}

void terminal_putc_color(char ch, enum terminal_color color) {
    if (!screen) return;
    if (ch == '\n') { terminal_newline(); return; }
    if (ch == '\r') { cursor_x = MARGIN; return; }
    if (ch == '\t') { for (int i = 0; i < 4; ++i) terminal_putc_color(' ', color); return; }
    if (ch < 32 || ch > 126) ch = '?';
    if (cursor_x + 5 * SCALE + MARGIN > screen->width) terminal_newline();
    uint32_t selected = color == TERMINAL_ACCENT ? accent :
                        color == TERMINAL_MUTED ? muted : ink;
    char display = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    draw_text(screen, (char[]){display, '\0'}, cursor_x, cursor_y, SCALE, selected);
    cursor_x += ADVANCE;
}

void terminal_write_color(const char *s, enum terminal_color color) {
    if (!s) return;
    while (*s) terminal_putc_color(*s++, color);
}
void terminal_write(const char *s) { terminal_write_color(s, TERMINAL_NORMAL); }

void terminal_backspace(void) {
    if (!screen || cursor_x <= MARGIN) return;
    cursor_x -= ADVANCE;
    for (uint64_t y = cursor_y; y < cursor_y + 7 * SCALE && y < screen->height; ++y)
        for (uint64_t x = cursor_x; x < cursor_x + ADVANCE && x < screen->width; ++x)
            put_pixel(screen, x, y, background);
}
