/* Renders every demo screen to a PNG file, without a screen. Runs on a laptop, so the
 * design can be checked without the Pi. Usage: snapshot <output folder> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "lvgl.h"
#include "ui/dd_ui.h"
#include "views/demo_screens.h"

#define WIDTH 1024
#define HEIGHT 600

static uint32_t pixels[WIDTH * HEIGHT];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

static uint32_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void put_u32(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
    fwrite(bytes, 1, 4, file);
}

static void put_chunk(FILE *file, const char *type, const uint8_t *data, uint32_t length)
{
    put_u32(file, length);
    fwrite(type, 1, 4, file);
    fwrite(data, 1, length, file);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), data, length);
    put_u32(file, (uint32_t)crc);
}

static int write_png(const char *path)
{
    /* Each row: filter byte 0, then RGB. */
    size_t raw_size = (size_t)HEIGHT * (1 + WIDTH * 3);
    uint8_t *raw = malloc(raw_size);
    uLongf packed_size = compressBound(raw_size);
    uint8_t *packed = malloc(packed_size);
    if (raw == NULL || packed == NULL) {
        return -1;
    }
    uint8_t *out = raw;
    for (int y = 0; y < HEIGHT; y++) {
        *out++ = 0;
        for (int x = 0; x < WIDTH; x++) {
            uint32_t p = pixels[y * WIDTH + x];
            *out++ = (uint8_t)(p >> 16);
            *out++ = (uint8_t)(p >> 8);
            *out++ = (uint8_t)p;
        }
    }
    compress2(packed, &packed_size, raw, raw_size, 9);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t header[13] = {0, 0, WIDTH >> 8, WIDTH & 0xFF, 0, 0, HEIGHT >> 8, HEIGHT & 0xFF, 8, 2, 0, 0, 0};
    fwrite(signature, 1, 8, file);
    put_chunk(file, "IHDR", header, sizeof(header));
    put_chunk(file, "IDAT", packed, (uint32_t)packed_size);
    put_chunk(file, "IEND", NULL, 0);
    fclose(file);
    free(raw);
    free(packed);
    return 0;
}

int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : ".";

    lv_init();
    lv_tick_set_cb(milliseconds);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    dd_theme_init();

    for (uint32_t i = 0; i < DEMO_SCREEN_COUNT; i++) {
        lv_obj_t *old = lv_screen_active();
        lv_screen_load(demo_screen_create((demo_screen_t)i));
        lv_obj_delete(old);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display);

        char path[512];
        snprintf(path, sizeof(path), "%s/%s.png", folder, demo_screen_name((demo_screen_t)i));
        if (write_png(path) != 0) {
            fprintf(stderr, "Could not write %s\n", path);
            return 1;
        }
        printf("%s\n", path);
    }
    return 0;
}
