#ifndef HOMEKEY_RELAY_H
#define HOMEKEY_RELAY_H
#include "reader.h"

#define RELAY_APDU_MAX 240
#define RELAY_HEADER 12
#define RELAY_VALUE_MAX (RELAY_HEADER + RELAY_APDU_MAX)
/* GATT callbacks are nonblocking. PN532/UART work stays in its owner task. */
void relay_init(void);
void relay_connected(bool connected);
size_t relay_status(uint8_t out[18]);
size_t relay_response(uint8_t out[RELAY_VALUE_MAX]);
bool relay_write(const uint8_t *data, size_t length);
bool relay_card(const nfc_card_t *card);
#endif
