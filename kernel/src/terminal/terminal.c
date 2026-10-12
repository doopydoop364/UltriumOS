#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "terminal.h"
#include "vga8x16.h"

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

static void draw_character(struct limine_framebuffer *fb, char ch,
                           uint64_t x, uint64_t y, uint32_t color) {
    unsigned char code = (unsigned char)ch;
    if (code < 32 || code > 126) code = '?';
    const uint8_t *rows = ultrium_vga8x16_ascii[code - 32];
    for (uint64_t row = 0; row < 16; ++row)
        for (uint64_t col = 0; col < 8; ++col)
            if (rows[row] & (0x80u >> col))
                put_pixel(fb, x + col, y + row, color);
}

static struct limine_framebuffer *screen;
static uint64_t cursor_x, cursor_y;
static uint32_t ink, background, accent, muted;
enum { MARGIN = 16, ADVANCE = 8, GLYPH_HEIGHT = 16, LINE_HEIGHT = 16 };

static void scroll_if_needed(void) {
    if (!screen || cursor_y + GLYPH_HEIGHT + MARGIN <= screen->height) return;
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
    background = fb_pixel(fb, 0, 0, 0);
    ink = fb_pixel(fb, 243, 241, 236); // PULSE warm white #F3F1EC
    accent = fb_pixel(fb, 255, 90, 54); // PULSE signal red #FF5A36
    muted = fb_pixel(fb, 138, 143, 152); // PULSE muted gray #8A8F98
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
    if (cursor_x + ADVANCE + MARGIN > screen->width) terminal_newline();
    uint32_t selected = color == TERMINAL_ACCENT ? accent :
                        color == TERMINAL_MUTED ? muted : ink;
    draw_character(screen, ch, cursor_x, cursor_y, selected);
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
    for (uint64_t y = cursor_y; y < cursor_y + GLYPH_HEIGHT && y < screen->height; ++y)
        for (uint64_t x = cursor_x; x < cursor_x + ADVANCE && x < screen->width; ++x)
            put_pixel(screen, x, y, background);
}

void terminal_cursor(int visible) {
    if (!screen) return;
    uint32_t color = visible ? accent : background;
    for (uint64_t y = cursor_y + GLYPH_HEIGHT - 2; y < cursor_y + GLYPH_HEIGHT && y < screen->height; ++y)
        for (uint64_t x = cursor_x; x < cursor_x + ADVANCE && x < screen->width; ++x)
            put_pixel(screen, x, y, color);
}
