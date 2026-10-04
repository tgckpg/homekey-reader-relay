# HomeKey Reader Relay
`PN532 -> ESP32-C3 (this part) -> Home Assistant`

In development. NOT FOR USE!

## Home Assistant UI setup

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

## Mock Flow

```text
ESP32-C3
  mock PN532 UID (DE:AD:BE:EF)
       |
       v
BLE service-data advertisement
       |
       v
Home Assistant Bluetooth
       |
       +--> HomeKey Reader config entry
       +--> sensor: Last NFC UID
       +--> event: homekey_reader_packet
```

Service UUID:

```text
7b1e0001-7b1e-4a11-9abc-0123456789ab
```

Advertisement payload after the UUID:

```text
byte 0      protocol version = 1
byte 1      message type = 1 (mock PN532 tag)
byte 2..3   sequence, uint16 little-endian
byte 4..7   mock UID = DE AD BE EF
```

### Testing the PN532

```
LIBNFC_DEVICE=pn532_uart:/dev/cu.usbserial-8310 nfc-list
LIBNFC_DEVICE=pn532_uart:/dev/cu.usbserial-8310 nfc-poll
```
