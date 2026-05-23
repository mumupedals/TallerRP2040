# Hardware Reference

These sketches target the mumuTech RP2040 instrument hardware.

## Shared audio pins

The examples use I2S audio:

| Signal | RP2040 GPIO |
| --- | --- |
| BCLK | GP2 |
| LRCK / WS | GP3 |
| DIN | GP4 |

In the Earle Philhower RP2040 Arduino core, LRCK is `BCLK + 1`, so the sketches define BCLK as `2` and DIN as `4`.

## Pots

| Control | RP2040 GPIO / ADC |
| --- | --- |
| Pot 1 | GP26 / ADC0 |
| Pot 2 | GP27 / ADC1 |
| Pot 3 | GP28 / ADC2 |
| Pot 4 | GP29 / ADC3 |

## Buttons

Buttons are active low and use `INPUT_PULLUP`.

| Control | RP2040 GPIO |
| --- | --- |
| Button 1 | GP6 |
| Button 2 | GP7 |
| Button 3 | GP8 |
| Button 4 | GP9 |
| Button 5, DrumLoop only | GP20 |

## LED

| Signal | RP2040 GPIO |
| --- | --- |
| Status LED | GP14 |

## Example behavior

| Sketch | Controls |
| --- | --- |
| `FourVoiceGate` | Four pots set pitch. Four buttons gate four voices. |
| `DrumLoop` | Four pots shape drum parameters. Buttons trigger and edit drum behavior. Button 5 controls open hi-hat and boot helper behavior. |
| `Dronaldo` | Four pots control root, chord, inversion, and cutoff. Four buttons recall/save chord presets. |

If your hardware uses different pins, change the constants at the top of the sketch or in `config.h`.
