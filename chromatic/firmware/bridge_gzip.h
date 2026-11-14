#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cf_inflate.h"

enum { BridgeMaxDatagram = 1200, BridgeMaxGzip = 1280 };

int bridge_gzip(const uint8_t *input, size_t input_size, uint8_t *output);
int bridge_gunzip(const uint8_t *input, size_t input_size, uint8_t *output, cf_infl_t *state);
