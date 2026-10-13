#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define LD_GATES 16
#define LD_PAYLOAD_MAX 128

typedef enum { LD_ACK, LD_MINIMAL, LD_DATA } ld_frame_kind;
typedef void (*ld_frame_cb)(ld_frame_kind kind, const uint8_t *data, size_t size, void *ctx);
typedef struct {
    uint8_t bytes[LD_PAYLOAD_MAX + 10];
    size_t used;
    uint32_t bad_bytes;
} ld_parser;
typedef struct {
    uint8_t state;
    uint16_t distance_cm;
    bool energy_valid;
    uint32_t energy[LD_GATES];
} ld_reading;
typedef struct {
    uint32_t far_gate, near_gate, absence_s, status_hz_x10, distance_hz_x10, response;
    uint32_t trigger[LD_GATES], hold[LD_GATES];
} ld_settings;
uint16_t ld_u16(const uint8_t *p);
uint32_t ld_u32(const uint8_t *p);
void ld_put16(uint8_t *p, uint16_t v);
void ld_put32(uint8_t *p, uint32_t v);
size_t ld_command(uint16_t command, const uint8_t *payload, size_t size, uint8_t *out, size_t capacity);
void ld_feed(ld_parser *p, const uint8_t *data, size_t size, ld_frame_cb cb, void *ctx);
bool ld_decode_reading(ld_frame_kind kind, const uint8_t *data, size_t size, ld_reading *out);
bool ld_decode_progress(ld_frame_kind kind, const uint8_t *data, size_t size, uint16_t *hundredths);
const char *ld_validate(const ld_settings *s);
