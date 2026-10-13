# LD2410S calibration tool for ESP32-C3

ESP-IDF firmware that connects an LD2410S directly to an ESP32-C3 and serves a local Wi-Fi calibration dashboard. Mount the breadboard on the wall, power it from USB, and test from your Mac or phone.

**Calibration fix:** device progress is an integer percentage (0–100), not hundredths. Earlier source displayed completion as 1% and eventually timed out. Rebuild from this source to apply the correction. The included prebuilt binaries were compiled before this fix and still have that progress bug.

This is a standalone calibration firmware. Flashing it replaces the running Home Key application on the ESP32; keep that project's build/source so you can flash it back afterwards. Disconnect the PN532 from GPIO3/GPIO4 while using these defaults.

## Wiring

| LD2410S | ESP32-C3 |
| --- | --- |
| 3V3 | Regulated 3V3 output |
| GND | GND |
| OT1 / TX / TX0 | **GPIO3** (ESP RX) |
| RX / RX0 | **GPIO4** (ESP TX) |
| OT2 | Leave disconnected |

UART1 runs at 115200 baud, 8N1. The ESP's USB console remains available. Sensor power and logic must be 3.3 V; do not connect 5 V or an unregulated 21700. After flashing, a USB wall supply or power bank can power the ESP board. Check that your power bank stays on at this load.

Fix the sensor firmly in position, with the antenna side facing the area to test. Avoid moving it during measurements or calibration.

## Connect wirelessly

The default firmware creates:

- **Wi-Fi SSID:** `LD2410S-Bench`
- **Password:** `calibrate2410`
- **Dashboard:** http://192.168.4.1/

Join that Wi-Fi on your Mac or phone. It has no internet; remain connected if your operating system asks. Open the dashboard manually in Firefox/Safari/Chrome. There is no captive portal or cloud service.

At boot, the ESP automatically attempts to read the sensor settings. Click **Read / retry sensor** if the sensor was wired or powered later. Enable **Show gate energy** to receive all 16 gate energy values. Distance is shown only for a detected person; the dashboard marks old readings as stale.

## Optional: use your home Wi-Fi

To access it from your existing home network, build from source and run `idf.py menuconfig`. Under **LD2410S bench**, set:

- Optional home Wi-Fi SSID
- Optional home Wi-Fi password

The bench AP remains available. The home network IP appears in the USB log and on the dashboard after connection. Use that numeric IP to open it from your normal Wi-Fi; mDNS and custom hostnames are not enabled. A failed home network connection does not disable the bench AP.

The same menu changes UART pins and the AP password. The supplied binary uses the defaults above. Wi-Fi credentials set in menuconfig are compiled into that customized firmware; don't share it or its generated sdkconfig.

## Flash the included default binaries

These are legacy binaries: **they do not include the calibration progress fix**. For calibration, use the source build instructions below.

The `firmware/` folder contains ESP32-C3 binaries compiled with ESP-IDF 5.4.1, using the default AP and UART pins above. From this directory, in your usual ESP-IDF environment:

```sh
python3 flash.py --port /dev/cu.usbmodem101
```

If you aren't in an ESP-IDF environment, install esptool in a virtual environment first:

```sh
python3 -m venv .flash-venv
source .flash-venv/bin/activate
python3 -m pip install 'esptool>=4.8,<5'
python3 flash.py --port /dev/cu.usbmodem101
```

The script verifies SHA-256 checksums and flashes only the bootloader, partition table and application at their individual offsets. It does not write a merged image across the NVS region or run erase-flash. Use `--baud 115200` if the serial adapter is unreliable at 460800. If auto-reset fails, hold BOOT, tap RESET, release BOOT, and retry; reset the board after flashing if it remains in download mode.

After flashing, join `LD2410S-Bench` and open http://192.168.4.1/. For different pins or home Wi-Fi credentials, rebuild and flash from source instead of using these default binaries.

## Build and flash from source

Use ESP-IDF **5.4.1 or newer**, including 6.1, with ESP32-C3 tools installed. IDF 5.x uses its built-in JSON component; IDF 6.x downloads `espressif/cjson` using `main/idf_component.yml`. Leave the IDF Component Manager enabled (the default) and allow internet access for that first download. From this project directory, in your normal ESP-IDF shell:

```sh
idf.py set-target esp32c3
idf.py menuconfig       # optional: Wi-Fi credentials / UART pins
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Exit the serial monitor with Ctrl-]. If that port isn't present, list `/dev/cu.*` and use the actual board device. The project defaults to the C3 native USB Serial/JTAG console and a 2 MB flash layout. If your board uses a separate USB–UART bridge, choose UART console in menuconfig instead.

If an older copy reports `Failed to resolve component 'json'` on IDF 6.x, replace `main/CMakeLists.txt` and add `main/idf_component.yml` from this package, then run `idf.py reconfigure` before `idf.py menuconfig`. Keep your existing `sdkconfig` to retain your Wi-Fi and pin choices.

No erase-flash step is needed. Bootloader, partition table, and application are flashed; the normal commands leave NVS intact. Startup deliberately does not automatically erase NVS if initialization fails. If you hit an NVS initialization error, inspect your board's existing data/layout before deciding whether to erase it.

## Calibration workflow

1. Mount the sensor in the final orientation and leave normal stationary surroundings in place.
2. Read settings and download the current settings JSON.
3. Enable gate energy. Walk towards it, stand still, walk away, and check for responses outside the intended range.
4. Set distance gates, response speed and report frequency, then click **Apply settings & verify**.
5. Start automatic calibration. The default **15-second start delay** gives you time to leave the room; the default scan lasts **120 seconds**, with trigger/hold factors **2/1**.
6. Keep the detection area empty throughout the scan. Watch progress remotely. Only a device progress report of **100%** means complete; elapsed time alone is not treated as success.
7. Click **Read from sensor** after completion to load the generated thresholds.
8. Test again, then switch to **Minimal output** before returning to the Home Key firmware.

Calibration and serial commands run in a worker task, so the web dashboard keeps responding during the countdown and scan. Keep the board powered and do not move it during a scan. There is no undocumented stop/cancel command; wait for completion rather than interrupting power.

## Backups and exports

Before every settings write or automatic calibration, the ESP reads the current settings and saves them to its own NVS flash. If reading or saving fails, it refuses the mutation. **One pre-change backup** is retained across reboots; the next mutation replaces it.

- **Download current settings** exports the last successfully read settings. Read first if you need a fresh snapshot.
- **Download pre-change backup** exports that retained snapshot.
- **Restore pre-change backup** writes it back and verifies readback. The pre-restore settings become the new pre-change backup, allowing an undo.
- **Load backup into editor** accepts the Python tool's JSON format too. It doesn't write anything until Apply is clicked.
- CSV exports the last **120 readings**, including gate energy when standard output is enabled. Times are ESP uptime in milliseconds, not wall-clock timestamps. The ring buffer resets on reboot.

Writes are multiple sensor commands, not an atomic transaction. A failed write can leave some values changed; use Read and then restore the backup. The tool verifies all settings by reading them back after a successful write. UART commands/replies appear in the web diagnostics and USB console.

## Limits

This is a USB-powered bench tool with active Wi-Fi, not the final battery-optimized reader firmware. Restoring minimal sensor output does not disable ESP Wi-Fi.

The sensor's published absence delay is 10–120 s. It is coarse human-presence radar, not an NFC-range hand detector. Range gates are approximately 0.70 m steps. The dashboard preserves raw energy values without guessing undocumented scale conversions.

Targets the newer LD2410S UART protocol and your module's **1.1.1** firmware. Firmware is decoded from the trailing three 16-bit words for the observed reply lengths, with the full raw reply retained. Legacy firmware may require different threshold semantics.

The web UI is served over local HTTP. Keep the board on your local network; no internet exposure is needed. The AP uses WPA2, and API requests use a per-boot token. Set a different AP password in menuconfig if necessary.

## Developer checks

Portable protocol tests:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main main/protocol.c tests/test_protocol.c -o /tmp/ld2410s-test
/tmp/ld2410s-test
```

For the optional dashboard logic checks (Node.js 18+):

```sh
node tests/test_ui.js
```

The supplied ESP32-C3 firmware was compiled successfully with ESP-IDF 5.4.1 (no compiler warnings). The app occupies 0xca490 bytes, leaving 21% of its 1 MiB app partition free. The source dependency configuration now handles IDF 6.x using Espressif's documented cJSON migration; a full IDF 6.1 firmware build has not been verified here. Portable decoder tests passed with address/undefined-behavior sanitizers, and dashboard JavaScript was checked for syntax and its settings/job/calibration flows. A physical ESP32-C3 and LD2410S were not attached during development; your first flash is the hardware test.

The UART decoder handles minimal frames, standard frames, calibration progress and interleaved replies, including fragmented/noisy data. A truncated partial frame is discarded after 500 ms of serial inactivity. JSON parameter ranges are checked before jobs are queued.

References:

- [Hi-Link LD2410S UART protocol V1.00](https://h.hlktech.com/download/HLK-LD2410S-24G/1/HLK-LD2410S-%E4%B8%B2%E5%8F%A3%E9%80%9A%E4%BF%A1%E5%8D%8F%E8%AE%AE-V1.00%20.pdf)
- [ESP-IDF ESP32-C3 UART documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32c3/api-reference/peripherals/uart.html)
