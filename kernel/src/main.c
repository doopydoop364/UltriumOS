#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}

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

static void welcome_screen(struct limine_framebuffer *fb) {
    uint32_t background = fb_pixel(fb, 11, 16, 32);
    uint32_t foreground = fb_pixel(fb, 237, 242, 249);
    uint32_t accent = fb_pixel(fb, 105, 230, 192);
    uint32_t muted = fb_pixel(fb, 158, 176, 198);
    fill_screen(fb, background);
    draw_text(fb, "ULTRIUM OS", 48, 64, 4, foreground);
    draw_text(fb, "KERNEL 0.0.1", 48, 128, 2, muted);
    draw_text(fb, "BOOT OK", 48, 182, 2, accent);
    draw_text(fb, "SHELL COMING SOON", 48, 222, 2, muted);
}

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) hcf();
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) hcf();

    for (uint64_t i = 0; i < framebuffer_request.response->framebuffer_count; ++i) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[i];
        if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) hcf();
        welcome_screen(fb);
    }
    hcf();
}
