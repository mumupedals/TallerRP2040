#pragma once

#include <Arduino.h>
#include <BRAIDS.h>

// ============================================================================
// DRONALDO — Workshop Chord Drone
// ============================================================================
//
// Stripped-down firmware for the mumunator RP2040 hardware. No display, no
// encoder, no sequencer. Just four buttons recalling chord presets and four
// pots shaping a continuous 6-voice drone.
//
//   BTN0 → Preset 1
//   BTN1 → Preset 2
//   BTN2 → Preset 3
//   BTN3 → Preset 4
//
//   POT1 → Root       (quantized to semitones, 2-octave range)
//   POT2 → Chord      (16 chord qualities)
//   POT3 → Inversion  (-6..+6, center detent = 0)
//   POT4 → Cutoff     (LP filter — full counterclockwise = silence)
// ============================================================================

// ---- Audio ----
#define SAMPLE_RATE     48000
#define BLOCK_SIZE      24      // must equal Braids kAudioBlockSize

constexpr int32_t AUDIO_MAX =  32767;
constexpr int32_t AUDIO_MIN = -32768;

// ---- I2S pins ----
#define I2S_BCLK_PIN    2       // LRCK is BCLK+1 = 3 (Arduino-Pico default)
#define I2S_DIN_PIN     4

// ---- Button pins (active LOW, INPUT_PULLUP) ----
#define BTN0_PIN        6       // Preset 1
#define BTN1_PIN        7       // Preset 2
#define BTN2_PIN        8       // Preset 3
#define BTN3_PIN        9       // Preset 4

// ---- Pot pins (RP2040 ADC inputs) ----
#define POT_ROOT_PIN       26
#define POT_CHORD_PIN      27
#define POT_INVERSION_PIN  28
#define POT_CUTOFF_PIN     29

// ---- Chord intervals ----
constexpr uint8_t NUM_CHORDS = 16;
constexpr uint8_t CHORD_INTERVAL_COUNT = 5;
constexpr int8_t  CHORD_INTERVALS[NUM_CHORDS][CHORD_INTERVAL_COUNT] = {
  { 0, 4, 7, 11, -1 }, // 0 = MAJOR 7
  { 0, 3, 7, 10, -1 }, // 1 = MINOR 7
  { 0, 4, 7, 10, -1 }, // 2 = DOMINANT 7
  { 0, 3, 6, 10, -1 }, // 3 = HALF-DIMINISHED 7 (m7b5)
  { 0, 4, 7, -1, -1 }, // 4 = MAJOR TRIAD
  { 0, 3, 7, -1, -1 }, // 5 = MINOR TRIAD
  { 0, 4, 8, -1, -1 }, // 6 = AUGMENTED TRIAD
  { 0, 3, 6, -1, -1 }, // 7 = DIMINISHED TRIAD
  { 0, 3, 6,  9, -1 }, // 8 = DIMINISHED 7 (fully diminished, 4 minor thirds stacked)
  { 0, 4, 7, 11, 14 }, // 9 = MAJOR 9
  { 0, 3, 7, 10, 14 }, // 10 = MINOR 9
  { 0, 4, 7, 10, 14 }, // 11 = DOMINANT 9
  { 5, 0, 10, 15, 17 }, // 12 = D# / G# / C# / F# / G# voicing
  { 0, 5, 7, 10, -1 }, // 13 = DOMINANT 7 SUS4
  { 0, 4, 7,  9, -1 }, // 14 = MAJOR 6
  { 0, 3, 7,  9, -1 }, // 15 = MINOR 6
};

// ---- Voice layout ----
// Up to 5 chord notes + one octave doubling for body.
constexpr uint8_t NUM_VOICES = 6;
constexpr uint16_t VOICE_MIX_SCALE_Q16 = 65536UL / NUM_VOICES;

// ---- Root pot range ----
// Two octaves centered on C4 → C3..B4 (MIDI 48..71).
constexpr uint8_t  ROOT_MIN_MIDI  = 48;
constexpr uint8_t  ROOT_SEMITONES = 24;

// ---- Inversion range (symmetric) ----
constexpr int8_t   MIN_INVERSION = -6;
constexpr int8_t   MAX_INVERSION =  6;
constexpr uint8_t  NUM_INVERSION_BINS = (MAX_INVERSION - MIN_INVERSION + 1);  // 13

// ---- Oscillator shape palette (pot 4 selects in save mode) ----
// Timbre and color are hardcoded per shape; they are applied whenever the
// shape changes and are never exposed to a pot.
struct ShapeConfig {
  braids::MacroOscillatorShape shape;
  int16_t timbre;
  int16_t color;
};

constexpr uint8_t NUM_SHAPES = 6;

static const ShapeConfig DRONE_SHAPES[NUM_SHAPES] = {
  { braids::MACRO_OSC_SHAPE_WAVETABLES,   16384, 16384 },  // 0 = Wavetable (default)
  { braids::MACRO_OSC_SHAPE_BOWED,        22000, 10000 },  // 1 = Bowed string
  { braids::MACRO_OSC_SHAPE_FEEDBACK_FM,  20000,  6000 },  // 2 = FM feedback
  { braids::MACRO_OSC_SHAPE_SAW_SWARM,    18000, 24000 },  // 3 = Saw swarm
  { braids::MACRO_OSC_SHAPE_HARMONICS,    14000, 20000 },  // 4 = Harmonics
  { braids::MACRO_OSC_SHAPE_VOWEL,        24000, 12000 },  // 5 = Vowel / formant
};

// ---- Fixed Braids timbral defaults (used only on first init) ----
constexpr uint16_t FIXED_RESONANCE = 800;
constexpr uint16_t MASTER_VOLUME   = 12000;

// ---- Soft takeover threshold (ADC units, ~2 % of 12-bit range) ----
constexpr uint16_t POT_PICKUP_THRESHOLD = 80;

// ---- Cutoff gate ----
// SVF lp/bp integrators freeze at F=0, which would leave a stuck DC offset
// audible at cutoff_q15 = 0. Scale the mix by cutoff over the bottom slice
// of the pot so the full-counterclockwise position is true silence.
constexpr uint16_t CUTOFF_GATE_THRESHOLD_Q15 = 3000;

// ---- Input handling ----
constexpr uint8_t  POT_IIR_SHIFT   = 3;    // smoothed = (smoothed*7 + raw) / 8
constexpr uint8_t  BTN_DEBOUNCE_MS = 5;
constexpr uint32_t INPUT_SCAN_INTERVAL_MS = 2;
constexpr uint32_t BUTTON_COMBO_WINDOW_MS = 120;
constexpr uint32_t SAVE_MODE_EXIT_HOLD_MS = 3000;

// ---- Startup declick ----
constexpr uint8_t  STARTUP_DECLICK_BLOCKS = 8;

// ---- Timing ----
constexpr uint32_t MICROSECONDS_PER_SECOND = 1000000UL;
constexpr uint8_t  I2S_INIT_MAX_RETRIES    = 3;
constexpr uint32_t I2S_INIT_RETRY_DELAY_MS = 100;

// ---- Diagnostics ----
#define ENABLE_SERIAL_LOG 1
