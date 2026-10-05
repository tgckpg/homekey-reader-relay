# HomeKey Reader Relay
`PN532 -> ESP32-C3 (this part) -> Home Assistant`

In development. NOT FOR USE!

Current firmware connects directly to `homekey-go` over BLE GATT and relays
NFC APDUs. The HACS component and service-data examples below describe the
earlier advertisement-based firmware; current firmware does not emit those
UID advertisements. See [the current relay protocol](esp32/PROTOCOL.md).

## Legacy Home Assistant UI setup

HACS

1. Settings -> Devices & services -> Add Integration.
2. Search for `HomeKey Reader`.
3. Pick the advertising ESP32-C3 reader.

Because the manifest also contains a Bluetooth matcher, Home Assistant can offer
the reader automatically when it sees the service-data advertisement.


## Install toolchain

```
brew install libgcrypt glib pixman sdl2 libslirp dfu-util cmake python

brew tap espressif/eim
brew install eim

eim install

source "$HOME/.espressif/tools/activate_idf_v6.x.x.sh"
```

### Flash the firmware

```sh
cd esp32
idf.py set-target esp32c3
idf.py -p /dev/cu.usbmodem101 flash monitor
```

## Current flow

```text
```

Service UUID:

```text
7b1e0001-7b1e-4a11-9abc-0123456789ab
```

Advertisement payload after the UUID:

```text
byte 0	  protocol version = 1
byte 1	  message type = 1 (mock PN532 tag)
byte 2..3   sequence, uint16 little-endian
byte 4..7   mock UID = DE AD BE EF
```

### Testing the PN532

```
LIBNFC_DEVICE=pn532_uart:/dev/cu.usbserial-8310 nfc-list
LIBNFC_DEVICE=pn532_uart:/dev/cu.usbserial-8310 nfc-poll
```

Test pins
```
idf.py -B build-pin-test -D PIN_TEST=ON build flash monitor
```

Test pn532
```
idf.py -B build-pn532-test -D PNC532_TEST=ON build flash monitor
```

## NFC APDU relay

The firmware now exposes session status and an APDU mailbox for `homekey-go`
0.0.5. Deploy the matching firmware and Go service together. NFC runs in
one UART owner task; the RF field stays on throughout authentication.

Go now supplies the provisioned Home group identifier over BLE. The reader
emits Home Key ECP before activation so iOS can select the key automatically.
Polling waits for this configuration on every connection. FAST authentication
and unknown-device attestation remain follow-up work. `homekey-go` verifies the key; UID detection alone never unlocks.
See `esp32/PROTOCOL.md` for the mailbox format and `tests/run.sh` for host tests.
