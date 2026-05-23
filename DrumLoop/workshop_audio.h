#pragma once

#include <Arduino.h>
#include <STMLIB.h>
#include <BRAIDS.h>

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint8_t BLOCK_SIZE = 32;

constexpr uint32_t Q32_UNITY = 0xFFFFFFFFu;
constexpr uint16_t Q15_UNITY = 32767;
constexpr int16_t Q15_MIDPOINT = 16384;
constexpr uint8_t MIDI_MAX = 127;
constexpr uint32_t Q16_UNITY = 65535u;

constexpr int32_t AUDIO_MAX = 32767;
constexpr int32_t AUDIO_MIN = -32768;

constexpr uint32_t DECAY_FAST = 0xE8000000u;
constexpr uint32_t DECAY_SLOW = 0xFF900000u;

constexpr uint32_t DECAY_KICK_DEFAULT   = 0xFE000000u;   // longer tail for body
constexpr uint32_t DECAY_SNARE_DEFAULT  = 0xFD000000u;
constexpr uint32_t DECAY_HAT_DEFAULT = 0xF0000000u;
constexpr uint32_t DECAY_PERC_DEFAULT = 0xF7000000u;
constexpr uint32_t DECAY_ENVELOPE_DEFAULT = 0xF0000000u;

// Transient (click) layer decays -- fast 5-30ms layer summed on top of body.
constexpr uint32_t TRANS_DECAY_KICK  = 0xE0000000u;
constexpr uint32_t TRANS_DECAY_SNARE = 0xE4000000u;
constexpr uint32_t TRANS_DECAY_HAT   = 0xCC000000u;
constexpr uint32_t TRANS_DECAY_PERC  = 0xDC000000u;

// Kick pitch-envelope decay (body 'thump').
constexpr uint32_t PITCH_ENV_DECAY_KICK = 0xE8000000u;

// Hi-hat specific decay range -- wider than generic DECAY_FAST/SLOW,
// and distinct bounds for closed vs open so the character is obvious.
constexpr uint32_t HAT_DECAY_CLOSED_MIN = 0xFB000000u;  // tight closed tick
constexpr uint32_t HAT_DECAY_CLOSED_MAX = 0xFF200000u;  // long closed body
constexpr uint32_t HAT_DECAY_OPEN_MIN   = 0xFC800000u;  // short open
constexpr uint32_t HAT_DECAY_OPEN_MAX   = 0xFFC00000u;  // long washy open (~3 s to -40 dB)

enum FiltType : uint8_t { FILT_OFF = 0, FILT_LP = 1, FILT_HP = 2 };

struct DrumVoice {
  braids::MacroOscillator* osc;
  int16_t buffer[BLOCK_SIZE];
  uint8_t sync_buffer[BLOCK_SIZE];
  uint8_t shape;
  int16_t pitch;            // nominal base pitch; pitch env is added on top
  int16_t timbre;
  int16_t color;
  bool last_trig;
  bool trigger_pending;

  // Two-stage amplitude envelope: body + transient are summed at block rate.
  uint32_t prev_amp;
  uint32_t amp;
  uint32_t amp_decay;
  uint32_t prev_trans_amp;
  uint32_t trans_amp;
  uint32_t trans_decay;
  int16_t  trans_gain_q15;  // how much transient is mixed on top of body (Q15)

  // Optional pitch envelope (mostly for kick). range_q7 = semitones * 128.
  uint32_t prev_pitch_env_amp;
  uint32_t pitch_env_amp;
  uint32_t pitch_env_decay;
  int16_t  pitch_env_range_q7;

  // Simple per-voice 1-pole filter, applied in updateDrum after Render.
  uint8_t filt_type;        // FiltType
  int16_t filt_k_q15;       // coefficient; higher = higher cutoff
  int32_t filt_state;

  // Per-voice output gain (applied in mixer).
  int16_t gain_q15;

  // Per-trigger velocity jitter (Q15). Randomised on every rising-edge
  // trigger so repeated hits of the same voice don't sound identical.
  int16_t velocity_q15;

  bool decay_override_pending;
  uint32_t next_amp_decay;
};

void initDrumVoice(DrumVoice* voice, uint8_t shape, int16_t pitch);
void triggerDrum(DrumVoice* voice);
void updateDrum(DrumVoice* voice);

static inline int16_t ccToPitch(uint8_t cc, uint8_t min_note, uint8_t max_note) {
  const uint32_t note = (uint32_t)min_note +
                        ((uint32_t)cc * (uint32_t)(max_note - min_note) / (uint32_t)MIDI_MAX);
  return (int16_t)(note << 7);
}

static inline uint32_t lerpStateU32(uint32_t a, uint32_t b, uint32_t t_q16) {
  return (uint32_t)((int64_t)a + ((((int64_t)b - (int64_t)a) * (int64_t)t_q16) >> 16));
}

static inline uint32_t lerpU32(uint32_t a, uint32_t b, uint32_t t_q16) {
  return a + (uint32_t)((((uint64_t)(b - a)) * (uint64_t)t_q16) >> 16);
}

static inline uint32_t decayFromCC(uint8_t cc) {
  const uint32_t t_q16 = (uint32_t)cc * Q16_UNITY / (uint32_t)MIDI_MAX;
  return lerpU32(DECAY_FAST, DECAY_SLOW, t_q16);
}

// Hi-hat decay: character (closed vs open) selects the decay-range bucket,
// and the global 'hat decay' pot sweeps within that bucket. This way the
// open hat is always meaningfully longer than the closed hat, and the pot
// shapes character instead of being fighting with it.
static inline uint32_t hatDecayFromCC(uint8_t global_cc, uint8_t local_cc) {
  const bool open = local_cc > 64;
  const uint32_t lo = open ? HAT_DECAY_OPEN_MIN : HAT_DECAY_CLOSED_MIN;
  const uint32_t hi = open ? HAT_DECAY_OPEN_MAX : HAT_DECAY_CLOSED_MAX;
  const uint32_t t_q16 = (uint32_t)global_cc * Q16_UNITY / (uint32_t)MIDI_MAX;
  return lerpU32(lo, hi, t_q16);
}

// Combined envelope (body + transient*trans_gain) at block rate, returned in Q15.
static inline uint16_t voiceEnvQ15AtSample(const DrumVoice* v, uint8_t sample_index) {
  const uint32_t t_q16 = (BLOCK_SIZE > 1)
      ? ((uint32_t)sample_index * Q16_UNITY / (uint32_t)(BLOCK_SIZE - 1))
      : Q16_UNITY;
  const uint32_t body = (uint32_t)(lerpStateU32(v->prev_amp, v->amp, t_q16) >> 17);
  const uint32_t trans_env = (uint32_t)(lerpStateU32(v->prev_trans_amp, v->trans_amp, t_q16) >> 17);
  const int32_t trans = (int32_t)((trans_env * (uint32_t)(uint16_t)v->trans_gain_q15) >> 15);
  int32_t sum = (int32_t)body + trans;
  if (sum > 32767) sum = 32767;
  if (sum < 0) sum = 0;
  return (uint16_t)sum;
}

static inline int32_t saturatingAdd(int32_t a, int32_t b) {
  const int32_t sum = a + b;
  if (sum > AUDIO_MAX) return AUDIO_MAX;
  if (sum < AUDIO_MIN) return AUDIO_MIN;
  return sum;
}

static inline int16_t clipToInt16(int32_t sample) {
  if (sample > AUDIO_MAX) return (int16_t)AUDIO_MAX;
  if (sample < AUDIO_MIN) return (int16_t)AUDIO_MIN;
  return (int16_t)sample;
}
