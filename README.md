# HomeKey Reader Relay

```
PN532 -> | ESP32-C3 (this part) | -> Home Assistant
         |  ^                   |
         | HLK-LD2410S sensor   |
```


In development. NOT FOR USE!

Current firmware connects directly to `homekey-go` over BLE GATT and relays
NFC APDUs. The HACS component and service-data examples below describe the
earlier advertisement-based firmware; current firmware does not emit those
UID advertisements. See [the current relay protocol](esp32/PROTOCOL.md).


## Workflow

LD2410S OT2 HIGH -> ESP32-C3 wakes/boots -> PN532 UART wake -> BLE reconnect
and Home group configuration -> ECP + NFC polling.

OT2 LOW -> absence confirmation -> finish any active NFC session -> PN532
RF off + PowerDown -> stop BLE -> ESP32-C3 deep sleep until OT2 HIGH.

### Presence wiring

| Module pin | ESP32-C3 / supply |
| --- | --- |
| LD2410S 3V3 | Always-on regulated 3.3 V |
| LD2410S GND | Common GND |
| LD2410S OT2 | **GPIO5** (HIGH = present) |
| LD2410S OT1/TX and RX | Disconnected in relay firmware |
| PN532 RX | **GPIO3** |
| PN532 TX | **GPIO4** |
| PN532 supply / GND | Existing regulated supply / common GND |

Remove the calibration firmware's radar UART wiring from GPIO3/4 before
reconnecting PN532. The relay uses OT2 only and does not change the radar's
saved calibration/settings. Keep LD2410S powered during ESP sleep. PN532 also
stays supplied: this is its software PowerDown mode, not a switched supply.
Its UART TX drive from the ESP is held idle HIGH across sleep to avoid false
wakes. Never connect battery voltage directly to 3.3 V power or GPIO pins.

### Sleep behavior and settings

`idf.py menuconfig` -> **Home Key presence / sleep**:

- Presence sleep is enabled by default on ESP32-C3. Disable it for bench
  operation without LD2410S; otherwise an unconnected input reads absent.
- OT2 defaults to GPIO5. Only GPIO0-5 support this C3 deep-sleep wake path;
  GPIO3/4 conflict with PN532 and are rejected at compile time. Avoid GPIO2
  because it is a boot strapping pin.
- Default additional absence confirmation: **1 second** after observing LOW.
  The radar's own configured absence delay happens first.
- Default cold/reset boot grace: **10 seconds** awake for sensor startup and
  USB access. It does not delay NFC startup. GPIO wakes skip this grace.

Deep sleep **disconnects BLE** and wake starts a new boot. The configured reader
address remains the same, but `homekey-go` must reconnect and resend its Home
group before polling resumes. Approach-to-ready time therefore includes boot
and BLE reconnect, not just PN532 wake. No ECP/credential data is cached across
sleep, and there are no periodic timer wakes.

An active NFC callback finishes (or hits its existing session timeout) before
the owner task considers sleep. Absence may therefore take longer during an
authentication. PN532 PowerDown must return a complete success response; a
failure keeps ESP awake, wakes/reinitializes PN532, and retries. A returning
HIGH during shutdown triggers an immediate GPIO wake after sleep entry.

The implementation selects the renamed wake API on ESP-IDF 6.x and the older
API on 5.x. It needs no automatic light-sleep or Bluetooth sleep configuration.
The standalone pin/PN532 test builds do not use presence control.

### Build and verify on hardware

```sh
cd esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

1. Keep someone in range. Expect the OT2 GPIO log, PN532 firmware log, BLE
   connection, Home Key ECP configuration, and normal phone authentication.
2. Leave the area. After OT2 goes LOW and the confirmation interval passes,
   expect `Absent: PN532 asleep, BLE stopped; ESP32 deep sleep`.
3. Approach again. Expect a new boot and BLE/ECP setup, then test Home Key.
4. Repeat several cycles; also test presence returning during shutdown and
   leaving during authentication. Compare supply current awake/asleep.

Native USB can disappear in deep sleep. Approach the sensor to wake it, or
hold BOOT and tap RESET to enter the ROM downloader if flashing cannot connect.
Development-board LEDs/regulators and PN532 board circuitry can dominate sleep
current; chip sleep does not guarantee a particular battery life.

Host tests cover absence timing, cold/GPIO wake policy, disabled presence,
PN532 wake delay, confirmed/rejected/truncated/missing PowerDown responses,
the polling-to-sleep transition, and existing APDU/ECP behavior. This change
has not been compiled with a full ESP-IDF toolchain or tested on physical
hardware in the development environment.

Protocol references: [LD2410S OT2 definition](https://h.hlktech.com/download/HLK-LD2410S-24G/1/HLK-LD2410S-%E4%B8%B2%E5%8F%A3%E9%80%9A%E4%BF%A1%E5%8D%8F%E8%AE%AE-V1.00%20.pdf),
[PN532 user manual, section 7.2.11](https://www.nxp.com/docs/en/user-guide/141520.pdf),
[ESP32-C3 sleep modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/sleep_modes.html).


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
