#include "audio_engine.h"

#include <cstring>

// ============================================================================
// VOICE POOL
// ============================================================================

static braids::MacroOscillator osc_pool[NUM_VOICES];
static uint8_t osc_pool_next = 0;

void voiceInit(DroneVoice *v, int16_t pitch, int16_t timbre) {
  v->osc = &osc_pool[osc_pool_next < NUM_VOICES ? osc_pool_next++ : 0];
  v->osc->Init((float)SAMPLE_RATE);
  const int16_t init_color = DRONE_SHAPES[0].color;
  v->osc->set_shape(DRONE_SHAPES[0].shape);
  v->osc->set_pitch(pitch);
  v->osc->set_parameters(timbre, init_color);

  v->pitch          = pitch;
  v->timbre         = timbre;
  v->color          = init_color;
  v->applied_pitch  = pitch;
  v->applied_timbre = timbre;
  v->applied_color  = init_color;

  v->svf_lp = 0;
  v->svf_bp = 0;

  // Seed each voice's LFO with a distinct state (golden-ratio constant) so
  // their random-target timings are decorrelated from the first block.
  const uint8_t voice_idx = (osc_pool_next > 0) ? (uint8_t)(osc_pool_next - 1) : 0;
  v->lfo_rng = 0xC0FFEE00u + (uint32_t)voice_idx * 0x9E3779B9u;
  v->lfo_value_q15  = 0;
  v->lfo_rng = v->lfo_rng * 1664525u + 1013904223u;
  v->lfo_target_q15 = (int16_t)(v->lfo_rng >> 16);
  // Stagger first retarget across voices so not all five re-roll together.
  const uint32_t span = LFO_RETARGET_MAX_SAMPLES - LFO_RETARGET_MIN_SAMPLES;
  v->lfo_samples_to_retarget =
      LFO_RETARGET_MIN_SAMPLES + ((uint32_t)voice_idx * span) / NUM_VOICES;

  memset(v->buffer,      0, sizeof(v->buffer));
  memset(v->sync_buffer, 0, sizeof(v->sync_buffer));
}

// ============================================================================
// VOICE RENDER — one audio block
// ============================================================================

void voiceRender(DroneVoice *v, uint16_t cutoff_q15, uint16_t resonance_q15) {
  // --- Tick per-voice pitch-wobble LFO (once per block) ---
  // When the retarget deadline hits, pick a new random Q15 target and a new
  // random interval. Between rolls, the value glides toward the target via a
  // one-pole. Integer truncation at small diffs gives natural micro-plateaus.
  if (v->lfo_samples_to_retarget <= BLOCK_SIZE) {
    v->lfo_rng = v->lfo_rng * 1664525u + 1013904223u;
    v->lfo_target_q15 = (int16_t)(v->lfo_rng >> 16);
    v->lfo_rng = v->lfo_rng * 1664525u + 1013904223u;
    const uint32_t span = LFO_RETARGET_MAX_SAMPLES - LFO_RETARGET_MIN_SAMPLES;
    v->lfo_samples_to_retarget = LFO_RETARGET_MIN_SAMPLES + (v->lfo_rng % span);
  } else {
    v->lfo_samples_to_retarget -= BLOCK_SIZE;
  }
  const int32_t glide_step =
      ((int32_t)v->lfo_target_q15 - (int32_t)v->lfo_value_q15) >> LFO_GLIDE_SHIFT;
  v->lfo_value_q15 = (int16_t)((int32_t)v->lfo_value_q15 + glide_step);

  // Apply wobble as a tiny offset on top of the chord pitch.
  const int16_t pitch_mod =
      (int16_t)(((int32_t)v->lfo_value_q15 * (int32_t)LFO_PITCH_DEPTH) >> 15);
  const int16_t target_pitch = (int16_t)(v->pitch + pitch_mod);

  // Push Braids param changes only when they differ — set_pitch() and
  // set_parameters() are cheap but not free; skip when no change.
  if (v->timbre != v->applied_timbre || v->color != v->applied_color) {
    v->osc->set_parameters(v->timbre, v->color);
    v->applied_timbre = v->timbre;
    v->applied_color  = v->color;
  }
  if (target_pitch != v->applied_pitch) {
    v->osc->set_pitch(target_pitch);
    v->applied_pitch = target_pitch;
  }

  memset(v->sync_buffer, 0, sizeof(v->sync_buffer));
  v->osc->Render(v->sync_buffer, v->buffer, BLOCK_SIZE);

  // ---- Chamberlin state-variable filter, LP output ----
  const int32_t F     = svfCutoffToF(cutoff_q15);
  const int32_t F12   = F >> 3;                    // Q12 to keep multiplies in int32
  const int32_t inv_Q = svfResonanceToQ(resonance_q15);

  constexpr int32_t SVF_CLAMP = 32767;

  int32_t lp = v->svf_lp;
  int32_t bp = v->svf_bp;

  for (int i = 0; i < BLOCK_SIZE; i++) {
    const int32_t x = (int32_t)v->buffer[i];

    lp += (F12 * bp) >> 12;
    if (lp >  SVF_CLAMP) lp =  SVF_CLAMP;
    if (lp < -SVF_CLAMP) lp = -SVF_CLAMP;

    const int32_t hp = x - lp - ((inv_Q * bp) >> 15);
    bp += (F12 * hp) >> 12;
    if (bp >  SVF_CLAMP) bp =  SVF_CLAMP;
    if (bp < -SVF_CLAMP) bp = -SVF_CLAMP;

    int32_t out = lp;
    if (out > AUDIO_MAX) out = AUDIO_MAX;
    if (out < AUDIO_MIN) out = AUDIO_MIN;
    v->buffer[i] = (int16_t)out;
  }

  v->svf_lp = lp;
  v->svf_bp = bp;
}

// ============================================================================
// CHORD / PITCH ASSIGNMENT
// ============================================================================
//
// Layout: four chord-interval voices plus a fifth doubling the root two
// octaves up for body. Inversion cycles through voices octave-wise, same
// algorithm as Murci.
// ============================================================================

void setDronePitches(DroneVoice voices[NUM_VOICES], uint8_t root_note,
                     uint8_t chord_type, int8_t inversion) {
  if (chord_type >= NUM_CHORDS) chord_type = 0;
  if (inversion < MIN_INVERSION) inversion = MIN_INVERSION;
  if (inversion > MAX_INVERSION) inversion = MAX_INVERSION;

  int16_t ivs[NUM_VOICES];
  uint8_t n = 0;
  for (uint8_t i = 0; i < CHORD_INTERVAL_COUNT && n < NUM_VOICES; i++) {
    const int8_t interval = CHORD_INTERVALS[chord_type][i];
    if (interval >= 0) ivs[n++] = interval;
  }
  for (uint8_t octave = 1; n < NUM_VOICES; octave++) {
    for (uint8_t i = 0; i < CHORD_INTERVAL_COUNT && n < NUM_VOICES; i++) {
      const int8_t interval = CHORD_INTERVALS[chord_type][i];
      if (interval >= 0) ivs[n++] = interval + 12 * octave;
    }
  }

  // Sort ascending
  for (uint8_t i = 0; i < NUM_VOICES - 1; i++)
    for (uint8_t j = i + 1; j < NUM_VOICES; j++)
      if (ivs[j] < ivs[i]) { int16_t t = ivs[i]; ivs[i] = ivs[j]; ivs[j] = t; }

  // Apply inversion: each step moves one voice up (or down) an octave
  if (inversion >= 0) {
    for (uint8_t step = 0; step < (uint8_t)inversion; step++)
      ivs[step % NUM_VOICES] += 12;
  } else {
    const uint8_t n = (uint8_t)(-inversion);
    for (uint8_t step = 0; step < n; step++)
      ivs[step % NUM_VOICES] -= 12;
  }

  // Re-sort after inversion moves
  for (uint8_t i = 0; i < NUM_VOICES - 1; i++)
    for (uint8_t j = i + 1; j < NUM_VOICES; j++)
      if (ivs[j] < ivs[i]) { int16_t t = ivs[i]; ivs[i] = ivs[j]; ivs[j] = t; }

  for (uint8_t i = 0; i < NUM_VOICES; i++) {
    int16_t note = (int16_t)root_note + ivs[i];
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    voices[i].pitch = (int16_t)(note << 7);
  }
}

// ============================================================================
// TIMBRE
// ============================================================================

void setDroneTimbre(DroneVoice voices[NUM_VOICES], int16_t timbre) {
  if (timbre < 0)     timbre = 0;
  if (timbre > 32767) timbre = 32767;
  for (uint8_t i = 0; i < NUM_VOICES; i++) voices[i].timbre = timbre;
}

// ============================================================================
// SHAPE
// ============================================================================

void setDroneShape(DroneVoice voices[NUM_VOICES],
                   braids::MacroOscillatorShape shape,
                   int16_t timbre, int16_t color) {
  for (uint8_t i = 0; i < NUM_VOICES; i++) {
    voices[i].osc->set_shape(shape);
    voices[i].osc->set_parameters(timbre, color);
    voices[i].timbre         = timbre;
    voices[i].color          = color;
    voices[i].applied_timbre = timbre;
    voices[i].applied_color  = color;
  }
}
