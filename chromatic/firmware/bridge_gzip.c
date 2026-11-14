#include "bridge_gzip.h"

#include <string.h>

#include "cf_inflate.h"

static uint32_t crc32_bytes(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t read_le(const uint8_t *bytes, unsigned count)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < count; ++i) result |= (uint32_t)bytes[i] << (8 * i);
    return result;
}

static void write_le(uint8_t *bytes, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) bytes[i] = value >> (8 * i);
}

typedef struct {
    uint8_t *output;
    size_t bits;
} BitWriter;

static void write_bits(BitWriter *writer, unsigned value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i, ++writer->bits) {
        size_t offset = writer->bits / 8;
        unsigned bit = writer->bits % 8;
        if (!bit) writer->output[offset] = 0;
        writer->output[offset] |= ((value >> i) & 1u) << bit;
    }
}

static void write_symbol(BitWriter *writer, unsigned symbol)
{
    unsigned code;
    unsigned width;
    if (symbol < 144) {
        code = 0x30 + symbol;
        width = 8;
    } else if (symbol < 256) {
        code = 0x190 + symbol - 144;
        width = 9;
    } else if (symbol < 280) {
        code = symbol - 256;
        width = 7;
    } else {
        code = 0xc0 + symbol - 280;
        width = 8;
    }
    for (unsigned i = width; i; --i) write_bits(writer, code >> (i - 1), 1);
}

static void write_run(BitWriter *writer, unsigned length)
{
    static const uint16_t base[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    unsigned index = 0;
    while (index + 1 < sizeof(base) / sizeof(*base) && base[index + 1] <= length) ++index;
    write_symbol(writer, 257 + index);
    if (index >= 8 && index < 28) write_bits(writer, length - base[index], (index - 4) / 4);
    write_bits(writer, 0, 5);  // Distance code zero copies the preceding byte.
}

int bridge_gzip(const uint8_t *input, size_t size, uint8_t *output)
{
    if (size > BridgeMaxDatagram) return -1;
    static const uint8_t header[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    memcpy(output, header, sizeof(header));
    uint8_t *body = output + sizeof(header);
    BitWriter writer = {.output = body};
    write_bits(&writer, 3, 3);  // Final block, fixed Huffman tables.

    // Detect repeated bytes without a heap allocation or a match dictionary.
    // Use a stored DEFLATE block if fixed codes would take up more space.
    for (size_t i = 0; i < size && writer.bits <= size * 8; ) {
        unsigned run = 0;
        if (i && input[i] == input[i - 1]) {
            while (run < 258 && i + run < size && input[i + run] == input[i]) ++run;
        }
        if (run >= 3) {
            write_run(&writer, run);
            i += run;
        } else {
            write_symbol(&writer, input[i++]);
        }
    }
    write_symbol(&writer, 256);
    size_t body_size = (writer.bits + 7) / 8;
    if (writer.bits >= size * 8) {
        body[0] = 1;  // Final block, stored.
        write_le(body + 1, size, 2);
        write_le(body + 3, (uint16_t)~size, 2);
        memcpy(body + 5, input, size);
        body_size = size + 5;
    }
    write_le(body + body_size, crc32_bytes(input, size), 4);
    write_le(body + body_size + 4, size, 4);
    return sizeof(header) + body_size + 8;
}

static int read_input(void *data, uint32_t offset, uint8_t *output, uint32_t size)
{
    memcpy(output, (const uint8_t *)data + offset, size);
    return 0;
}

int bridge_gunzip(const uint8_t *input, size_t size, uint8_t *output, cf_infl_t *state)
{
    if (size < 20 || size > BridgeMaxGzip || input[0] != 0x1f || input[1] != 0x8b ||
        input[2] != 8 || input[3] & 0xe0) return -1;
    const uint8_t flags = input[3];
    const size_t end = size - 8;
    size_t start = 10;
    if (flags & 4) {
        if (end - start < 2) return -1;
        size_t extra = read_le(input + start, 2);
        start += 2;
        if (extra > end - start) return -1;
        start += extra;
    }
    for (unsigned flag = 8; flag <= 16; flag *= 2) {
        if (flags & flag) {
            const uint8_t *nul = memchr(input + start, 0, end - start);
            if (!nul) return -1;
            start = nul - input + 1;
        }
    }
    if (flags & 2) {
        if (end - start < 2 || (crc32_bytes(input, start) & 0xffff) != read_le(input + start, 2)) return -1;
        start += 2;
    }
    size_t decoded = read_le(input + end + 4, 4);
    if (start >= end || decoded > BridgeMaxDatagram) return -1;
    uint8_t *segments[] = {output};
    int result = cf_inflate(state, read_input, (void *)input, start, end - start, segments, 11, decoded);
    if (result || crc32_bytes(output, decoded) != read_le(input + end, 4)) return -1;
    return decoded;
}
