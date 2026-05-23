#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <I2S.h>
#include <math.h>

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

constexpr uint8_t LED_PIN = 14;

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint8_t BLOCK_SIZE = 32;
constexpr uint8_t VOICE_COUNT = 4;
constexpr uint16_t POT_MAX = 4095;
constexpr uint32_t PHASE_UNITY = 0xFFFFFFFFu;
constexpr int16_t Q15_UNITY = 32767;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr int16_t ATTACK_STEP_Q15 = 900;
constexpr int16_t RELEASE_STEP_Q15 = 450;
constexpr int16_t MASTER_GAIN_Q15 = 26000;

struct Voice {
  uint32_t phase;
  uint32_t phase_inc;
  int16_t amp_q15;
  int16_t target_amp_q15;
  bool raw_pressed;
  bool stable_pressed;
  uint32_t last_change_ms;
};

I2S i2s(OUTPUT, I2S_BCLK_PIN, I2S_DIN_PIN);
Voice voices[VOICE_COUNT] = {};
uint16_t pot_values[VOICE_COUNT] = {};

static inline void serviceUsb() {
  TinyUSBDevice.task();
  yield();
}

static inline int16_t clipToInt16(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return (int16_t)sample;
}

static inline int16_t triangleFromPhase(uint32_t phase) {
  const uint32_t x = phase >> 16;
  const int32_t half = (x < 32768u) ? (int32_t)x : (int32_t)(65535u - x);
  return (int16_t)((half << 1) - 32768);
}

static uint32_t frequencyToPhaseInc(float hz) {
  return (uint32_t)((hz * 4294967296.0f) / (float)SAMPLE_RATE);
}

static uint32_t potToPhaseInc(uint16_t pot) {
  const float normal = (float)pot / (float)POT_MAX;
  const float hz = 55.0f * powf(2.0f, normal * 5.0f);
  return frequencyToPhaseInc(hz);
}

static inline int16_t slewAmp(int16_t current, int16_t target) {
  if (current < target) {
    const int32_t next = (int32_t)current + ATTACK_STEP_Q15;
    return (next > target) ? target : (int16_t)next;
  }
  if (current > target) {
    const int32_t next = (int32_t)current - RELEASE_STEP_Q15;
    return (next < target) ? target : (int16_t)next;
  }
  return current;
}

static void readPots() {
  const uint8_t pins[VOICE_COUNT] = {PIN_POT_0, PIN_POT_1, PIN_POT_2, PIN_POT_3};
  for (uint8_t i = 0; i < VOICE_COUNT; ++i) {
    pot_values[i] = (uint16_t)analogRead(pins[i]);
  }
}

static void updateFrequencies() {
  for (uint8_t i = 0; i < VOICE_COUNT; ++i) {
    voices[i].phase_inc = potToPhaseInc(pot_values[i]);
  }
}

static void pollButtons() {
  const uint8_t pins[VOICE_COUNT] = {PIN_BTN_0, PIN_BTN_1, PIN_BTN_2, PIN_BTN_3};
  const uint32_t now_ms = millis();
  bool any_gate = false;

  for (uint8_t i = 0; i < VOICE_COUNT; ++i) {
    const bool raw_pressed = digitalRead(pins[i]) == LOW;
    if (raw_pressed != voices[i].raw_pressed) {
      voices[i].raw_pressed = raw_pressed;
      voices[i].last_change_ms = now_ms;
    }

    if ((now_ms - voices[i].last_change_ms) >= BUTTON_DEBOUNCE_MS && raw_pressed != voices[i].stable_pressed) {
      voices[i].stable_pressed = raw_pressed;
      voices[i].target_amp_q15 = raw_pressed ? Q15_UNITY : 0;
    }

    any_gate = any_gate || voices[i].stable_pressed;
  }

  digitalWrite(LED_PIN, any_gate ? HIGH : LOW);
}

static void renderAudioBlock() {
  serviceUsb();
  if (i2s.availableForWrite() < (int)(BLOCK_SIZE * 2)) return;

  for (uint8_t sample_index = 0; sample_index < BLOCK_SIZE; ++sample_index) {
    int32_t mix = 0;

    for (uint8_t voice_index = 0; voice_index < VOICE_COUNT; ++voice_index) {
      Voice* voice = &voices[voice_index];
      voice->amp_q15 = slewAmp(voice->amp_q15, voice->target_amp_q15);
      voice->phase += voice->phase_inc;
      const int32_t osc = triangleFromPhase(voice->phase);
      mix += (osc * (int32_t)voice->amp_q15) >> 15;
    }

    int32_t sample = mix >> 2;
    sample = (sample * (int32_t)MASTER_GAIN_Q15) >> 15;
    const int16_t out = clipToInt16(sample);

    if ((sample_index & 0x07u) == 0u) serviceUsb();
    i2s.write(out);
    i2s.write(out);
  }
}

void setup() {
  analogReadResolution(12);
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(PIN_BTN_0, INPUT_PULLUP);
  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BTN_2, INPUT_PULLUP);
  pinMode(PIN_BTN_3, INPUT_PULLUP);

  i2s.setBitsPerSample(16);
  i2s.begin(SAMPLE_RATE);

  readPots();
  updateFrequencies();

  for (uint8_t i = 0; i < VOICE_COUNT; ++i) {
    voices[i].phase = (uint32_t)i * (PHASE_UNITY / VOICE_COUNT);
  }
}

void loop() {
  serviceUsb();
  readPots();
  updateFrequencies();
  pollButtons();
  renderAudioBlock();
}
