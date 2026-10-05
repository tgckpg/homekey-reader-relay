#ifndef HOMEKEY_READER_H
#define HOMEKEY_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NFC_UID_MAX 10

typedef struct {
	uint8_t uid[NFC_UID_MAX];
	uint8_t uid_len;
	uint8_t target;
	uint8_t sak;
} nfc_card_t;

typedef bool (*pn532_card_callback_t)(const nfc_card_t *card);

/* Blocks in the caller's task. Callback returns false if its queue is full. */
void start_polling(pn532_card_callback_t callback);
/* Only called by the NFC owner task while its target is active. */
int pn532_exchange(uint8_t target, const uint8_t *apdu, size_t length,
				   uint8_t *out, size_t capacity, size_t *out_length);

static inline bool nfc_uid_valid(size_t length)
{
	return length == 4 || length == 7 || length == 10;
}

static inline bool nfc_card_equal(const nfc_card_t *a, const nfc_card_t *b)
{
	return a->uid_len == b->uid_len &&
		   memcmp(a->uid, b->uid, a->uid_len) == 0;
}

typedef struct {
	nfc_card_t last;
	bool present;
	unsigned misses;
} nfc_presence_t;

/* Only successful no-target scans count toward removal. */
static inline bool nfc_presence_update(nfc_presence_t *state, int result,
									   const nfc_card_t *card)
{
	if (result == 1) {
		state->misses = 0;
		return !state->present || !nfc_card_equal(&state->last, card);
	}
	if (result == 0) {
		if (state->present && ++state->misses >= 2) {
			state->present = false;
		}
	} else {
		state->misses = 0;
	}
	return false;
}

static inline void nfc_presence_accept(nfc_presence_t *state,
									   const nfc_card_t *card)
{
	state->last = *card;
	state->present = true;
}

/* Nonconnectable legacy ADV: one 128-bit service-data AD, at most 31 bytes.
 * v2 payload = version(2), event_seq LE16, UID (length inferred).
 * Flags/name/type are omitted to fit a full 10-byte UID.
 */
static inline size_t build_card_adv(uint8_t adv[31], const nfc_card_t *card,
									uint16_t event_seq)
{
	static const uint8_t uuid[16] = {
		0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0xbc, 0x9a,
		0x11, 0x4a, 0x1e, 0x7b, 0x01, 0x00, 0x1e, 0x7b,
	};
	if (!nfc_uid_valid(card->uid_len)) {
		return 0;
	}
	adv[0] = (uint8_t)(20 + card->uid_len);
	adv[1] = 0x21;
	memcpy(adv + 2, uuid, sizeof(uuid));
	// Protocol Version
	adv[18] = 1;
	adv[19] = (uint8_t)event_seq;
	adv[20] = (uint8_t)(event_seq >> 8);
	memcpy(adv + 21, card->uid, card->uid_len);
	return 21 + card->uid_len;
}

#endif
