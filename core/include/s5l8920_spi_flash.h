/* Explicit decoded-byte connection; no automatic board attachment.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_SPI_FLASH_H
#define S5LBOX_S5L8920_SPI_FLASH_H
#include "s5l8920_spi.h"
#include "sst25vf080b.h"

/* Connect an already configured 8-bit SPI word link to an explicit flash.
 * Caller supplies qualified mode0/3, MSB-first electrical traffic, synchronizes
 * flash CE with software CS using sst25vf080b_pins, and supplies independently
 * divided SCK periods. Flash time and controller word-delay time remain separate
 * inputs. Neither CPU cycles, pin defaults nor a NOR image are inferred.
 * bias_value/bias_known describe each bit sampled when SO is undriven. Unknown
 * received bits refuse the whole call. TX-only traffic requires no MISO bias.
 * Both device/controller states and count are unchanged on any refusal, even
 * after a prefix of valid bytes. Byte effects occur at completed-word boundaries;
 * partial words remain in the controller. Do not mix this with a different peer
 * or change flash pins during a partial word. No board IRQ refresh is implied. */
bool s5l8920_spi_flash_clock(s5l8920_spi_t *s, sst25vf080b_t *flash,
    uint64_t periods, uint8_t bias_value, uint8_t bias_known, size_t *count);
#endif
