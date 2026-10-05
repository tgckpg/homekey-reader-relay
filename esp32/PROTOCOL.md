# BLE NFC relay v1

Service: `7a6b0001-5b21-4f36-8e5d-9c3a26d74210`.

| Characteristic suffix | Access | Value |
| --- | --- | --- |
| `0002` | Read/write | Existing 8-byte PING/PONG probe |
| `0003` | Read | 18-byte NFC session status |
| `0004` | Read/write | Fragmented APDU request / stable response mailbox |

All multi-byte integers below are big-endian. The UUID suffix means the last
four digits of the UUID's first component, with the remainder unchanged.

Status: `version:u8=1, active:u8, uid_length:u8, sak:u8, session:u32, uid[10]`.
UID bytes are metadata, not credentials. Active means an APDU session is open;
it does not guarantee that the phone remains physically present. Session IDs
start from a random boot seed and increase for each offered target. Reconnect
invalidates all old requests and responses.

APDU packet header (12 bytes):
`version:u8=1, kind:u8, session:u32, sequence:u16, offset:u16, total:u16`.

| Kind | Direction | Meaning |
| --- | --- | --- |
| `01` | Go → ESP32 | Command fragment; payload follows the header |
| `02` | Go → ESP32 | Release session; zero offset/total, no payload |
| `03` | Go → ESP32 | ECP configuration; session/sequence/offset = 0, total = 8 (Home group ID) or 0 (disable) |
| `10` | ESP32 → Go | Pending response; zero offset/total, no payload |
| `11` | ESP32 → Go | Ready response; zero offset, total = payload size |
| `12` | ESP32 → Go | Transport error; zero offset, total = 1, error byte |

Each APDU sequence starts at 1 and strictly increases within its session. Only
one APDU is admitted at a time. Fragments must be contiguous with matching
session/sequence/total, within 4..240 bytes. A duplicate, gap, stale session or
busy write is rejected. No APDU is executed until its complete payload is
queued to the NFC owner task.

Go uses ATT write requests and fragments to MTU-3 when BlueZ reports MTU,
otherwise to 20 bytes. Responses are read using BlueZ's long-read support.
The response remains stable across repeated reads until the next complete
request or a connection/session reset. A GATT callback never touches UART.

Error bytes: 1 = invalid APDU/target, 2 = UART/frame/timeout failure,
3 = PN532 error or unsupported chaining flags, 4 = response too large.
Normal `11` responses include the APDU's SW1/SW2 bytes. PN532 errors cannot
be mistaken for an APDU status word.

The owner holds the selected target while Go sends SELECT/AUTH0/AUTH1 and
CONTROL FLOW. It turns RF off after release, transport failure, disconnect or
15-second expiry, then resumes presence polling. Held cards are suppressed by
the existing UID presence state until removal; disconnected detections are
retried after reconnection. GPIO3 TX / GPIO4 RX and startup wake packets are
preserved.

## Home Key ECP polling

Matching firmware requires Go's `03` configuration on every BLE connection.
The configuration is a single 20-byte ATT write, fitting the minimum MTU.
It carries only the public 8-byte Home group identifier:
`SHA256("key-identifier" || provisioned_reader_private_scalar)[0:8]`.
This is the group half of the 16-byte NFC reader identifier, not its unique
reader half or the Bluetooth MAC address. Private keys remain in Go.

Go refreshes this value between sessions when provisioning changes; zero total
disables polling until a reader key exists. Firmware clears configuration on
connect/disconnect and keeps RF off until configured. GATT writes only update
mutex-protected metadata; an active APDU session is never interrupted, and a
configuration update applies to subsequent scans.

Each scan transmits `6a02cb0206021100 || group[8] || CRC-A[2]` using PN532
InCommunicateThru before InListPassiveTarget activates a card. The ESP32 selects
106 kbps Type A/full-byte transmission and disables hardware CRC for this raw
frame, since CRC-A is supplied in software. PN532 RF status `01` (no reply) is
normal for this broadcast; host UART/frame timeouts and other PN532 errors
remain failures. A short ECP response timeout is restored to the PN532 default
before card activation. NFC polling/ECP remain suspended during APDU exchange.

The ECP express flag is enabled; the phone's Home Key Express Mode setting
still applies. Disabling Express Mode should present the key for user approval.
ECP chooses the credential; STANDARD authentication still verifies it before
unlocking. FAST and unknown-device attestation remain outside this release.

Hardware validation is required: host tests cannot prove that a phone receives
RF frames or selects its Home Key.
