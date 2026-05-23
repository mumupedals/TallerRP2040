# mumuTech RP2040 Instrument Workshop

Workshop code and student instruments for the mumuTech RP2040 audio hardware.

This repository contains the example instruments from the workshop and a place for students to upload their own sketches.

## Included instruments

- `FourVoiceGate/` - four gated triangle voices, one pot and one button per voice.
- `DrumLoop/` - drum machine sketch with USB MIDI clock support.
- `Dronaldo/` - chord drone using Braids oscillators.

## Start here

1. Install the required software: [English](docs/student-setup.md) / [Español](docs/student-setup-es.md)
2. Check the hardware pin map: [docs/hardware-reference.md](docs/hardware-reference.md)
3. Open one example folder in Arduino IDE.
4. Select `Raspberry Pi Pico` and `Adafruit TinyUSB`.
5. Verify, upload, change the code, and make sound.

## What students should download

- GitHub account: https://github.com/
- Git: https://git-scm.com/downloads
- Arduino IDE 2: https://www.arduino.cc/en/software
- Earle Philhower RP2040 board package for Arduino:
  `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
- Arduino libraries through Library Manager:
  - `Adafruit TinyUSB Library`
  - `MIDI Library`
- Bundled Mutable Instruments audio libraries from this repo:
  - `libraries/BRAIDS`
  - `libraries/STMLIB`

Detailed setup steps are in [English](docs/student-setup.md) and [Español](docs/student-setup-es.md).

## Uploading student instruments

Students should add their work inside `submissions/` using this pattern:

```text
submissions/
  student-name-instrument-name/
    student-name-instrument-name.ino
    README.md
```

See [submissions/README.md](submissions/README.md) for the submission format and [docs/uploading-code.md](docs/uploading-code.md) for the GitHub workflow.

## Recommended GitHub workflow

- Keep `main` clean and working.
- Students make a fork or branch.
- Each student adds one folder under `submissions/`.
- Students open a pull request.
- You review, test, and merge.

This keeps everyone's instruments visible while protecting the workshop examples from accidental changes.

## Maintainer notes

To publish this workshop as a GitHub repository, follow [docs/maintainer-publishing.md](docs/maintainer-publishing.md).

Before publishing, add:

- A clear repository description.
- Photos or a diagram of the hardware.
- A license, if you want students to reuse/remix the code openly.
- Any extra notes about the bundled third-party `BRAIDS` and `STMLIB` libraries.
