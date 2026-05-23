#pragma once

#include <Arduino.h>
#include <STMLIB.h>
#include <BRAIDS.h>

#include "config.h"

// ============================================================================
// DRONE VOICE — one Braids oscillator + SVF state
// ============================================================================
//
// No envelopes. The voice is always audible; amplitude is controlled by the
// post-mix cutoff gate in Dronaldo.ino. setDronePitches() updates pitch
// legato (no retrigger) so button presses don't click.
// ============================================================================

struct DroneVoice {
  braids::MacroOscillator *osc;

  int16_t buffer[BLOCK_SIZE];
  uint8_t sync_buffer[BLOCK_SIZE];

  // Target parameters (written from input handler)
  int16_t pitch;        // Q7: midi_note << 7
  int16_t timbre;       // 0..32767
  int16_t color;        // 0..32767 — hardcoded per shape

  // Last-applied values to avoid redundant Braids calls
  int16_t applied_pitch;
  int16_t applied_timbre;
  int16_t applied_color;

  // SVF state
  int32_t svf_lp;
  int32_t svf_bp;

  // Per-voice "random-hold + glide" LFO for micro pitch wobble. Each voice
  // picks a new random target at random intervals and glides toward it; the
  // five voices drift independently so the ensemble breathes without any
  // single voice being obviously modulated.
  int16_t  lfo_value_q15;
  int16_t  lfo_target_q15;
  uint32_t lfo_samples_to_retarget;
  uint32_t lfo_rng;
};

// ---- Per-voice pitch-wobble LFO ----
// Depth is in Braids pitch units (1 semitone = 128). 8 -> ~6 cents peak.
constexpr int16_t  LFO_PITCH_DEPTH = 8;
// One-pole glide: step = (target - value) >> LFO_GLIDE_SHIFT applied per block.
// Shift 11 at 2000 blocks/s -> ~1 s time constant.
constexpr uint8_t  LFO_GLIDE_SHIFT = 11;
// Random retarget interval bounds.
constexpr uint32_t LFO_RETARGET_MIN_SAMPLES = (uint32_t)SAMPLE_RATE * 1u;
constexpr uint32_t LFO_RETARGET_MAX_SAMPLES = (uint32_t)SAMPLE_RATE * 4u;

// ---- Voice lifecycle ----
void voiceInit(DroneVoice *v, int16_t pitch, int16_t timbre);
void voiceRender(DroneVoice *v, uint16_t cutoff_q15, uint16_t resonance_q15);

// ---- Chord / parameter updates ----
void setDronePitches(DroneVoice voices[NUM_VOICES], uint8_t root_note,
                     uint8_t chord_type, int8_t inversion);
void setDroneTimbre(DroneVoice voices[NUM_VOICES], int16_t timbre);
void setDroneShape(DroneVoice voices[NUM_VOICES],
                   braids::MacroOscillatorShape shape,
                   int16_t timbre, int16_t color);

// ============================================================================
// SVF COEFFICIENTS (inline — see Murci audio_engine.h for derivation)
// ============================================================================

static inline int32_t svfCutoffToF(uint16_t cutoff_q15) {
  constexpr int32_t F_MAX_Q15 = 56000;
  return (int32_t)((uint32_t)cutoff_q15 * (uint32_t)F_MAX_Q15 / 32767u);
}

static inline int32_t svfResonanceToQ(uint16_t res_q15) {
  const int32_t inv_q = (int32_t)65536 -
      (int32_t)((uint32_t)res_q15 * 63488u / 32767u);
  if (inv_q < 128)   return 128;
  if (inv_q > 65536) return 65536;
  return inv_q;
}
