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
