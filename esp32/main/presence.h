#ifndef HOMEKEY_PRESENCE_H
#define HOMEKEY_PRESENCE_H
#include <stdbool.h>

void presence_init(void);
/* NFC owner only: checks absence between complete NFC sessions. */
bool presence_sleep_requested(void);
/* Called after PN532 PowerDown succeeded and Bluetooth has stopped. */
void presence_enter_sleep(void);
#endif
