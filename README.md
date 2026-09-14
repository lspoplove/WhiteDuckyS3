# ESP32-S3 N16R8 Wi-Fi DuckyScript Manager

This project combines:

- ESP32-S3 native USB HID keyboard support
- A DuckyScript 3.x core interpreter
- Wi-Fi access-point mode
- A browser-based `payload.txt` file manager
- LittleFS storage in internal Flash
- A WS2812B status LED connected to GPIO21

Use this project only on systems you own or are explicitly authorized to test.

## Included Files

- `ESP32S3_Ducky_WiFi_LittleFS_WS2812B_IO21_English.ino`
- `partitions.csv`
- `payload.txt` — harmless test payload
- `README.md`

## Hardware

Target module: ESP32-S3 N16R8

- N16: 16 MB internal Flash
- R8: 8 MB OPI PSRAM
- WS2812B data input: GPIO21
- Native USB D-: GPIO19
- Native USB D+: GPIO20

The persistent `payload.txt` file is stored in the N16 Flash using LittleFS. PSRAM is runtime memory and is not used as permanent file storage.

## Recommended Arduino IDE Settings

- Board: `ESP32S3 Dev Module`
- Flash Size: `16MB (128Mb)`
- PSRAM: `OPI PSRAM`
- USB Mode: `USB-OTG (TinyUSB)`
- USB CDC On Boot: `Enabled` if serial logs are needed
- Partition Scheme: choose an option with enough application space to compile; the `partitions.csv` file in the sketch folder defines the final layout

Keep `partitions.csv` in the same folder as the `.ino` file.

## Custom Partition Layout

The supplied partition table provides approximately:

- NVS: 20 KB
- Application partition: 4 MB
- LittleFS partition: 11.94 MiB

Firmware OTA is not configured in this layout. Supporting firmware OTA later will require a different partition table with OTA application slots and OTA data.

The current interpreter limits a single payload to 128 KiB because the complete script is loaded into memory before parsing:

```cpp
static const size_t MAX_PAYLOAD_BYTES = 128 * 1024;
```

## First Flash

When installing this custom partition table for the first time, use:

- `Erase All Flash Before Sketch Upload: Enabled`

After the first successful installation, use the following setting when you want firmware uploads to preserve the stored `payload.txt` file:

- `Erase All Flash Before Sketch Upload: Disabled`

Changing the partition table or erasing all Flash can remove stored files.

## Wi-Fi Configuration

The device creates its own Wi-Fi access point after boot.

Default network format:

```text
DSTIKE-DUCKY-XXXXXX
```

Default password:

```text
ChangeMe123!
```

Change the password near the top of the sketch before shipping or regular use:

```cpp
static const char *AP_PASSWORD = "ChangeMe123!";
```

The password must contain at least eight characters.

## Web Interface

1. Compile and flash the project.
2. Connect a phone or computer to the `DSTIKE-DUCKY-XXXXXX` Wi-Fi network.
3. Open the following address in a browser:

```text
http://192.168.4.1
```

The web interface can:

- Upload or replace `payload.txt`
- View the current payload
- Download the current payload
- Delete the current payload
- Run the payload manually
- Show LittleFS capacity and usage
- Show the number of connected Wi-Fi clients
- Show the current WS2812B status

Uploads are written to Flash in chunks. The existing payload is backed up while the replacement file is installed, reducing the chance of losing a working payload after an interrupted upload.

## Execution Settings

The safe defaults are:

```cpp
static const bool AUTO_RUN_AFTER_UPLOAD = false;
static const bool AUTO_RUN_ON_BOOT = false;
```

With these defaults, uploading a file does not execute it. Use the **Run Manually** button on the web interface.

To execute automatically after a successful upload:

```cpp
static const bool AUTO_RUN_AFTER_UPLOAD = true;
```

To execute the stored payload automatically after every boot:

```cpp
static const bool AUTO_RUN_ON_BOOT = true;
```

A physical confirmation button is recommended for a finished product.

## WS2812B Status LED

The WS2812B data input is connected to GPIO21.

- Red: the current payload has not been executed, the device has just rebooted, a new payload has been uploaded, the payload has been deleted, or loading failed
- Green: the **Run Manually** button has been clicked and execution has been triggered; it remains green after successful completion

Default brightness:

```cpp
static const uint8_t STATUS_LED_BRIGHTNESS = 48;
```

Valid range: `0` to `255`.

Default color order:

```cpp
LED_COLOR_ORDER_GRB
```

Most WS2812B LEDs use GRB. If the displayed colors are incorrect, try another order such as `LED_COLOR_ORDER_RGB`.

This project uses the `esp32-hal-rgb-led.h` component included with Arduino-ESP32 3.x. No separate Adafruit NeoPixel library is required.

## USB HID Connection

Keyboard HID output must use the ESP32-S3 native USB interface on GPIO19 and GPIO20. A CH340, CP2102, or other USB-to-UART connector cannot replace the native USB HID connection.

The sketch uses:

```cpp
Keyboard.begin(KeyboardLayout_en_US);
USB.begin();
```

The target computer should use the US English keyboard layout. Otherwise, some punctuation and symbol keys may not match.

## Example Payload

The included `payload.txt` is a harmless test:

```text
REM Authorized local test only
DELAY 1000
STRINGLN ESP32-S3 payload.txt loaded from LittleFS
```

## Important Notes

- The web interface uses the Wi-Fi password as its primary access control and does not currently include a separate web login.
- Use a unique password per device for commercial products.
- Running a payload temporarily blocks the simple web-server loop, so the page may be unavailable until the script completes.
- Use keyboard automation only in an authorized environment.
