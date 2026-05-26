/*
  Dronaldo — Workshop Chord Drone for mumunator RP2040 hardware

  A continuous 5-voice Braids drone. Four buttons recall saved chords;
  four pots set root, chord quality, cutoff, and inversion. Close the cutoff pot
  fully to mute the output silently — useful for changing chords without
  any audible artifact, since voices never retrigger.

    BTN0 (GPIO6) → Preset 1         POT1 (GPIO26) → Root (quantized, 2 oct)
    BTN1 (GPIO7) → Preset 2         POT2 (GPIO27) → Chord quality (8 types)
    BTN2 (GPIO8) → Preset 3         POT3 (GPIO28) → Inversion (-6..+6)
    BTN3 (GPIO9) → Preset 4         POT4 (GPIO29) → Cutoff (normal) / Osc shape (save mode)

  In save mode POT4 selects one of 6 Braids oscillator shapes with soft
  takeover (the pot must reach the current shape's position before it
  takes control, preventing jumps on mode entry). Timbre and color are
  hardcoded per shape. Cutoff also re-arms with soft takeover on exit so
  the filter doesn't jump when POT4 is handed back.

  All audio and input runs on Core 0; Core 1 is unused.

  This sketch builds on Braids/STMLIB synthesis code by
  Émilie Gillet / Mutable Instruments.
*/

#include <Arduino.h>
#include <I2S.h>
#include <Adafruit_TinyUSB.h>   // required: board's USB stack is Adafruit TinyUSB
#include <STMLIB.h>
#include <BRAIDS.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "config.h"
#include "audio_engine.h"

// ============================================================================
// SOFT TAKEOVER
// ============================================================================

struct PotPickup {
  uint16_t target;
  bool     active;
};

static inline bool potPickupActive(PotPickup *pp, uint16_t adc) {
  if (pp->active) return true;
  int32_t diff = (int32_t)adc - (int32_t)pp->target;
  if (diff < 0) diff = -diff;
  if ((uint16_t)diff <= POT_PICKUP_THRESHOLD) pp->active = true;
  return pp->active;
}

// ============================================================================
// HARDWARE
// ============================================================================

I2S i2s(OUTPUT, I2S_BCLK_PIN, I2S_DIN_PIN);

static const uint8_t BTN_PINS[4] = { BTN0_PIN, BTN1_PIN, BTN2_PIN, BTN3_PIN };

// ============================================================================
// STATE
// ============================================================================

static DroneVoice voices[NUM_VOICES];

struct ChordPreset {
  uint8_t root_bin;
  uint8_t chord;
};

static ChordPreset button_presets[4];

static uint8_t current_chord     = 0;  // 0=MAJ7, 1=MIN7, 2=DOM7, 3=HDM7
static uint8_t current_root_bin  = 12; // 0..ROOT_SEMITONES-1 → MIDI offset
static int8_t  current_inversion = 0;

static uint8_t physical_chord    = 0;
static uint8_t physical_root_bin = 12;

static uint16_t pot_root_smoothed      = 0;
static uint16_t pot_chord_smoothed     = 0;
static uint16_t pot_cutoff_smoothed    = 0;
static uint16_t pot_inversion_smoothed = 0;

// Written by input scan, read by audio render (same core — no volatile needed)
static uint16_t filter_cutoff_q15 = 0;
static int16_t  current_timbre    = 16384;

static uint8_t   current_shape_idx  = 0;       // index into DRONE_SHAPES[]
static PotPickup shape_pot_pickup   = { 0, false };  // pot4 → shape in save mode
static PotPickup cutoff_pot_pickup  = { 0, true  };  // pot4 → cutoff in normal mode

// Runtime tunables (serial commands can adjust these; defaults from config.h)
static uint16_t current_resonance = FIXED_RESONANCE;
static uint16_t current_volume    = MASTER_VOLUME;

static bool save_mode = false;
static bool save_mode_ignore_until_release[4] = { false, false, false, false };

// Button debounce
static bool     btn_stable[4]   = { false, false, false, false };
static bool     btn_raw_prev[4] = { false, false, false, false };
static uint32_t btn_change_ms[4] = { 0, 0, 0, 0 };
static uint32_t btn_press_ms[4]  = { 0, 0, 0, 0 };
static int8_t   pending_recall_slot = -1;
static uint32_t pending_recall_ms = 0;

// ============================================================================
// AUDIO TIMER
// ============================================================================

static volatile uint32_t render_pending_blocks = 0;
static volatile uint32_t audio_missed_blocks   = 0;
static uint8_t  startup_declick_blocks         = STARTUP_DECLICK_BLOCKS;

static uint32_t alarm_period_us   = 0;
static uint32_t alarm_frac_us_num = 0;
static uint32_t alarm_frac_us_den = 1;
static uint32_t alarm_frac_accum  = 0;

static void audio_timer_callback() {
  timer_hw->intr = 1u << 0;
  uint32_t next = alarm_period_us;
  alarm_frac_accum += alarm_frac_us_num;
  if (alarm_frac_accum >= alarm_frac_us_den) {
    alarm_frac_accum -= alarm_frac_us_den;
    next++;
  }
  timer_hw->alarm[0] = timer_hw->timerawl + next;

  if (render_pending_blocks >= 4u) {
    audio_missed_blocks++;
  } else {
    render_pending_blocks++;
  }
}

// ============================================================================
// RENDER ONE AUDIO BLOCK
// ============================================================================

static void renderAudioBlock() {
  // Startup declick: write silence so the I2S DAC settles before the drone
  // comes in. Prevents a boot thump through the amp.
  if (startup_declick_blocks > 0) {
    startup_declick_blocks--;
    for (int i = 0; i < BLOCK_SIZE; i++) {
      i2s.write((int16_t)0);
      i2s.write((int16_t)0);
    }
    return;
  }

  const uint16_t cutoff = filter_cutoff_q15;

  for (uint8_t v = 0; v < NUM_VOICES; v++) {
    voiceRender(&voices[v], cutoff, current_resonance);
  }

  // Low-end silencer: at low cutoff the SVF integrators freeze, so scale the
  // mix by cutoff over the bottom CUTOFF_GATE_THRESHOLD slice. Above the
  // threshold, full volume — filter sweeps behave normally everywhere else.
  int32_t gate_q15;
  if (cutoff >= CUTOFF_GATE_THRESHOLD_Q15) {
    gate_q15 = 32767;
  } else {
    gate_q15 = (int32_t)cutoff * 32767 / CUTOFF_GATE_THRESHOLD_Q15;
  }

  int16_t out_buf[BLOCK_SIZE];
  for (int i = 0; i < BLOCK_SIZE; i++) {
    int32_t mix = 0;
    for (uint8_t v = 0; v < NUM_VOICES; v++) mix += (int32_t)voices[v].buffer[i];
    // ÷ NUM_VOICES as fixed-point multiply
    mix = (mix * (int32_t)VOICE_MIX_SCALE_Q16) >> 16;
    mix = (mix * gate_q15) >> 15;
    mix = (mix * (int32_t)current_volume) >> 15;
    if (mix > AUDIO_MAX) mix = AUDIO_MAX;
    if (mix < AUDIO_MIN) mix = AUDIO_MIN;
    out_buf[i] = (int16_t)mix;
  }

  for (int i = 0; i < BLOCK_SIZE; i++) {
    i2s.write(out_buf[i]);
    i2s.write(out_buf[i]);
  }
}

// ============================================================================
// INPUT HELPERS
// ============================================================================

// IIR low-pass: smoothed = smoothed - (smoothed >> shift) + (raw >> shift).
// Equivalent to  smoothed = smoothed * (2^s - 1) / 2^s  +  raw / 2^s.
static inline uint16_t potSmooth(uint16_t smoothed, uint16_t raw) {
  return (uint16_t)(smoothed - (smoothed >> POT_IIR_SHIFT) + (raw >> POT_IIR_SHIFT));
}

// Quantize a pot value (0..4095) to one of `num_bins` buckets, with a
// hysteresis zone around the current bucket to prevent boundary flicker.
static uint8_t quantizeHysteresis(uint16_t raw, uint8_t num_bins, uint8_t cur_bin) {
  const int32_t bin_w = 4096 / num_bins;
  const int32_t hys   = bin_w / 8;   // ~12.5% of bin width — plenty to kill jitter

  const int32_t stay_lo = (int32_t)cur_bin * bin_w - hys;
  const int32_t stay_hi = (int32_t)(cur_bin + 1) * bin_w + hys;

  if ((int32_t)raw >= stay_lo && (int32_t)raw < stay_hi) return cur_bin;

  int32_t new_bin = (int32_t)raw / bin_w;
  if (new_bin < 0) new_bin = 0;
  if (new_bin >= (int32_t)num_bins) new_bin = num_bins - 1;
  return (uint8_t)new_bin;
}

static void applyCurrentChord() {
  const uint8_t root_midi = ROOT_MIN_MIDI + current_root_bin;
  setDronePitches(voices, root_midi, current_chord, current_inversion);
}

static void syncCurrentChordToPots() {
  current_root_bin = physical_root_bin;
  current_chord = physical_chord;
  applyCurrentChord();
}

static void recallPreset(uint8_t slot) {
  if (slot >= 4) return;
  current_root_bin = button_presets[slot].root_bin;
  current_chord = button_presets[slot].chord;
  applyCurrentChord();
  #if ENABLE_SERIAL_LOG
    Serial.print("recall slot=");
    Serial.print(slot);
    Serial.print(" root=");
    Serial.print(current_root_bin);
    Serial.print(" chord=");
    Serial.println(current_chord);
  #endif
}

static void savePreset(uint8_t slot) {
  if (slot >= 4) return;
  button_presets[slot].root_bin = physical_root_bin;
  button_presets[slot].chord = physical_chord;
  current_root_bin = physical_root_bin;
  current_chord = physical_chord;
  applyCurrentChord();
  #if ENABLE_SERIAL_LOG
    Serial.print("save slot=");
    Serial.print(slot);
    Serial.print(" root=");
    Serial.print(physical_root_bin);
    Serial.print(" chord=");
    Serial.println(physical_chord);
  #endif
}

static void enterSaveMode() {
  save_mode = true;
  for (uint8_t i = 0; i < 4; i++) save_mode_ignore_until_release[i] = btn_stable[i];
  syncCurrentChordToPots();
  // Lock shape pot to the ADC bin centre for the current shape so the pot
  // must physically reach it before taking control (no snap on entry).
  shape_pot_pickup.target = (uint16_t)(((uint32_t)current_shape_idx * 4096u + 2048u)
                                       / (uint32_t)NUM_SHAPES);
  shape_pot_pickup.active = false;
  #if ENABLE_SERIAL_LOG
    Serial.println("save mode on");
  #endif
}

static void exitSaveMode() {
  save_mode = false;
  for (uint8_t i = 0; i < 4; i++) save_mode_ignore_until_release[i] = false;
  pending_recall_slot = -1;
  // Re-arm cutoff pickup so pot4 can't jump the filter on mode exit.
  cutoff_pot_pickup.target = (uint16_t)(filter_cutoff_q15 >> 3);
  cutoff_pot_pickup.active = false;
  #if ENABLE_SERIAL_LOG
    Serial.println("save mode off");
  #endif
}

// ============================================================================
// INPUT SCAN
// ============================================================================

static void scanPots() {
  pot_root_smoothed      = potSmooth(pot_root_smoothed,      analogRead(POT_ROOT_PIN));
  pot_chord_smoothed     = potSmooth(pot_chord_smoothed,     analogRead(POT_CHORD_PIN));
  pot_cutoff_smoothed    = potSmooth(pot_cutoff_smoothed,    analogRead(POT_CUTOFF_PIN));
  pot_inversion_smoothed = potSmooth(pot_inversion_smoothed, analogRead(POT_INVERSION_PIN));

  // ---- Root (quantized to 24 semitones, 2 octaves) ----
  const uint8_t new_root_bin = quantizeHysteresis(pot_root_smoothed,
                                                  ROOT_SEMITONES, physical_root_bin);
  bool pitch_dirty = false;
  if (new_root_bin != physical_root_bin) {
    physical_root_bin = new_root_bin;
    current_root_bin = physical_root_bin;
    pitch_dirty = true;
  }

  const uint8_t new_chord = quantizeHysteresis(pot_chord_smoothed,
                                               NUM_CHORDS, physical_chord);
  if (new_chord != physical_chord) {
    physical_chord = new_chord;
    current_chord = physical_chord;
    pitch_dirty = true;
  }

  // ---- Inversion (-6..+6, 13 bins) ----
  const uint8_t new_inv_bin = quantizeHysteresis(pot_inversion_smoothed,
                                                 NUM_INVERSION_BINS,
                                                 (uint8_t)(current_inversion - MIN_INVERSION));
  const int8_t new_inv = (int8_t)new_inv_bin + MIN_INVERSION;
  if (new_inv != current_inversion) {
    current_inversion = new_inv;
    pitch_dirty = true;
  }

  if (pitch_dirty) {
    applyCurrentChord();
  }

  // ---- Pot 4: oscillator shape (save mode) or cutoff (normal mode) ----
  if (save_mode) {
    if (potPickupActive(&shape_pot_pickup, pot_cutoff_smoothed)) {
      const uint8_t new_shape = quantizeHysteresis(pot_cutoff_smoothed,
                                                   NUM_SHAPES, current_shape_idx);
      if (new_shape != current_shape_idx) {
        current_shape_idx = new_shape;
        const ShapeConfig& sc = DRONE_SHAPES[current_shape_idx];
        setDroneShape(voices, sc.shape, sc.timbre, sc.color);
        #if ENABLE_SERIAL_LOG
          Serial.print("shape=");
          Serial.println(current_shape_idx);
        #endif
      }
    }
  } else {
    if (potPickupActive(&cutoff_pot_pickup, pot_cutoff_smoothed)) {
      filter_cutoff_q15 = pot_cutoff_smoothed << 3;
    }
  }
}

static void scanButtons(uint32_t now_ms) {
  bool press_edge[4] = { false, false, false, false };

  for (uint8_t i = 0; i < 4; i++) {
    const bool raw = (digitalRead(BTN_PINS[i]) == LOW);
    if (raw != btn_raw_prev[i]) {
      btn_raw_prev[i]  = raw;
      btn_change_ms[i] = now_ms;
    }
    if ((now_ms - btn_change_ms[i]) >= BTN_DEBOUNCE_MS && raw != btn_stable[i]) {
      btn_stable[i] = raw;
      if (raw) {
        btn_press_ms[i] = now_ms;
        press_edge[i] = true;
      } else {
        save_mode_ignore_until_release[i] = false;
      }
    }
  }

  uint8_t pressed_count = 0;
  bool all_pressed = true;
  for (uint8_t i = 0; i < 4; i++) {
    if (btn_stable[i]) {
      pressed_count++;
    } else {
      all_pressed = false;
    }
  }

  if (!save_mode && all_pressed) {
    pending_recall_slot = -1;
    enterSaveMode();
    return;
  }

  if (save_mode) {
    pending_recall_slot = -1;
    for (uint8_t i = 0; i < 4; i++) {
      if (press_edge[i] && !save_mode_ignore_until_release[i]) savePreset(i);
    }
    for (uint8_t i = 0; i < 4; i++) {
      if (btn_stable[i] &&
          !save_mode_ignore_until_release[i] &&
          (now_ms - btn_press_ms[i]) >= SAVE_MODE_EXIT_HOLD_MS) {
        exitSaveMode();
        return;
      }
    }
    return;
  }

  if (pressed_count > 1) {
    pending_recall_slot = -1;
    return;
  }

  if (pending_recall_slot >= 0) {
    if (pressed_count == 0) {
      recallPreset((uint8_t)pending_recall_slot);
      pending_recall_slot = -1;
    } else if ((now_ms - pending_recall_ms) >= BUTTON_COMBO_WINDOW_MS) {
      recallPreset((uint8_t)pending_recall_slot);
      pending_recall_slot = -1;
    }
  }

  if (pending_recall_slot < 0 && pressed_count == 1) {
    for (uint8_t i = 0; i < 4; i++) {
      if (press_edge[i]) {
        pending_recall_slot = (int8_t)i;
        pending_recall_ms = now_ms;
        break;
      }
    }
  }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  set_sys_clock_khz(250000, true);

  // ---- Inputs ----
  analogReadResolution(12);
  pinMode(BTN0_PIN, INPUT_PULLUP);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);

  // BOOTSEL back door: hold the first and last buttons while plugging in USB
  // to jump straight to the UF2 drag-and-drop bootloader.
  if (digitalRead(BTN0_PIN) == LOW && digitalRead(BTN3_PIN) == LOW) {
    reset_usb_boot(0, 0);
  }

  // TinyUSB must be initialized before Serial (CDC) is used.
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);

  #if ENABLE_SERIAL_LOG
    Serial.begin(115200);
    delay(500);
    Serial.println("=== DRONALDO BOOT ===");
  #endif

  // Prime smoothed pot values so the drone starts at the physical pot
  // positions rather than sweeping up from zero on first scan.
  pot_root_smoothed      = analogRead(POT_ROOT_PIN);
  pot_chord_smoothed     = analogRead(POT_CHORD_PIN);
  pot_cutoff_smoothed    = analogRead(POT_CUTOFF_PIN);
  pot_inversion_smoothed = analogRead(POT_INVERSION_PIN);

  physical_root_bin = pot_root_smoothed * ROOT_SEMITONES / 4096;
  if (physical_root_bin >= ROOT_SEMITONES) physical_root_bin = ROOT_SEMITONES - 1;
  current_root_bin  = physical_root_bin;
  physical_chord    = pot_chord_smoothed * NUM_CHORDS / 4096;
  if (physical_chord >= NUM_CHORDS) physical_chord = NUM_CHORDS - 1;
  current_chord     = physical_chord;
  if (current_root_bin >= ROOT_SEMITONES) current_root_bin = ROOT_SEMITONES - 1;
  current_inversion = (int8_t)(pot_inversion_smoothed * NUM_INVERSION_BINS / 4096) + MIN_INVERSION;
  if (current_inversion > MAX_INVERSION) current_inversion = MAX_INVERSION;
  current_timbre    = 16384;
  filter_cutoff_q15 = pot_cutoff_smoothed << 3;
  cutoff_pot_pickup.target = pot_cutoff_smoothed;
  cutoff_pot_pickup.active = true;

  // ---- Startup presets ----
  // 1: D#m7add9,  2: F#m7add9,  3: Emaj7,  4: D# / G# / C# / F# / G#
  button_presets[0] = { 15, 10 }; // D#4 = 63, minor 9
  button_presets[1] = { 18, 10 }; // F#4 = 66, minor 9
  button_presets[2] = { 16, 0 };  // E4  = 64, major 7
  button_presets[3] = { 15, 12 }; // D#4 custom voicing

  // Start on preset 0
  current_root_bin = button_presets[0].root_bin;
  current_chord    = button_presets[0].chord;

  // ---- Voices ----
  const uint8_t root_midi    = ROOT_MIN_MIDI + current_root_bin;
  const int16_t default_pitch = (int16_t)(root_midi << 7);
  for (uint8_t i = 0; i < NUM_VOICES; i++) {
    voiceInit(&voices[i], default_pitch, current_timbre);
  }
  applyCurrentChord();

  #if ENABLE_SERIAL_LOG
    Serial.println("[1] voices ok");
  #endif

  // ---- I2S ----
  i2s.setBitsPerSample(16);
  bool i2s_ok = false;
  for (uint8_t r = 0; r < I2S_INIT_MAX_RETRIES; r++) {
    if (i2s.begin(SAMPLE_RATE)) { i2s_ok = true; break; }
    delay(I2S_INIT_RETRY_DELAY_MS);
  }
  #if ENABLE_SERIAL_LOG
    Serial.println(i2s_ok ? "[2] i2s ok" : "[2] i2s FAIL");
  #endif

  // ---- Audio timer ----
  hw_set_bits(&timer_hw->inte, 1u << 0);
  irq_set_exclusive_handler(TIMER_IRQ_0, audio_timer_callback);
  irq_set_enabled(TIMER_IRQ_0, true);
  alarm_period_us   = (MICROSECONDS_PER_SECOND * (uint32_t)BLOCK_SIZE) / SAMPLE_RATE;
  alarm_frac_us_num = (MICROSECONDS_PER_SECOND * (uint32_t)BLOCK_SIZE) % SAMPLE_RATE;
  alarm_frac_us_den = SAMPLE_RATE;
  alarm_frac_accum  = 0;
  timer_hw->intr    = 1u << 0;
  timer_hw->alarm[0] = timer_hw->timerawl + alarm_period_us;

  #if ENABLE_SERIAL_LOG
    Serial.println("=== BOOT COMPLETE ===");
  #endif
}

// ============================================================================
// SERIAL COMMANDS (adapted from DrumLoop)
// ============================================================================
// Commands (newline-terminated):
//   0-4 pitch <value>     -- voice pitch (MIDI note * 128)
//   0-4 timbre <value>    -- voice timbre (0..32767)
//   0-4 color <value>     -- voice color (0..32767)
//   shape <idx>           -- set oscillator shape (0..5)
//   cutoff <value>        -- filter cutoff (0..32767)
//   res <value>           -- filter resonance (0..32767)
//   vol <value>           -- master volume (0..32767)
//   dump                  -- print all voice params
//   help                  -- show command summary
//
static void serviceSerialCommands() {
  static char cmd[64];
  static uint8_t cmd_len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmd[cmd_len] = '\0';
      cmd_len = 0;

      char tok0[8] = {};
      char tok1[8] = {};
      char tok2[32] = {};
      int n = sscanf(cmd, "%7s %7s %31s", tok0, tok1, tok2);

      // Voice command: first token is a digit 0-4
      if (tok0[0] >= '0' && tok0[0] <= '4' && tok0[1] == '\0' && n == 3) {
        uint8_t vidx = (uint8_t)(tok0[0] - '0');
        DroneVoice* v = &voices[vidx];
        int32_t value = (int32_t)strtol(tok2, nullptr, 0);
        if (strcmp(tok1, "pitch") == 0) {
          v->pitch = (int16_t)value;
          v->applied_pitch = (int16_t)value; // prevent re-render fight
        }
        else if (strcmp(tok1, "timbre") == 0) v->timbre = (int16_t)value;
        else if (strcmp(tok1, "color") == 0)  v->color  = (int16_t)value;
        else {
          Serial.println("Unknown param. Use: pitch, timbre, color");
          continue;
        }
        Serial.print("OK: v"); Serial.print(vidx); Serial.print(" ");
        Serial.print(tok1); Serial.print(" = "); Serial.println(value);
      }
      // Global commands: 2 tokens (cmd + value)
      else if (n == 2) {
        int32_t value = (int32_t)strtol(tok1, nullptr, 0);
        if (strcmp(tok0, "shape") == 0) {
          if (value >= 0 && value < NUM_SHAPES) {
            current_shape_idx = (uint8_t)value;
            const ShapeConfig& sc = DRONE_SHAPES[current_shape_idx];
            setDroneShape(voices, sc.shape, sc.timbre, sc.color);
            Serial.print("OK: shape = "); Serial.println(current_shape_idx);
          } else {
            Serial.println("Shape index out of range (0..5)");
          }
        }
        else if (strcmp(tok0, "cutoff") == 0) {
          filter_cutoff_q15 = (uint16_t)constrain(value, 0, 32767);
          cutoff_pot_pickup.active = true; // pot takeover disabled until moved
          Serial.print("OK: cutoff = "); Serial.println(filter_cutoff_q15);
        }
        else if (strcmp(tok0, "res") == 0 || strcmp(tok0, "resonance") == 0) {
          current_resonance = (uint16_t)constrain(value, 0, 32767);
          Serial.print("OK: resonance = "); Serial.println(current_resonance);
        }
        else if (strcmp(tok0, "vol") == 0 || strcmp(tok0, "volume") == 0) {
          current_volume = (uint16_t)constrain(value, 0, 32767);
          Serial.print("OK: volume = "); Serial.println(current_volume);
        }
        else {
          Serial.println("Unknown command. Try: help");
        }
      }
      else if (strcmp(tok0, "dump") == 0) {
        Serial.println("// === DRONALDO CURRENT STATE ===");
        Serial.print("// shape="); Serial.print(current_shape_idx);
        Serial.print(" cutoff="); Serial.print(filter_cutoff_q15);
        Serial.print(" res="); Serial.print(current_resonance);
        Serial.print(" vol="); Serial.println(current_volume);
        for (uint8_t i = 0; i < NUM_VOICES; i++) {
          Serial.print("// Voice "); Serial.println(i);
          Serial.print("voices["); Serial.print(i); Serial.print("].pitch = "); Serial.print(voices[i].pitch); Serial.println(";");
          Serial.print("voices["); Serial.print(i); Serial.print("].timbre = "); Serial.print(voices[i].timbre); Serial.println(";");
          Serial.print("voices["); Serial.print(i); Serial.print("].color = "); Serial.print(voices[i].color); Serial.println(";");
        }
      }
      else if (strcmp(tok0, "help") == 0 || strcmp(tok0, "?") == 0) {
        Serial.println("Commands:");
        Serial.println("  <0-4> pitch <v>    -- voice pitch (MIDI*128)");
        Serial.println("  <0-4> timbre <v>   -- voice timbre (0..32767)");
        Serial.println("  <0-4> color <v>    -- voice color (0..32767)");
        Serial.println("  shape <0-5>        -- oscillator shape");
        Serial.println("  cutoff <0..32767>  -- filter cutoff");
        Serial.println("  res <0..32767>     -- filter resonance");
        Serial.println("  vol <0..32767>     -- master volume");
        Serial.println("  dump               -- print current state");
        Serial.println("  help               -- this message");
      }
      else if (cmd_len > 1) {
        Serial.println("Bad command. Try: 0 timbre 20000  or  cutoff 20000  or  help");
      }
    } else if (cmd_len < sizeof(cmd)-1) {
      cmd[cmd_len++] = c;
    }
  }
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
  // Audio always first — drain any pending blocks before touching inputs.
  constexpr uint8_t MAX_RENDERS_PER_LOOP = 4;
  uint8_t renders = 0;
  while (render_pending_blocks > 0 && renders < MAX_RENDERS_PER_LOOP) {
    noInterrupts(); render_pending_blocks--; interrupts();
    renderAudioBlock();
    renders++;
  }

  // Serial commands (non-blocking)
  serviceSerialCommands();

  // Input scan — throttled. The IIR filter needs many samples to converge,
  // but we don't need to poll at audio rate.
  const uint32_t now_ms = millis();
  static uint32_t last_scan_ms = 0;
  if ((now_ms - last_scan_ms) >= INPUT_SCAN_INTERVAL_MS) {
    last_scan_ms = now_ms;
    scanPots();
    scanButtons(now_ms);
  }

  #if ENABLE_SERIAL_LOG
    static uint32_t last_dropout_ms = 0;
    static uint32_t last_dropout_snap = 0;
    if ((now_ms - last_dropout_ms) >= 1000u) {
      last_dropout_ms = now_ms;
      const uint32_t missed = audio_missed_blocks;
      if (missed != last_dropout_snap) {
        Serial.print("DROPOUT missed=");
        Serial.println(missed);
        last_dropout_snap = missed;
      }
    }
  #endif
}
