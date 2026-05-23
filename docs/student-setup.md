# Student Setup

These steps install everything needed to compile and upload the workshop instruments.

## 1. Accounts and tools

Install:

- GitHub account: https://github.com/
- Git: https://git-scm.com/downloads
- Arduino IDE 2: https://www.arduino.cc/en/software

Optional, but useful:

- GitHub Desktop: https://desktop.github.com/
- Visual Studio Code: https://code.visualstudio.com/

## 2. Add RP2040 board support

In Arduino IDE:

1. Open `File > Preferences`.
2. Find `Additional Boards Manager URLs`.
3. Add this URL:

```text
https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
```

4. Open `Tools > Board > Boards Manager`.
5. Search for `pico`.
6. Install `Raspberry Pi Pico/RP2040/RP2350` by Earle F. Philhower.

## 3. Select the board

In Arduino IDE:

1. Open `Tools > Board`.
2. Select `Raspberry Pi Pico/RP2040/RP2350 > Raspberry Pi Pico`.
3. Set `Tools > USB Stack` to `Adafruit TinyUSB`.
4. Leave `Upload Method` as `Default (UF2)`.

The important board settings are:

```text
Board: Raspberry Pi Pico
Core: Raspberry Pi Pico/RP2040/RP2350
USB Stack: Adafruit TinyUSB
Upload Method: Default (UF2)
```

## 4. Install libraries

Open `Sketch > Include Library > Manage Libraries...` and install:

- `Adafruit TinyUSB Library`
- `MIDI Library`

For `Dronaldo` and `DrumLoop`, this repository includes:

- `BRAIDS`
- `STMLIB`

Copy these two folders from the repository:

```text
libraries/BRAIDS/
libraries/STMLIB/
```

into your Arduino libraries folder.

Typical Windows location:

```text
Documents/Arduino/libraries/
```

The folders should look like:

```text
Documents/Arduino/libraries/BRAIDS/
Documents/Arduino/libraries/STMLIB/
```

Restart Arduino IDE after copying the folders.

## 5. Get this repository

Option A: Download ZIP

1. On GitHub, click `Code`.
2. Click `Download ZIP`.
3. Unzip it.
4. Open an `.ino` file from one of the example folders.

Option B: Use Git

```bash
git clone REPLACE_WITH_REPOSITORY_URL
cd REPLACE_WITH_REPOSITORY_FOLDER
```

## 6. Upload a sketch

1. Connect the instrument by USB.
2. Open an example folder, such as `FourVoiceGate/FourVoiceGate.ino`.
3. Click `Verify`.
4. Click `Upload`.

If upload fails, hold the Pico `BOOTSEL` button while plugging in USB, then upload again. The board should appear as a USB drive or upload target.

## 7. Command-line compile, optional

If you use `arduino-cli`, compile with:

```bash
arduino-cli compile --fqbn rp2040:rp2040:rpipico:usbstack=tinyusb FourVoiceGate
arduino-cli compile --fqbn rp2040:rp2040:rpipico:usbstack=tinyusb DrumLoop
arduino-cli compile --fqbn rp2040:rp2040:rpipico:usbstack=tinyusb Dronaldo
```
