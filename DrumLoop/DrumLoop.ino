/*
  This workshop sketch was generated with AI assistance.
  Please read it critically, test it, and question every design choice.
  If something seems confusing, surprising, or wrong, that is a good reason to inspect the code and improve it.
*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <I2S.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"   // reset_usb_boot() for the BOOTSEL back door
#include "workshop_audio.h"

constexpr uint8_t I2S_BCLK_PIN = 2;
constexpr uint8_t I2S_DIN_PIN = 4;

constexpr uint8_t PIN_POT_0 = 26;
constexpr uint8_t PIN_POT_1 = 27;
constexpr uint8_t PIN_POT_2 = 28;
constexpr uint8_t PIN_POT_3 = 29;

constexpr uint8_t PIN_STATUS_LED = 14;  // change to your LED pin

constexpr uint8_t PIN_BTN_0 = 6;
constexpr uint8_t PIN_BTN_1 = 7;
constexpr uint8_t PIN_BTN_2 = 8;
constexpr uint8_t PIN_BTN_3 = 9;
constexpr uint8_t PIN_BTN_4 = 20;  // open hi-hat
constexpr uint8_t BUTTON_COUNT = 5;

constexpr uint16_t POT_MAX = 4095;
constexpr uint16_t POT_PICKUP_THRESHOLD = 80;  // ~2% of 12-bit range
constexpr uint8_t STEP_COUNT_MAX = 32;
constexpr uint16_t BPM_MIN = 60;
constexpr uint16_t BPM_MAX = 180;
constexpr uint8_t SWING_MAX = 50;
constexpr uint8_t MIDI_CLOCKS_PER_STEP = 6;
constexpr uint32_t MIDI_CLOCK_TIMEOUT_MS = 500;
constexpr uint32_t SHORT_PRESS_MIN_MS = 20;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr uint32_t CLEAR_HOLD_MS = 1000;
constexpr uint8_t HAT_CLOSED_LOCAL_CC = 40;
constexpr uint8_t HAT_OPEN_LOCAL_CC = 100;
constexpr uint32_t SNARE_DECAY_NOISE = 0xF5000000u;
constexpr uint32_t SNARE_DECAY_TONE = 0xF9400000u;

// Mode-change ping spacing (samples between consecutive ticks).
constexpr uint32_t MODE_PING_SPACING_SAMPLES = (uint32_t)(SAMPLE_RATE / 10);  // 100 ms

constexpr int16_t PITCH_KICK_DEFAULT = 4608;   // 36 << 7
constexpr int16_t PITCH_SNARE_DEFAULT = 8200;
constexpr int16_t PITCH_HAT_DEFAULT = 11520;   // 90 << 7
constexpr int16_t PITCH_PERC_DEFAULT = 9600;   // 75 << 7

constexpr int16_t TIMBRE_KICK_DEFAULT = 25000;
constexpr int16_t COLOR_KICK_DEFAULT = -11072;
constexpr int16_t TIMBRE_SNARE_DEFAULT = 30000;
constexpr int16_t COLOR_SNARE_DEFAULT = -25536;
constexpr int16_t TIMBRE_HAT_DEFAULT = -25536;
constexpr int16_t COLOR_HAT_DEFAULT = 30000;
constexpr int16_t TIMBRE_PERC_DEFAULT = 25464;
constexpr int16_t COLOR_PERC_DEFAULT = 19464;

enum class VoiceId : uint8_t {
  Kick = 0,
  Snare = 1,
  Hat = 2,
  Perc = 3,
};

enum class HatStepType : uint8_t {
  Off = 0,
  Closed = 1,
  Open = 2,
};

struct ButtonState {
  bool raw_pressed;
  bool stable_pressed;
  uint32_t last_change_ms;
  uint32_t pressed_ms;
  bool ignore_release;
  bool retrigger_mode_press;
  bool has_tap_snapshot;
  uint8_t tap_target_step;
  HatStepType tap_hat_type;
  bool clear_triggered;
};

// Pitched woodblock-style metronome: a triangle oscillator fed through a
// 1-pole LP (to round it toward a sine) plus a short noise transient for
// the attack. amp/decay shape the body env, trans_amp/trans_decay shape
// the click transient, phase/phase_inc drive the pitched oscillator.
struct MetronomeState {
  uint32_t amp;
  uint32_t decay;
  uint32_t trans_amp;
  uint32_t trans_decay;
  uint32_t phase;
  uint32_t phase_inc;
  int32_t  lp_state;
  int16_t  gain;
};

// Soft takeover: a pot only takes control once its physical position
// matches where the parameter currently sits, preventing jumps on mode switch.
struct PotPickup {
  uint16_t target;
  bool     active;
};

static inline bool potActive(PotPickup* pp, uint16_t adc) {
  if (pp->active) return true;
  int32_t diff = (int32_t)adc - (int32_t)pp->target;
  if (diff < 0) diff = -diff;
  if ((uint16_t)diff <= POT_PICKUP_THRESHOLD) pp->active = true;
  return pp->active;
}

static inline void potsLockAll(PotPickup pp[4], const uint16_t targets[4]) {
  for (uint8_t i = 0; i < 4; ++i) { pp[i].target = targets[i]; pp[i].active = false; }
}

static inline uint32_t freqToPhaseInc(uint32_t hz) {
  return (uint32_t)(((uint64_t)hz << 32) / (uint64_t)SAMPLE_RATE);
}

I2S i2s(OUTPUT, I2S_BCLK_PIN, I2S_DIN_PIN);
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

DrumVoice kick;
DrumVoice snare;
DrumVoice hihat;
DrumVoice percussion;

bool kick_pattern[STEP_COUNT_MAX] = {};
bool snare_pattern[STEP_COUNT_MAX] = {};
bool perc_pattern[STEP_COUNT_MAX] = {};
HatStepType hat_pattern[STEP_COUNT_MAX] = {};

ButtonState buttons[BUTTON_COUNT] = {};
MetronomeState metronome = {};

uint16_t pot_values[4] = {};
bool     led_active  = false;
uint32_t led_off_ms  = 0;
uint8_t kick_decay_cc = 64;
uint8_t snare_mix_cc = 64;
uint8_t hat_decay_cc = 64;
uint8_t perc_pitch_cc = 64;
uint16_t bpm = 128;
uint8_t swing_percent = 20;
uint8_t pattern_length = 16;
uint16_t master_level = 20000;
bool bpm_mode = false;

uint8_t current_step = 0;
uint8_t next_step = 1;
// Sample-accurate transport: all timing is measured in audio samples so it
// is immune to millis() jitter caused by I2S back-pressure.
uint32_t current_step_length_samples = 6000;   // 16th @120 BPM, 48 kHz
int32_t  step_samples_remaining = 6000;
uint8_t retrigger_mask = 0;  // one bit per button index (0..4)
uint32_t metronome_noise_state = 0x13579BDFu;

// Mode-change ping queue: fires N ticks spaced MODE_PING_SPACING_SAMPLES apart.
uint8_t  mode_ping_remaining = 0;
int32_t  mode_ping_next_samples = 0;
int16_t  mode_ping_gain = 10000;
uint32_t mode_ping_decay = 0xE1800000u;
uint32_t mode_ping_freq = 1800;

PotPickup pot_pickup[4] = {};

bool midi_ready = false;
bool midi_clock_active = false;
volatile bool midi_clock_seen = false;
volatile bool midi_transport_running = false;
volatile bool midi_start_requested = false;
volatile uint16_t midi_clock_pulses_queued = 0;
volatile uint32_t midi_last_clock_ms = 0;
uint8_t midi_clock_step_pulse = 0;

static inline void serviceUsb() {
  TinyUSBDevice.task();
  if (midi_ready) MIDI.read();
  yield();
}

static uint8_t potToCc(uint16_t pot) {
  return (uint8_t)((uint32_t)pot * (uint32_t)MIDI_MAX / (uint32_t)POT_MAX);
}

static uint16_t potToLevelQ15(uint16_t pot) {
  return (uint16_t)((uint32_t)pot * (uint32_t)Q15_UNITY / (uint32_t)POT_MAX);
}

static uint16_t potToBpm(uint16_t pot) {
  return (uint16_t)(BPM_MIN + ((uint32_t)pot * (uint32_t)(BPM_MAX - BPM_MIN) / (uint32_t)POT_MAX));
}

static uint8_t potToSwing(uint16_t pot) {
  return (uint8_t)((uint32_t)pot * (uint32_t)SWING_MAX / (uint32_t)POT_MAX);
}

static uint8_t potToPatternLength(uint16_t pot) {
  static const uint8_t choices[5] = {8, 12, 16, 24, 32};
  uint8_t index = (uint8_t)((uint32_t)pot * 5u / ((uint32_t)POT_MAX + 1u));
  if (index > 4) index = 4;
  return choices[index];
}

// Base 16th-note duration in audio samples: SR * 60 / (BPM * 4).
static uint32_t baseStepSamples() {
  return (uint32_t)SAMPLE_RATE * 60u / ((uint32_t)bpm * 4u);
}

static uint32_t stepDurationSamples(uint8_t step_index) {
  const uint32_t base = baseStepSamples();
  const uint32_t offset = ((uint64_t)base * (uint64_t)swing_percent) / 100u;
  if ((step_index & 1u) == 0u) return base + offset;
  const uint32_t shortened = (base > offset) ? (base - offset) : 1u;
  return shortened ? shortened : 1u;
}

static void wrapTransportToLength() {
  if (pattern_length == 0) pattern_length = 16;
  current_step %= pattern_length;
  next_step %= pattern_length;
}

static void updateCurrentStepLength() {
  // Recompute the current step's length in samples; the remaining-counter
  // is left alone so we don't retime mid-step when BPM/swing change.
  current_step_length_samples = stepDurationSamples(current_step);
}

static void clearVoicePattern(VoiceId voice) {
  for (uint8_t step = 0; step < STEP_COUNT_MAX; ++step) {
    if (voice == VoiceId::Kick) kick_pattern[step] = false;
    if (voice == VoiceId::Snare) snare_pattern[step] = false;
    if (voice == VoiceId::Hat) hat_pattern[step] = HatStepType::Off;
    if (voice == VoiceId::Perc) perc_pattern[step] = false;
  }
}

static void triggerHatClosed() {
  hihat.amp = 0;
  hihat.decay_override_pending = true;
  hihat.next_amp_decay = hatDecayFromCC(hat_decay_cc, HAT_CLOSED_LOCAL_CC);
  triggerDrum(&hihat);
}

static void triggerHatOpen() {
  hihat.decay_override_pending = true;
  hihat.next_amp_decay = hatDecayFromCC(hat_decay_cc, HAT_OPEN_LOCAL_CC);
  triggerDrum(&hihat);
}

// Fire a pitched metronome click. Resets the oscillator phase so each tick
// has a clean, consistent attack instead of starting mid-waveform.
static inline void triggerMetronome(uint32_t freq, uint32_t decay, int16_t gain) {
  metronome.amp = Q32_UNITY;
  metronome.trans_amp = Q32_UNITY;
  metronome.decay = decay;
  metronome.trans_decay = 0xC0000000u;  // ~3 ms click
  metronome.phase = 0;
  metronome.phase_inc = freqToPhaseInc(freq);
  metronome.lp_state = 0;
  metronome.gain = gain;
}

static void triggerVoice(VoiceId voice, HatStepType hat_type) {
  if (voice == VoiceId::Kick) triggerDrum(&kick);
  if (voice == VoiceId::Snare) triggerDrum(&snare);
  if (voice == VoiceId::Hat) {
    if (hat_type == HatStepType::Open) triggerHatOpen();
    else triggerHatClosed();
  }
  if (voice == VoiceId::Perc) triggerDrum(&percussion);
}

static void playPatternStep(uint8_t step) {
  if (kick_pattern[step]) triggerVoice(VoiceId::Kick, HatStepType::Closed);
  if (snare_pattern[step]) triggerVoice(VoiceId::Snare, HatStepType::Closed);
  if (hat_pattern[step] == HatStepType::Open) triggerVoice(VoiceId::Hat, HatStepType::Open);
  if (hat_pattern[step] == HatStepType::Closed) triggerVoice(VoiceId::Hat, HatStepType::Closed);
  if (perc_pattern[step]) triggerVoice(VoiceId::Perc, HatStepType::Closed);

  // Retrigger mask is indexed by button index: 0=Kick, 1=Snare, 2=HatClosed,
  // 3=Perc, 4=HatOpen. In settings (bpm) mode, holding an instrument button
  // keeps re-firing its voice on every step.
  if (retrigger_mask & (1u << 0)) triggerVoice(VoiceId::Kick, HatStepType::Closed);
  if (retrigger_mask & (1u << 1)) triggerVoice(VoiceId::Snare, HatStepType::Closed);
  if (retrigger_mask & (1u << 2)) triggerVoice(VoiceId::Hat, HatStepType::Closed);
  if (retrigger_mask & (1u << 3)) triggerVoice(VoiceId::Perc, HatStepType::Closed);
  if (retrigger_mask & (1u << 4)) triggerVoice(VoiceId::Hat, HatStepType::Open);
}

static void advanceTransport() {
  playPatternStep(next_step);
  // BPM LED: flash on every quarter note so it reads as a live metronome.
  if ((next_step % 4u) == 0u) {
    const uint32_t flash_ms = max(20u, 60000u / (uint32_t)bpm / 8u);
    digitalWrite(PIN_STATUS_LED, HIGH);
    led_active = true;
    led_off_ms = millis() + flash_ms;
  }
  current_step = next_step;
  current_step_length_samples = stepDurationSamples(current_step);
  next_step = (uint8_t)((current_step + 1u) % pattern_length);
}

static void updateMidiClockState() {
  const bool seen = midi_clock_seen;
  const bool running = midi_transport_running;
  const uint32_t last_clock_ms = midi_last_clock_ms;
  midi_clock_active = seen && (!running || ((uint32_t)(millis() - last_clock_ms) <= MIDI_CLOCK_TIMEOUT_MS));
}

static void serviceMidiClockSequencer() {
  if (midi_start_requested) {
    midi_start_requested = false;
    midi_clock_pulses_queued = 0;
    midi_clock_step_pulse = 0;
    current_step = 0;
    next_step = 0;
    advanceTransport();
    step_samples_remaining = (int32_t)current_step_length_samples;
  }

  if (!midi_transport_running) {
    midi_clock_pulses_queued = 0;
    return;
  }

  uint16_t pulses = midi_clock_pulses_queued;
  midi_clock_pulses_queued = 0;
  while (pulses > 0) {
    pulses--;
    midi_clock_step_pulse++;
    if (midi_clock_step_pulse >= MIDI_CLOCKS_PER_STEP) {
      midi_clock_step_pulse = 0;
      advanceTransport();
    }
  }
}

static void handleMidiClock() {
  midi_clock_seen = true;
  midi_last_clock_ms = millis();
  if (midi_transport_running && midi_clock_pulses_queued < 96u) midi_clock_pulses_queued++;
}

static void handleMidiStart() {
  midi_clock_seen = true;
  midi_transport_running = true;
  midi_start_requested = true;
  midi_clock_pulses_queued = 0;
  midi_last_clock_ms = millis();
}

static void handleMidiContinue() {
  midi_clock_seen = true;
  midi_transport_running = true;
  midi_last_clock_ms = millis();
}

static void handleMidiStop() {
  midi_clock_seen = true;
  midi_transport_running = false;
  midi_clock_pulses_queued = 0;
  midi_last_clock_ms = millis();
}

static uint8_t nearestStepForTap() {
  const int32_t remaining = step_samples_remaining;
  const int32_t elapsed = (int32_t)current_step_length_samples - remaining;
  if (elapsed < (int32_t)(current_step_length_samples / 2u)) return current_step;
  return next_step;
}

static void toggleRecordedStep(VoiceId voice, uint8_t step, HatStepType hat_type) {
  if (voice == VoiceId::Kick) {
    kick_pattern[step] = !kick_pattern[step];
    return;
  }
  if (voice == VoiceId::Snare) {
    snare_pattern[step] = !snare_pattern[step];
    return;
  }
  if (voice == VoiceId::Perc) {
    perc_pattern[step] = !perc_pattern[step];
    return;
  }
  if (hat_pattern[step] == HatStepType::Off) {
    hat_pattern[step] = hat_type;
  } else {
    hat_pattern[step] = HatStepType::Off;
  }
}

static void applyNormalPots() {
  // Only apply if the pot has 'caught' its stored target (soft takeover).

  // Pot 0 -- Kick "Punch": couples amp decay AND pitch-envelope depth, so
  // the pot goes from a tight blip (short tail, small drop) at CCW to a
  // deep boom (long tail, 18-semitone sweep) at CW. Musically this is a
  // single gesture instead of a one-dimensional decay.
  if (potActive(&pot_pickup[0], pot_values[0])) {
    kick_decay_cc = potToCc(pot_values[0]);
    // Extended decay range for kick: goes from DECAY_FAST to very slow (0xFFF00000u)
    const uint32_t t_q16 = (uint32_t)kick_decay_cc * Q16_UNITY / (uint32_t)MIDI_MAX;
    kick.amp_decay = lerpU32(DECAY_FAST, 0xFFF00000u, t_q16);
    kick.pitch_env_range_q7 =
        (int16_t)((6u + ((uint32_t)kick_decay_cc * 12u / (uint32_t)MIDI_MAX)) << 7);
  }

  // Pot 1 -- Snare "Decay": CCW = tight/short tail, CW = long body.
  if (potActive(&pot_pickup[1], pot_values[1])) {
    snare_mix_cc = potToCc(pot_values[1]);
    snare.amp_decay = decayFromCC(snare_mix_cc);
  }

  // Pot 2 -- Hi-hat "Decay": controls the decay length for both closed and
  // open hi-hats. Open vs. closed is selected by which button you press
  // (BTN_2 = closed, BTN_4 = open).
  if (potActive(&pot_pickup[2], pot_values[2])) {
    hat_decay_cc = potToCc(pot_values[2]);
  }

  // Pot 3 -- Perc "Tune + Character": coupling timbre/color to pitch so
  // low notes read as warm mallet hits and high notes read as metallic
  // pings -- a musician's expectation when "tuning" a percussion voice.
  if (potActive(&pot_pickup[3], pot_values[3])) {
    perc_pitch_cc = potToCc(pot_values[3]);
    percussion.pitch = ccToPitch(perc_pitch_cc, 24, 96);
    percussion.timbre =
        (int16_t)(14000 + ((uint32_t)perc_pitch_cc * 16000u / (uint32_t)MIDI_MAX));
    percussion.color =
        (int16_t)(8000 + ((uint32_t)perc_pitch_cc * 16000u / (uint32_t)MIDI_MAX));
  }
}

static void applyBpmModePots() {
  if (potActive(&pot_pickup[0], pot_values[0])) bpm = potToBpm(pot_values[0]);
  if (potActive(&pot_pickup[1], pot_values[1])) swing_percent = potToSwing(pot_values[1]);
  if (potActive(&pot_pickup[2], pot_values[2])) {
    const uint8_t new_length = potToPatternLength(pot_values[2]);
    if (pattern_length != new_length) {
      pattern_length = new_length;
      wrapTransportToLength();
    }
  }
  if (potActive(&pot_pickup[3], pot_values[3])) master_level = potToLevelQ15(pot_values[3]);
}

static void applyPotAssignments() {
  if (bpm_mode) applyBpmModePots();
  else applyNormalPots();
  updateCurrentStepLength();
}

// Convert the current parameter values back to pot (ADC) units so that
// soft-takeover can lock to them when entering a new mode.
static void lockPotsToNormalMode() {
  const uint16_t targets[4] = {
    (uint16_t)((uint32_t)kick_decay_cc * POT_MAX / MIDI_MAX),
    (uint16_t)((uint32_t)snare_mix_cc  * POT_MAX / MIDI_MAX),
    (uint16_t)((uint32_t)hat_decay_cc  * POT_MAX / MIDI_MAX),
    (uint16_t)((uint32_t)perc_pitch_cc * POT_MAX / MIDI_MAX),
  };
  potsLockAll(pot_pickup, targets);
}

static void lockPotsToBpmMode() {
  // Pattern-length pot is discrete; pick nearest ADC center.
  static const uint8_t choices[5] = {8, 12, 16, 24, 32};
  uint8_t idx = 2;
  for (uint8_t i = 0; i < 5; ++i) { if (choices[i] == pattern_length) { idx = i; break; } }
  const uint16_t len_adc = (uint16_t)(((uint32_t)idx + 0u) * (POT_MAX + 1u) / 5u + (POT_MAX + 1u) / 10u);

  const uint16_t targets[4] = {
    (uint16_t)(((uint32_t)(bpm - BPM_MIN)) * POT_MAX / (uint32_t)(BPM_MAX - BPM_MIN)),
    (uint16_t)((uint32_t)swing_percent * POT_MAX / SWING_MAX),
    len_adc,
    (uint16_t)((uint32_t)master_level * POT_MAX / Q15_UNITY),
  };
  potsLockAll(pot_pickup, targets);
}

// Queue N short pitched ticks spaced MODE_PING_SPACING_SAMPLES apart --
// audible mode-change feedback. Two high ticks = entered BPM mode; one
// lower tick = exited.
static void queueModePing(uint8_t count, int16_t gain, uint32_t decay, uint32_t freq) {
  mode_ping_remaining = count;
  mode_ping_next_samples = 0;  // fire first one immediately
  mode_ping_gain = gain;
  mode_ping_decay = decay;
  mode_ping_freq = freq;
}

static void readPots() {
  const uint8_t pins[4] = {PIN_POT_0, PIN_POT_1, PIN_POT_2, PIN_POT_3};
  for (uint8_t i = 0; i < 4; ++i) {
    pot_values[i] = (uint16_t)analogRead(pins[i]);
  }
}

// Map a button index (0..4) to its voice + hat type.
static inline VoiceId voiceForButton(uint8_t index) {
  if (index == 4) return VoiceId::Hat;
  return (VoiceId)index;
}
static inline HatStepType hatTypeForButton(uint8_t index) {
  return (index == 4) ? HatStepType::Open : HatStepType::Closed;
}

static void onButtonPressed(uint8_t index, uint32_t now_ms) {
  // In settings (bpm) mode, each instrument button retriggers its voice
  // continuously while held -- replaces the old retrigger-modifier gesture.
  if (bpm_mode) {
    buttons[index].ignore_release = false;
    buttons[index].retrigger_mode_press = true;
    buttons[index].has_tap_snapshot = false;
    buttons[index].pressed_ms = now_ms;
    retrigger_mask |= (uint8_t)(1u << index);
    triggerVoice(voiceForButton(index), hatTypeForButton(index));
    return;
  }

  buttons[index].pressed_ms = now_ms;
  buttons[index].ignore_release = false;
  buttons[index].retrigger_mode_press = false;
  buttons[index].has_tap_snapshot = true;
  buttons[index].tap_target_step = nearestStepForTap();
}

static void clearTrackForButton(uint8_t index) {
  const VoiceId voice = voiceForButton(index);
  const HatStepType hat_type = hatTypeForButton(index);
  // Long-hold on a voice button clears every step from that track. For
  // the hi-hat buttons, we only clear steps of the matching type so the
  // closed button doesn't wipe open hits (and vice versa).
  if (voice == VoiceId::Hat) {
    for (uint8_t step = 0; step < STEP_COUNT_MAX; ++step) {
      if (hat_pattern[step] == hat_type) hat_pattern[step] = HatStepType::Off;
    }
  } else {
    clearVoicePattern(voice);
  }
}

static void onButtonReleased(uint8_t index, uint32_t now_ms) {
  ButtonState& button = buttons[index];
  button.clear_triggered = false;
  if (button.retrigger_mode_press) {
    button.retrigger_mode_press = false;
    retrigger_mask &= (uint8_t)~(1u << index);
    return;
  }
  if (button.ignore_release) return;

  const uint32_t held_ms = now_ms - button.pressed_ms;
  if (held_ms < SHORT_PRESS_MIN_MS) return;

  const VoiceId voice = voiceForButton(index);
  const HatStepType hat_type = hatTypeForButton(index);

  if (held_ms >= CLEAR_HOLD_MS) {
    clearTrackForButton(index);
    return;
  }

  if (bpm_mode) return;

  triggerVoice(voice, hat_type);
  if (button.has_tap_snapshot) {
    toggleRecordedStep(voice, button.tap_target_step, hat_type);
  }
}

static void pollButtons() {
  const uint8_t pins[BUTTON_COUNT] = {PIN_BTN_0, PIN_BTN_1, PIN_BTN_2, PIN_BTN_3, PIN_BTN_4};
  const uint32_t now_ms = millis();

  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    const bool raw_pressed = (digitalRead(pins[i]) == LOW);
    if (raw_pressed != buttons[i].raw_pressed) {
      buttons[i].raw_pressed = raw_pressed;
      buttons[i].last_change_ms = now_ms;
    }

    if ((now_ms - buttons[i].last_change_ms) < BUTTON_DEBOUNCE_MS) continue;
    if (raw_pressed == buttons[i].stable_pressed) continue;

    buttons[i].stable_pressed = raw_pressed;
    if (raw_pressed) {
      onButtonPressed(i, now_ms);
    } else {
      onButtonReleased(i, now_ms);
    }
  }

  // Check for hold-to-clear threshold while button is held (immediate clear)
  if (!bpm_mode) {
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
      if (!buttons[i].stable_pressed) continue;
      if (buttons[i].clear_triggered) continue;
      const uint32_t held_ms = now_ms - buttons[i].pressed_ms;
      if (held_ms >= CLEAR_HOLD_MS) {
        buttons[i].clear_triggered = true;
        buttons[i].ignore_release = true;
        clearTrackForButton(i);
      }
    }
  }

  bool all_pressed = true;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!buttons[i].stable_pressed) {
      all_pressed = false;
      break;
    }
  }

  static bool all_pressed_previous = false;
  if (all_pressed && !all_pressed_previous) {
    bpm_mode = !bpm_mode;
    retrigger_mask = 0;
    // Soft-takeover: lock all 4 pots to their current parameter values in
    // the newly-entered mode so no pot jumps on mode switch.
    if (bpm_mode) {
      lockPotsToBpmMode();
      // Two high/quick ticks = entered BPM mode.
      queueModePing(2, 14000, 0xE8000000u, 2200u);
    } else {
      lockPotsToNormalMode();
      // One low/slower tick = exited BPM mode.
      queueModePing(1, 11000, 0xEA000000u, 1200u);
    }
    for (uint8_t i = 0; i < 4; ++i) {
      buttons[i].ignore_release = true;
      buttons[i].retrigger_mode_press = false;
      buttons[i].has_tap_snapshot = false;
    }
  }
  all_pressed_previous = all_pressed;
}

// Decrement the sample counter by one block; advance transport every time it
// crosses zero. Called from renderAudioBlock so triggers line up with audio.
static void serviceSequencerSamples() {
  if (midi_clock_active) {
    serviceMidiClockSequencer();
    return;
  }
  step_samples_remaining -= (int32_t)BLOCK_SIZE;
  while (step_samples_remaining <= 0) {
    advanceTransport();
    step_samples_remaining += (int32_t)current_step_length_samples;
  }
}

// Trigger the next queued mode ping if its sample deadline has arrived.
static inline void serviceModePing() {
  if (mode_ping_remaining == 0) return;
  if (mode_ping_next_samples > 0) { mode_ping_next_samples -= (int32_t)BLOCK_SIZE; return; }
  triggerMetronome(mode_ping_freq, mode_ping_decay, mode_ping_gain);
  mode_ping_remaining--;
  mode_ping_next_samples = (int32_t)MODE_PING_SPACING_SAMPLES;
}

static void renderAudioBlock() {
  serviceUsb();
  // Non-blocking gate: if the I2S FIFO has no room for a full stereo block,
  // bail out so loop() can keep calling TinyUSBDevice.task(). This is what
  // keeps the 1200-baud reset-to-BOOTSEL trick working when a new sketch is
  // uploaded; the previous blocking loop is what required opening the case.
  // Each stereo frame = 2 int16 writes, so we need BLOCK_SIZE * 2 slots.
  if (i2s.availableForWrite() < (int)(BLOCK_SIZE * 2)) return;

  // Sample-accurate sequencer tick BEFORE rendering, so any triggers set
  // here are consumed by updateDrum below -- avoids a block of latency.
  serviceSequencerSamples();
  serviceModePing();

  updateDrum(&kick);
  updateDrum(&snare);
  updateDrum(&hihat);
  updateDrum(&percussion);

  for (uint8_t i = 0; i < BLOCK_SIZE; ++i) {
    const uint16_t env_k = voiceEnvQ15AtSample(&kick, i);
    const uint16_t env_s = voiceEnvQ15AtSample(&snare, i);
    const uint16_t env_h = voiceEnvQ15AtSample(&hihat, i);
    const uint16_t env_p = voiceEnvQ15AtSample(&percussion, i);
    int32_t k = ((int32_t)kick.buffer[i]       * (int32_t)env_k) >> 15;
    int32_t s = ((int32_t)snare.buffer[i]      * (int32_t)env_s) >> 15;
    int32_t h = ((int32_t)hihat.buffer[i]      * (int32_t)env_h) >> 15;
    int32_t p = ((int32_t)percussion.buffer[i] * (int32_t)env_p) >> 15;
    k = (k * (int32_t)kick.gain_q15)       >> 15;
    s = (s * (int32_t)snare.gain_q15)      >> 15;
    h = (h * (int32_t)hihat.gain_q15)      >> 15;
    p = (p * (int32_t)percussion.gain_q15) >> 15;
    k = (k * (int32_t)kick.velocity_q15)       >> 15;
    s = (s * (int32_t)snare.velocity_q15)      >> 15;
    h = (h * (int32_t)hihat.velocity_q15)      >> 15;
    p = (p * (int32_t)percussion.velocity_q15) >> 15;
    int32_t m = 0;

    if (metronome.amp != 0 || metronome.trans_amp != 0) {
      // Pitched body: triangle wave from phase accumulator, softened by a
      // 1-pole LP to round it toward a sine-ish woodblock tone.
      metronome.phase += metronome.phase_inc;
      const uint32_t ph = metronome.phase;
      int32_t tri;
      if (ph < 0x80000000u) {
        tri = (int32_t)(ph >> 15) - 0x8000;          // -32768..32767 up-ramp
      } else {
        tri = 0x7FFF - (int32_t)((ph - 0x80000000u) >> 15);  // down-ramp
      }
      metronome.lp_state += ((tri - metronome.lp_state) * 10000) >> 15;
      const int32_t body_env = (int32_t)(uint16_t)(metronome.amp >> 16);
      int32_t body = (metronome.lp_state * body_env) >> 15;

      // Short noise transient for the attack "tick".
      metronome_noise_state = metronome_noise_state * 1664525u + 1013904223u;
      const int16_t noise = (int16_t)(metronome_noise_state >> 16);
      const int32_t trans_env = (int32_t)(uint16_t)(metronome.trans_amp >> 16);
      int32_t trans = ((int32_t)noise * trans_env) >> 15;
      trans = (trans * 14000) >> 15;

      m = (((body + trans) * (int32_t)metronome.gain) >> 15);
      metronome.amp = (uint32_t)(((uint64_t)metronome.amp * (uint64_t)metronome.decay) >> 32);
      metronome.trans_amp = (uint32_t)(((uint64_t)metronome.trans_amp * (uint64_t)metronome.trans_decay) >> 32);
    }

    // Accumulate into int32 without saturation so the soft-clipper below
    // has room to compress peaks instead of hard-clipping them.
    int32_t mix = k + s + h + p + m;

    // Soft-clip: gentle cubic saturation above ~0.75 full scale prevents
    // the aggressive brick-wall clipping that was audible on dense beats.
    int32_t sample = ((int32_t)mix * (int32_t)master_level) >> 15;
    if (sample > 24576) {
      const int32_t over = sample - 24576;
      sample = 24576 + (over * 6144) / (over + 12288);
    } else if (sample < -24576) {
      const int32_t over = -24576 - sample;
      sample = -24576 - (over * 6144) / (over + 12288);
    }
    const int16_t out = clipToInt16(sample);
    if ((i & 0x07u) == 0u) serviceUsb();
    i2s.write(out);
    i2s.write(out);
  }
}

void setup() {
  analogReadResolution(12);
  // NOTE: Do NOT call TinyUSBDevice.begin() here. On the arduino-pico core
  // with the Adafruit TinyUSB stack selected, the core already starts the
  // USB device (and registers the 1200bps-touch -> BOOTSEL reset handler)
  // before setup() runs. Calling begin() again here re-initialises the USB
  // stack and drops that reset hook, which is what forces manual UF2-mode
  // reflashing.
  Serial.begin(115200);
  usb_midi.setStringDescriptor("DrumLoop MIDI");
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();
  MIDI.setHandleClock(handleMidiClock);
  MIDI.setHandleStart(handleMidiStart);
  MIDI.setHandleContinue(handleMidiContinue);
  MIDI.setHandleStop(handleMidiStop);
  midi_ready = true;
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
  // Wait for serial connection (optional, timeout after 3s)
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 3000) {
    serviceUsb();
    delay(1);
  }

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_BTN_0, INPUT_PULLUP);
  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BTN_2, INPUT_PULLUP);
  pinMode(PIN_BTN_3, INPUT_PULLUP);
  pinMode(PIN_BTN_4, INPUT_PULLUP);

  // BOOTSEL back door: if the user is holding the open-hat button +
  // button 0 at plug-in, jump straight to the UF2 bootloader. This avoids
  // having to open the enclosure to press the physical BOOTSEL button if a
  // future sketch ever hangs before TinyUSB comes up.
  delay(50);  // let INPUT_PULLUP lines settle
  if (digitalRead(PIN_BTN_4) == LOW && digitalRead(PIN_BTN_0) == LOW) {
    reset_usb_boot(0, 0);
  }

  i2s.setBitsPerSample(16);
  i2s.begin(SAMPLE_RATE);
  serviceUsb();

  initDrumVoice(&kick, braids::MACRO_OSC_SHAPE_KICK, PITCH_KICK_DEFAULT);
  initDrumVoice(&snare, braids::MACRO_OSC_SHAPE_SNARE, PITCH_SNARE_DEFAULT);
  initDrumVoice(&hihat, braids::MACRO_OSC_SHAPE_CYMBAL, PITCH_HAT_DEFAULT);
  initDrumVoice(&percussion, braids::MACRO_OSC_SHAPE_STRUCK_DRUM, PITCH_PERC_DEFAULT);

  kick.amp_decay = 0xFECE51A0u;
  snare.amp_decay = 0xFED1D7E0u;
  hihat.amp_decay = 0xFC6DDA20u;
  percussion.amp_decay = 0xF7000000u;

  kick.timbre = TIMBRE_KICK_DEFAULT;
  kick.color = COLOR_KICK_DEFAULT;
  snare.timbre = TIMBRE_SNARE_DEFAULT;
  snare.color = COLOR_SNARE_DEFAULT;
  hihat.timbre = TIMBRE_HAT_DEFAULT;
  hihat.color = COLOR_HAT_DEFAULT;
  percussion.timbre = TIMBRE_PERC_DEFAULT;
  percussion.color = COLOR_PERC_DEFAULT;

  // Transient (click) layer -- fast attack-side bump summed on top of body.
  kick.trans_decay  = TRANS_DECAY_KICK;  kick.trans_gain_q15  = 18000;
  snare.trans_decay = TRANS_DECAY_SNARE; snare.trans_gain_q15 = 14000;
  hihat.trans_decay = TRANS_DECAY_HAT;   hihat.trans_gain_q15 = 16000;
  percussion.trans_decay = TRANS_DECAY_PERC; percussion.trans_gain_q15 = 12000;

  // Kick pitch envelope: +12 semitones on strike, decaying to base. This is
  // the single biggest factor turning 'click' into 'thump'.
  kick.pitch_env_range_q7 = (int16_t)(12 << 7);
  kick.pitch_env_decay    = PITCH_ENV_DECAY_KICK;

  // Per-voice tone shaping: LP kick/snare/perc, HP the cymbal so it sits up.
  kick.filt_type  = FILT_LP;  kick.filt_k_q15  = 6000;   // ~2.8 kHz LP
  snare.filt_type = FILT_LP;  snare.filt_k_q15 = 18000;  // ~4.5 kHz LP
  hihat.filt_type = FILT_HP;  hihat.filt_k_q15 = 16000;  // HP, cuts mud below ~4 kHz
  percussion.filt_type = FILT_LP; percussion.filt_k_q15 = 10000; // ~2.5 kHz LP

  // Per-voice gain so the mixer no longer needs a blanket >> 1.
  kick.gain_q15       = 30000;
  snare.gain_q15      = 22000;
  hihat.gain_q15      = 25000;
  percussion.gain_q15 = 20000;

  readPots();
  // Start with pots active: parameters immediately match physical positions.
  for (uint8_t i = 0; i < 4; ++i) { pot_pickup[i].active = true; }
  applyPotAssignments();

  current_step = 0;
  current_step_length_samples = stepDurationSamples(current_step);
  step_samples_remaining = (int32_t)current_step_length_samples;
  next_step = 1;
  playPatternStep(current_step);
}

// Serial command parser: set voice params live, then dump for hardcoding.
// Commands (newline-terminated):
//   k|s|h|p pitch <value>       -- set pitch (MIDI note * 128)
//   k|s|h|p timbre <value>      -- set timbre (0..32767)
//   k|s|h|p color <value>       -- set color (0..32767)
//   k|s|h|p decay <value>       -- set amp_decay (hex, e.g., 0xFE000000)
//   k|s|h|p filt <value>        -- set filt_k_q15 (0..32767)
//   k|s|h|p gain <value>        -- set gain_q15 (0..32767)
//   k|s|h|p trans <value>       -- set trans_gain_q15 (0..32767)
//   dump                        -- print all current voice params
//   help                        -- show command summary
// Letters: k=kick, s=snare, h=hihat, p=perc
static void serviceSerialCommands() {
  static char cmd[64];
  static uint8_t cmd_len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmd[cmd_len] = '\0';
      cmd_len = 0;
      // Parse
      char voice_letter = 0;
      char param[8] = {};
      char val_str[32] = {};
      int n = sscanf(cmd, "%c %7s %31s", &voice_letter, param, val_str);
      DrumVoice* voice = nullptr;
      if (voice_letter == 'k') voice = &kick;
      else if (voice_letter == 's') voice = &snare;
      else if (voice_letter == 'h') voice = &hihat;
      else if (voice_letter == 'p') voice = &percussion;

      if (voice && n == 3) {
        int32_t value = (int32_t)strtol(val_str, nullptr, 0); // auto-detect hex/dec
        if (strcmp(param, "pitch") == 0) voice->pitch = (int16_t)value;
        else if (strcmp(param, "timbre") == 0) voice->timbre = (int16_t)value;
        else if (strcmp(param, "color") == 0) voice->color = (int16_t)value;
        else if (strcmp(param, "decay") == 0) voice->amp_decay = (uint32_t)value;
        else if (strcmp(param, "filt") == 0) voice->filt_k_q15 = (int16_t)value;
        else if (strcmp(param, "gain") == 0) voice->gain_q15 = (int16_t)value;
        else if (strcmp(param, "trans") == 0) voice->trans_gain_q15 = (int16_t)value;
        else Serial.println("Unknown param. Use: pitch timbre color decay filt gain trans");
        Serial.print("OK: "); Serial.print(voice_letter); Serial.print(" "); Serial.print(param); Serial.print(" = "); Serial.println(value);
      } else if (strcmp(cmd, "dump") == 0) {
        auto dumpVoice = [](const char* name, const DrumVoice* v) {
          Serial.print("// "); Serial.println(name);
          Serial.print("voice.pitch = "); Serial.print(v->pitch); Serial.println(";");
          Serial.print("voice.timbre = "); Serial.print(v->timbre); Serial.println(";");
          Serial.print("voice.color = "); Serial.print(v->color); Serial.println(";");
          Serial.print("voice.amp_decay = 0x"); Serial.print(v->amp_decay, HEX); Serial.println("u;");
          Serial.print("voice.filt_k_q15 = "); Serial.print(v->filt_k_q15); Serial.println(";");
          Serial.print("voice.gain_q15 = "); Serial.print(v->gain_q15); Serial.println(";");
          Serial.print("voice.trans_gain_q15 = "); Serial.print(v->trans_gain_q15); Serial.println(";");
          Serial.println();
        };
        Serial.println("// === CURRENT VOICE SETTINGS ===");
        dumpVoice("Kick", &kick); dumpVoice("Snare", &snare); dumpVoice("Hat", &hihat); dumpVoice("Perc", &percussion);
      } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        Serial.println("Commands: <voice> <param> <value>  (voice: k,s,h,p)");
        Serial.println("Params: pitch, timbre, color, decay, filt, gain, trans");
        Serial.println("Examples: k timbre 25000   s decay 0xFD000000   h gain 26000");
        Serial.println("Other: dump   help");
      } else if (cmd_len > 1) {
        Serial.println("Bad command. Try: k timbre 25000   or   dump");
      }
    } else if (cmd_len < sizeof(cmd)-1) {
      cmd[cmd_len++] = c;
    }
  }
}

void loop() {
  serviceUsb();
  serviceSerialCommands();
  updateMidiClockState();

  readPots();
  applyPotAssignments();
  pollButtons();

  // Transport ticking happens inside renderAudioBlock, driven by the audio
  // sample count. No millis() in the hot path means no timing jitter.
  renderAudioBlock();
  serviceUsb();
  if (led_active && (millis() >= led_off_ms)) {
    digitalWrite(PIN_STATUS_LED, LOW);
    led_active = false;
  }
}
