#include <gb/gb.h>
#include <gb/cgb.h>
#include "road.h"

static const palette_color_t backgrounds[][4] = {
    {RGB8(19, 30, 36), RGB8(73, 91, 96), RGB8(140, 156, 151), RGB8(244, 233, 203)},
    {RGB8(36, 59, 35), RGB8(59, 86, 49), RGB8(115, 106, 69), RGB8(84, 122, 64)},
    {RGB8(51, 54, 52), RGB8(83, 85, 81), RGB8(65, 68, 67), RGB8(224, 226, 211)},
    {RGB8(51, 54, 52), RGB8(83, 85, 81), RGB8(65, 68, 67), RGB8(240, 188, 65)},
};

static const palette_color_t sprites[][4] = {
    {0, RGB8(247, 240, 217), RGB8(18, 23, 27), RGB8(22, 27, 32)},
    {0, RGB8(21, 30, 39), RGB8(186, 52, 46), RGB8(255, 162, 101)},
    {0, RGB8(115, 69, 42), RGB8(244, 171, 62), RGB8(254, 235, 200)},
    {0, RGB8(43, 37, 50), RGB8(230, 174, 142), RGB8(153, 118, 206)},
    {0, RGB8(52, 45, 39), RGB8(251, 237, 196), RGB8(247, 125, 42)},
    {0, RGB8(201, 64, 39), RGB8(253, 155, 52), RGB8(255, 241, 169)},
    {0, RGB8(90, 22, 25), RGB8(166, 38, 36), RGB8(240, 93, 68)},
};

static void dim_palette(palette_color_t *out, const palette_color_t *in, uint8_t darkness)
{
    for (uint8_t i = 0; i < 4; ++i) {
        // Shift the three RGB555 channels independently.
        const palette_color_t color = in[i];
        out[i] = darkness == 3 ? 0 :
            ((color & 0x001Fu) >> darkness) |
            (((color & 0x03E0u) >> darkness) & 0x03E0u) |
            (((color & 0x7C00u) >> darkness) & 0x7C00u);
    }
}

void road_color_fade(uint8_t darkness)
{
    if (_cpu != CGB_TYPE) return;

    palette_color_t colors[4];
    for (uint8_t n = 0; n < sizeof backgrounds / sizeof backgrounds[0]; ++n) {
        dim_palette(colors, backgrounds[n], darkness);
        set_bkg_palette(n, 1, colors);
    }
    for (uint8_t n = 0; n < sizeof sprites / sizeof sprites[0]; ++n) {
        dim_palette(colors, sprites[n], darkness);
        set_sprite_palette(n, 1, colors);
    }
}
