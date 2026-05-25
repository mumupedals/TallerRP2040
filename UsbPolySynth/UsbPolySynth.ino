/*
  UsbPolySynth - 6-voice USB MIDI polysynth for the mumunator RP2040 hardware.

  Audio: I2S BCLK GPIO2, DIN GPIO4, 48 kHz mono mirrored to stereo.
  Controls: four page buttons on GPIO6..9, four pots on ADC GPIO26..29.

  Pages:
    1. Oscillator: shape, timbre, color, master level.
    2. Filter envelope: attack, decay, sustain, release.
    3. VCA envelope: attack, decay, sustain, release.
    4. Filter: cutoff, resonance, envelope amount, key tracking.

  Hold buttons 1 and 4 while plugging in USB to jump to the UF2 bootloader.
*/

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <I2S.h>
#include <STMLIB.h>
#include <BRAIDS.h>
#include <stmlib/algorithms/voice_allocator.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include <cstring>

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint8_t BLOCK_SIZE = 24;
constexpr uint8_t NUM_VOICES = 6;
constexpr uint8_t NUM_POTS = 4;
constexpr uint8_t NUM_PAGES = 4;
constexpr uint8_t NUM_SHAPES = 8;
constexpr uint16_t POT_MAX = 4095;
constexpr uint16_t Q15_UNITY = 32767;
constexpr uint32_t Q24_UNITY = 0x01000000u;
constexpr uint8_t MIDI_MAX = 127;

constexpr uint8_t I2S_BCLK_PIN = 2;
constexpr uint8_t I2S_DIN_PIN = 4;

constexpr uint8_t PIN_POT_0 = 26;
constexpr uint8_t PIN_POT_1 = 27;
constexpr uint8_t PIN_POT_2 = 28;
constexpr uint8_t PIN_POT_3 = 29;

constexpr uint8_t PIN_BTN_0 = 6;
constexpr uint8_t PIN_BTN_1 = 7;
constexpr uint8_t PIN_BTN_2 = 8;
constexpr uint8_t PIN_BTN_3 = 9;
constexpr uint8_t PIN_STATUS_LED = 14;

constexpr uint16_t POT_PICKUP_THRESHOLD = 80;
constexpr uint8_t POT_IIR_SHIFT = 3;
constexpr uint32_t INPUT_SCAN_INTERVAL_MS = 2;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr uint32_t LED_PAGE_PULSE_MS = 90;
constexpr uint32_t LED_PICKUP_BLINK_MS = 120;
constexpr uint32_t MICROSECONDS_PER_SECOND = 1000000UL;
constexpr uint8_t STARTUP_DECLICK_BLOCKS = 8;
constexpr uint8_t I2S_INIT_MAX_RETRIES = 3;
constexpr uint32_t I2S_INIT_RETRY_DELAY_MS = 100;
constexpr uint32_t ENV_ATTACK_MIN_MS = 2;
constexpr uint32_t ENV_DECAY_MIN_MS = 80;
constexpr uint32_t ENV_RELEASE_MIN_MS = 60;
constexpr uint16_t CUTOFF_GATE_THRESHOLD_Q15 = 800;
constexpr uint16_t RESONANCE_MAX_Q15 = 24000;
constexpr uint16_t DEFAULT_MASTER_Q15 = 18000;
constexpr uint16_t STARTUP_MIN_MASTER_ADC = 1200;
constexpr uint16_t STARTUP_MIN_VCA_SUSTAIN_ADC = 1600;
constexpr uint16_t STARTUP_MIN_FILTER_CUTOFF_ADC = 1400;
constexpr uint16_t STARTUP_MAX_VCA_ATTACK_ADC = 512;
constexpr uint16_t FILTER_BYPASS_CUTOFF_Q15 = 30000;

struct ShapeConfig {
  braids::MacroOscillatorShape shape;
  int16_t timbre;
  int16_t color;
};

static const ShapeConfig SHAPES[NUM_SHAPES] = {
  { braids::MACRO_OSC_SHAPE_CSAW,          16384, 16384 },
  { braids::MACRO_OSC_SHAPE_SAW_SQUARE,    16384, 16384 },
  { braids::MACRO_OSC_SHAPE_SINE_TRIANGLE, 16384, 16384 },
  { braids::MACRO_OSC_SHAPE_BUZZ,          18000, 12000 },
  { braids::MACRO_OSC_SHAPE_SAW_SWARM,     18000, 24000 },
  { braids::MACRO_OSC_SHAPE_FM,            20000, 10000 },
  { braids::MACRO_OSC_SHAPE_FEEDBACK_FM,   18000,  8000 },
  { braids::MACRO_OSC_SHAPE_WAVETABLES,    16384, 16384 },
};

enum Page : uint8_t {
  PAGE_OSC = 0,
  PAGE_FILTER_ENV = 1,
  PAGE_VCA_ENV = 2,
  PAGE_FILTER = 3,
};

enum EnvStage : uint8_t {
  ENV_OFF = 0,
  ENV_ATTACK = 1,
  ENV_DECAY = 2,
  ENV_SUSTAIN = 3,
  ENV_RELEASE = 4,
};

struct AdsrSettings {
  uint32_t attack_ms;
  uint32_t decay_ms;
  uint32_t release_ms;
  uint32_t sustain_q24;
};

struct EnvState {
  uint8_t stage;
  uint32_t level_q24;
  uint32_t attack_step_q24;
  uint32_t decay_step_q24;
  uint32_t release_step_q24;
};

struct SynthVoice {
  braids::MacroOscillator* osc;
  int16_t buffer[BLOCK_SIZE];
  uint8_t sync_buffer[BLOCK_SIZE];
  EnvState vca_env;
  EnvState filter_env;
  uint8_t note;
  uint8_t velocity;
  bool active;
  bool held;
  int32_t svf_lp;
  int32_t svf_bp;
  braids::MacroOscillatorShape applied_shape;
  int16_t applied_pitch;
  int16_t applied_timbre;
  int16_t applied_color;
};

struct PotPickup {
  uint16_t target;
  bool active;
};

struct ButtonState {
  bool raw_pressed;
  bool stable_pressed;
  uint32_t last_change_ms;
};

I2S i2s(OUTPUT, I2S_BCLK_PIN, I2S_DIN_PIN);
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

static braids::MacroOscillator osc_pool[NUM_VOICES];
static SynthVoice voices[NUM_VOICES];
static stmlib::VoiceAllocator<NUM_VOICES> voice_allocator;

static const uint8_t POT_PINS[NUM_POTS] = {PIN_POT_0, PIN_POT_1, PIN_POT_2, PIN_POT_3};
static const uint8_t BTN_PINS[NUM_POTS] = {PIN_BTN_0, PIN_BTN_1, PIN_BTN_2, PIN_BTN_3};

static uint16_t pot_smoothed[NUM_POTS] = {};
static uint16_t page_param_adc[NUM_PAGES][NUM_POTS] = {
  { 256, 2048, 2048, 2800 },   // Osc: shape, timbre, color, master level.
  { 128, 1100, 1600, 900 },    // Filter env: fast attack, medium sustain.
  { 128, 900, 3400, 900 },     // VCA env: immediate, sustained, audible.
  { 4095, 350, 0, 0 },         // Filter: bypass-open, low resonance, no keytrack surprise.
};
static PotPickup pot_pickup[NUM_POTS] = {};
static ButtonState buttons[NUM_POTS] = {};

static Page current_page = PAGE_OSC;
static uint8_t current_shape_index = 0;
static int16_t current_timbre = 16384;
static int16_t current_color = 16384;
static uint16_t master_level_q15 = DEFAULT_MASTER_Q15;
static uint16_t filter_cutoff_q15 = 24000;
static uint16_t filter_resonance_q15 = 4000;
static uint16_t filter_env_amount_q15 = 14000;
static uint16_t filter_keytrack_q15 = 0;
static AdsrSettings vca_settings = {0, 250, 250, (uint32_t)(0.8f * Q24_UNITY)};
static AdsrSettings filter_settings = {0, 300, 300, 0};

static bool midi_ready = false;
static uint8_t led_page_toggles_remaining = 0;
static bool led_state = false;
static uint32_t led_next_ms = 0;
static bool pickup_blink_state = false;
static uint32_t pickup_led_next_ms = 0;

static volatile uint32_t render_pending_blocks = 0;
static volatile uint32_t audio_missed_blocks = 0;
static uint8_t startup_declick_blocks = STARTUP_DECLICK_BLOCKS;

static uint32_t alarm_period_us = 0;
static uint32_t alarm_frac_us_num = 0;
static uint32_t alarm_frac_us_den = 1;
static uint32_t alarm_frac_accum = 0;

static inline void serviceUsbStack() {
  TinyUSBDevice.task();
  yield();
}

static inline void serviceUsb() {
  serviceUsbStack();
  if (midi_ready) MIDI.read();
}

static inline uint16_t adcToQ15(uint16_t adc) {
  return (uint16_t)((uint32_t)adc * (uint32_t)Q15_UNITY / (uint32_t)POT_MAX);
}

static inline uint8_t adcToMidi(uint16_t adc) {
  return (uint8_t)((uint32_t)adc * (uint32_t)MIDI_MAX / (uint32_t)POT_MAX);
}

static inline uint8_t adcToShapeIndex(uint16_t adc) {
  uint8_t index = (uint8_t)((uint32_t)adc * (uint32_t)NUM_SHAPES / ((uint32_t)POT_MAX + 1u));
  if (index >= NUM_SHAPES) index = NUM_SHAPES - 1;
  return index;
}

static uint32_t adcToEnvelopeMs(uint16_t adc, uint32_t max_ms, uint32_t min_ms = 0) {
  const uint32_t x = adc;
  const uint32_t span_ms = max_ms > min_ms ? max_ms - min_ms : 0;
  return min_ms + (uint32_t)(((uint64_t)x * (uint64_t)x * (uint64_t)span_ms) / ((uint64_t)POT_MAX * (uint64_t)POT_MAX));
}

static inline uint16_t potSmooth(uint16_t smoothed, uint16_t raw) {
  return (uint16_t)(smoothed - (smoothed >> POT_IIR_SHIFT) + (raw >> POT_IIR_SHIFT));
}

static inline bool potPickupActive(PotPickup* pickup, uint16_t adc) {
  if (pickup->active) return true;
  int32_t diff = (int32_t)adc - (int32_t)pickup->target;
  if (diff < 0) diff = -diff;
  if ((uint16_t)diff <= POT_PICKUP_THRESHOLD) pickup->active = true;
  return pickup->active;
}

static bool allPotsPickedUp() {
  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    if (!pot_pickup[i].active) return false;
  }
  return true;
}

static void enforceAudibleStartupPatch() {
  if (page_param_adc[PAGE_OSC][3] < STARTUP_MIN_MASTER_ADC) {
    page_param_adc[PAGE_OSC][3] = STARTUP_MIN_MASTER_ADC;
  }
  if (page_param_adc[PAGE_VCA_ENV][0] > STARTUP_MAX_VCA_ATTACK_ADC) {
    page_param_adc[PAGE_VCA_ENV][0] = STARTUP_MAX_VCA_ATTACK_ADC;
  }
  if (page_param_adc[PAGE_VCA_ENV][2] < STARTUP_MIN_VCA_SUSTAIN_ADC) {
    page_param_adc[PAGE_VCA_ENV][2] = STARTUP_MIN_VCA_SUSTAIN_ADC;
  }
  if (page_param_adc[PAGE_FILTER][0] < STARTUP_MIN_FILTER_CUTOFF_ADC) {
    page_param_adc[PAGE_FILTER][0] = STARTUP_MIN_FILTER_CUTOFF_ADC;
  }
}

static uint32_t msToSamples(uint32_t ms) {
  return (uint32_t)(((uint64_t)ms * (uint64_t)SAMPLE_RATE) / 1000u);
}

static uint32_t stepForDelta(uint32_t delta_q24, uint32_t ms) {
  if (delta_q24 == 0) return 0;
  if (ms == 0) return delta_q24;
  const uint32_t samples = msToSamples(ms);
  if (samples == 0) return delta_q24;
  uint32_t step = delta_q24 / samples;
  if (step == 0) step = 1;
  return step;
}

static void envGateOn(EnvState* env, const AdsrSettings* settings) {
  env->attack_step_q24 = stepForDelta(Q24_UNITY, settings->attack_ms);
  env->decay_step_q24 = stepForDelta(Q24_UNITY - settings->sustain_q24, settings->decay_ms);
  env->release_step_q24 = 0;
  if (settings->attack_ms == 0) {
    env->level_q24 = Q24_UNITY;
    env->stage = ENV_DECAY;
  } else {
    env->level_q24 = 0;
    env->stage = ENV_ATTACK;
  }
}

static void envGateOff(EnvState* env, const AdsrSettings* settings) {
  if (env->stage == ENV_OFF) return;
  env->release_step_q24 = stepForDelta(env->level_q24, settings->release_ms);
  env->stage = ENV_RELEASE;
}

static uint32_t envProcessSample(EnvState* env, const AdsrSettings* settings) {
  if (env->stage == ENV_OFF) return 0;

  if (env->stage == ENV_ATTACK) {
    const uint32_t step = env->attack_step_q24;
    if (Q24_UNITY - env->level_q24 <= step) {
      env->level_q24 = Q24_UNITY;
      env->stage = ENV_DECAY;
    } else {
      env->level_q24 += step;
    }
  } else if (env->stage == ENV_DECAY) {
    const uint32_t sustain = settings->sustain_q24;
    if (env->level_q24 <= sustain || settings->decay_ms == 0) {
      env->level_q24 = sustain;
      env->stage = ENV_SUSTAIN;
    } else {
      const uint32_t step = env->decay_step_q24;
      if (env->level_q24 - sustain <= step) {
        env->level_q24 = sustain;
        env->stage = ENV_SUSTAIN;
      } else {
        env->level_q24 -= step;
      }
    }
  } else if (env->stage == ENV_SUSTAIN) {
    env->level_q24 = settings->sustain_q24;
  } else if (env->stage == ENV_RELEASE) {
    if (settings->release_ms == 0 || env->level_q24 <= env->release_step_q24) {
      env->level_q24 = 0;
      env->stage = ENV_OFF;
    } else {
      env->level_q24 -= env->release_step_q24;
    }
  }

  return env->level_q24;
}

static inline int16_t clipToInt16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

static int32_t svfCutoffToF(uint16_t cutoff_q15) {
  constexpr int32_t F_MAX_Q15 = 56000;
  return (int32_t)((uint32_t)cutoff_q15 * (uint32_t)F_MAX_Q15 / 32767u);
}

static int32_t svfResonanceToQ(uint16_t res_q15) {
  const int32_t inv_q = (int32_t)65536 - (int32_t)((uint32_t)res_q15 * 63488u / 32767u);
  if (inv_q < 128) return 128;
  if (inv_q > 65536) return 65536;
  return inv_q;
}

static uint16_t voiceCutoff(const SynthVoice* voice, uint32_t filter_env_level_q24) {
  int32_t cutoff = filter_cutoff_q15;
  cutoff += (int32_t)(((filter_env_level_q24 >> 9) * (uint32_t)filter_env_amount_q15) >> 15);
  cutoff += ((int32_t)voice->note - 60) * (int32_t)filter_keytrack_q15 / 60;
  if (cutoff < 0) cutoff = 0;
  if (cutoff > 32767) cutoff = 32767;
  return (uint16_t)cutoff;
}

static void applyParametersFromPageState() {
  current_shape_index = adcToShapeIndex(page_param_adc[PAGE_OSC][0]);
  current_timbre = (int16_t)adcToQ15(page_param_adc[PAGE_OSC][1]);
  current_color = (int16_t)adcToQ15(page_param_adc[PAGE_OSC][2]);
  master_level_q15 = adcToQ15(page_param_adc[PAGE_OSC][3]);

  filter_settings.attack_ms = adcToEnvelopeMs(page_param_adc[PAGE_FILTER_ENV][0], 5000, ENV_ATTACK_MIN_MS);
  filter_settings.decay_ms = adcToEnvelopeMs(page_param_adc[PAGE_FILTER_ENV][1], 6000, ENV_DECAY_MIN_MS);
  filter_settings.sustain_q24 = (uint32_t)(((uint64_t)adcToQ15(page_param_adc[PAGE_FILTER_ENV][2]) * (uint64_t)Q24_UNITY) >> 15);
  filter_settings.release_ms = adcToEnvelopeMs(page_param_adc[PAGE_FILTER_ENV][3], 8000, ENV_RELEASE_MIN_MS);

  vca_settings.attack_ms = adcToEnvelopeMs(page_param_adc[PAGE_VCA_ENV][0], 5000, ENV_ATTACK_MIN_MS);
  vca_settings.decay_ms = adcToEnvelopeMs(page_param_adc[PAGE_VCA_ENV][1], 6000, ENV_DECAY_MIN_MS);
  vca_settings.sustain_q24 = (uint32_t)(((uint64_t)adcToQ15(page_param_adc[PAGE_VCA_ENV][2]) * (uint64_t)Q24_UNITY) >> 15);
  vca_settings.release_ms = adcToEnvelopeMs(page_param_adc[PAGE_VCA_ENV][3], 8000, ENV_RELEASE_MIN_MS);

  filter_cutoff_q15 = adcToQ15(page_param_adc[PAGE_FILTER][0]);
  filter_resonance_q15 = (uint16_t)((uint32_t)adcToQ15(page_param_adc[PAGE_FILTER][1]) * RESONANCE_MAX_Q15 / Q15_UNITY);
  filter_env_amount_q15 = adcToQ15(page_param_adc[PAGE_FILTER][2]);
  filter_keytrack_q15 = adcToQ15(page_param_adc[PAGE_FILTER][3]);
}

static void startPageLedPulse() {
  led_page_toggles_remaining = (uint8_t)((current_page + 1u) * 2u);
  led_state = false;
  led_next_ms = 0;
}

static void lockPotsToCurrentPage() {
  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    pot_pickup[i].target = page_param_adc[current_page][i];
    pot_pickup[i].active = false;
  }
}

static void setPage(Page page) {
  if (page == current_page) return;
  current_page = page;
  lockPotsToCurrentPage();
  startPageLedPulse();
}

static void updateLed() {
  const uint32_t now_ms = millis();
  if (led_page_toggles_remaining > 0) {
    if (now_ms >= led_next_ms) {
      led_state = !led_state;
      digitalWrite(PIN_STATUS_LED, led_state ? HIGH : LOW);
      led_page_toggles_remaining--;
      led_next_ms = now_ms + LED_PAGE_PULSE_MS;
    }
    return;
  }

  if (!allPotsPickedUp()) {
    if (now_ms >= pickup_led_next_ms) {
      pickup_blink_state = !pickup_blink_state;
      digitalWrite(PIN_STATUS_LED, pickup_blink_state ? HIGH : LOW);
      pickup_led_next_ms = now_ms + LED_PICKUP_BLINK_MS;
    }
    return;
  }

  digitalWrite(PIN_STATUS_LED, HIGH);
}

static void initVoice(SynthVoice* voice, uint8_t index) {
  voice->osc = &osc_pool[index];
  voice->osc->Init((float)SAMPLE_RATE);
  voice->osc->set_shape(SHAPES[0].shape);
  voice->osc->set_pitch((int16_t)(60 << 7));
  voice->osc->set_parameters(SHAPES[0].timbre, SHAPES[0].color);
  voice->vca_env = {ENV_OFF, 0, 0};
  voice->filter_env = {ENV_OFF, 0, 0};
  voice->note = 60;
  voice->velocity = 0;
  voice->active = false;
  voice->held = false;
  voice->svf_lp = 0;
  voice->svf_bp = 0;
  voice->applied_shape = SHAPES[0].shape;
  voice->applied_pitch = (int16_t)(60 << 7);
  voice->applied_timbre = SHAPES[0].timbre;
  voice->applied_color = SHAPES[0].color;
  memset(voice->buffer, 0, sizeof(voice->buffer));
  memset(voice->sync_buffer, 0, sizeof(voice->sync_buffer));
}

static void noteOnVoice(uint8_t note, uint8_t velocity) {
  const uint8_t voice_index = voice_allocator.NoteOn(note, stmlib::VOICE_STEALING_MODE_LRU);
  if (voice_index == stmlib::NOT_ALLOCATED || voice_index >= NUM_VOICES) return;
  SynthVoice* voice = &voices[voice_index];
  voice->note = note;
  voice->velocity = velocity;
  voice->active = true;
  voice->held = true;
  voice->svf_lp = 0;
  voice->svf_bp = 0;
  envGateOn(&voice->vca_env, &vca_settings);
  envGateOn(&voice->filter_env, &filter_settings);
  voice->osc->Strike();
}

static void noteOffVoice(uint8_t note) {
  const uint8_t voice_index = voice_allocator.NoteOff(note);
  if (voice_index == stmlib::NOT_ALLOCATED || voice_index >= NUM_VOICES) return;
  SynthVoice* voice = &voices[voice_index];
  voice->held = false;
  envGateOff(&voice->vca_env, &vca_settings);
  envGateOff(&voice->filter_env, &filter_settings);
}

static void releaseAllVoices() {
  voice_allocator.ClearNotes();
  for (uint8_t i = 0; i < NUM_VOICES; ++i) {
    voices[i].held = false;
    envGateOff(&voices[i].vca_env, &vca_settings);
    envGateOff(&voices[i].filter_env, &filter_settings);
  }
}

static void handleNoteOn(byte channel, byte note, byte velocity) {
  (void)channel;
  if (velocity == 0) {
    noteOffVoice(note);
  } else {
    noteOnVoice(note, velocity);
  }
}

static void handleNoteOff(byte channel, byte note, byte velocity) {
  (void)channel;
  (void)velocity;
  noteOffVoice(note);
}

static void handleControlChange(byte channel, byte number, byte value) {
  (void)channel;
  if (number == 120 || number == 123) {
    releaseAllVoices();
  } else if (number == 7) {
    page_param_adc[PAGE_OSC][3] = (uint16_t)((uint32_t)value * POT_MAX / MIDI_MAX);
    applyParametersFromPageState();
  }
}

static void renderVoice(SynthVoice* voice) {
  if (!voice->active) return;

  const ShapeConfig& shape = SHAPES[current_shape_index];
  if (shape.shape != voice->applied_shape) {
    voice->osc->set_shape(shape.shape);
    voice->applied_shape = shape.shape;
  }
  if (current_timbre != voice->applied_timbre || current_color != voice->applied_color) {
    voice->osc->set_parameters(current_timbre, current_color);
    voice->applied_timbre = current_timbre;
    voice->applied_color = current_color;
  }

  const int16_t pitch = (int16_t)(voice->note << 7);
  if (pitch != voice->applied_pitch) {
    voice->osc->set_pitch(pitch);
    voice->applied_pitch = pitch;
  }

  memset(voice->sync_buffer, 0, sizeof(voice->sync_buffer));
  voice->osc->Render(voice->sync_buffer, voice->buffer, BLOCK_SIZE);

  int32_t lp = voice->svf_lp;
  int32_t bp = voice->svf_bp;
  const int32_t inv_q = svfResonanceToQ(filter_resonance_q15);
  const uint16_t velocity_q15 = (uint16_t)((uint32_t)voice->velocity * 258u);

  for (uint8_t i = 0; i < BLOCK_SIZE; ++i) {
    const uint32_t filter_env = envProcessSample(&voice->filter_env, &filter_settings);
    const uint32_t vca_env = envProcessSample(&voice->vca_env, &vca_settings);
    const uint16_t cutoff = voiceCutoff(voice, filter_env);
    const int32_t x = (int32_t)voice->buffer[i];

    int32_t out = x;
    if (cutoff < FILTER_BYPASS_CUTOFF_Q15) {
      const int32_t f12 = svfCutoffToF(cutoff) >> 3;

      lp += (f12 * bp) >> 12;
      if (lp > 32767) lp = 32767;
      if (lp < -32768) lp = -32768;

      const int32_t hp = x - lp - ((inv_q * bp) >> 15);
      bp += (f12 * hp) >> 12;
      if (bp > 32767) bp = 32767;
      if (bp < -32768) bp = -32768;

      out = lp;
    } else {
      lp = 0;
      bp = 0;
    }

    out = (out * (int32_t)(vca_env >> 9)) >> 15;
    out = (out * (int32_t)velocity_q15) >> 15;
    voice->buffer[i] = clipToInt16(out);
  }

  voice->svf_lp = lp;
  voice->svf_bp = bp;

  if (!voice->held && voice->vca_env.stage == ENV_OFF) {
    voice->active = false;
  }
}

static void renderAudioBlock() {
  serviceUsbStack();

  if (startup_declick_blocks > 0) {
    startup_declick_blocks--;
    for (uint8_t i = 0; i < BLOCK_SIZE; ++i) {
      i2s.write((int16_t)0);
      i2s.write((int16_t)0);
    }
    return;
  }

  for (uint8_t v = 0; v < NUM_VOICES; ++v) {
    renderVoice(&voices[v]);
    if ((v & 1u) == 1u) serviceUsbStack();
  }

  for (uint8_t i = 0; i < BLOCK_SIZE; ++i) {
    int32_t mix = 0;
    uint8_t active_count = 0;
    for (uint8_t v = 0; v < NUM_VOICES; ++v) {
      if (voices[v].active) {
        mix += voices[v].buffer[i];
        active_count++;
      }
    }
    if (active_count == 0) active_count = 1;

    mix = (mix * (int32_t)(65536u / active_count)) >> 16;
    if (filter_cutoff_q15 < CUTOFF_GATE_THRESHOLD_Q15) {
      mix = (mix * (int32_t)filter_cutoff_q15) / CUTOFF_GATE_THRESHOLD_Q15;
    }
    mix = (mix * (int32_t)master_level_q15) >> 15;

    if (mix > 24576) {
      const int32_t over = mix - 24576;
      mix = 24576 + (over * 6144) / (over + 12288);
    } else if (mix < -24576) {
      const int32_t over = -24576 - mix;
      mix = -24576 - (over * 6144) / (over + 12288);
    }

    const int16_t out = clipToInt16(mix);
    if ((i & 0x07u) == 0u) serviceUsbStack();
    i2s.write(out);
    i2s.write(out);
  }
}

static void scanPots() {
  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    pot_smoothed[i] = potSmooth(pot_smoothed[i], analogRead(POT_PINS[i]));
    if (potPickupActive(&pot_pickup[i], pot_smoothed[i])) {
      page_param_adc[current_page][i] = pot_smoothed[i];
    }
  }
  applyParametersFromPageState();
}

static void scanButtons(uint32_t now_ms) {
  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    const bool raw_pressed = (digitalRead(BTN_PINS[i]) == LOW);
    if (raw_pressed != buttons[i].raw_pressed) {
      buttons[i].raw_pressed = raw_pressed;
      buttons[i].last_change_ms = now_ms;
    }

    if ((now_ms - buttons[i].last_change_ms) < BUTTON_DEBOUNCE_MS) continue;
    if (raw_pressed == buttons[i].stable_pressed) continue;

    buttons[i].stable_pressed = raw_pressed;
    if (raw_pressed) {
      setPage((Page)i);
    }
  }
}

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

void setup() {
  set_sys_clock_khz(250000, true);
  analogReadResolution(12);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }

  delay(50);
  if (digitalRead(PIN_BTN_0) == LOW && digitalRead(PIN_BTN_3) == LOW) {
    reset_usb_boot(0, 0);
  }

  Serial.begin(115200);
  usb_midi.setStringDescriptor("UsbPolySynth MIDI");
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleControlChange);
  midi_ready = true;
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  for (uint8_t i = 0; i < NUM_POTS; ++i) {
    pot_smoothed[i] = analogRead(POT_PINS[i]);
  }
  enforceAudibleStartupPatch();
  lockPotsToCurrentPage();

  voice_allocator.Init();
  voice_allocator.set_size(NUM_VOICES);
  for (uint8_t i = 0; i < NUM_VOICES; ++i) {
    initVoice(&voices[i], i);
  }
  applyParametersFromPageState();

  i2s.setBitsPerSample(16);
  for (uint8_t r = 0; r < I2S_INIT_MAX_RETRIES; ++r) {
    if (i2s.begin(SAMPLE_RATE)) break;
    delay(I2S_INIT_RETRY_DELAY_MS);
  }

  hw_set_bits(&timer_hw->inte, 1u << 0);
  irq_set_exclusive_handler(TIMER_IRQ_0, audio_timer_callback);
  irq_set_enabled(TIMER_IRQ_0, true);
  alarm_period_us = (MICROSECONDS_PER_SECOND * (uint32_t)BLOCK_SIZE) / SAMPLE_RATE;
  alarm_frac_us_num = (MICROSECONDS_PER_SECOND * (uint32_t)BLOCK_SIZE) % SAMPLE_RATE;
  alarm_frac_us_den = SAMPLE_RATE;
  alarm_frac_accum = 0;
  timer_hw->intr = 1u << 0;
  timer_hw->alarm[0] = timer_hw->timerawl + alarm_period_us;

  startPageLedPulse();
}

void loop() {
  serviceUsb();

  uint8_t renders = 0;
  while (render_pending_blocks > 0 && renders < 4u) {
    noInterrupts();
    render_pending_blocks--;
    interrupts();
    renderAudioBlock();
    renders++;
  }

  const uint32_t now_ms = millis();
  static uint32_t last_scan_ms = 0;
  if ((now_ms - last_scan_ms) >= INPUT_SCAN_INTERVAL_MS) {
    last_scan_ms = now_ms;
    scanPots();
    scanButtons(now_ms);
  }

  updateLed();
  serviceUsb();
}
