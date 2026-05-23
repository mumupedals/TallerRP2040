#include "workshop_audio.h"

#include <cstring>

// Shared LCG for per-trigger velocity jitter. Advances on every rising-edge
// trigger across all voices, so each hit gets a fresh random attenuation.
static uint32_t s_trigger_rng = 0xDEADBEEFu;

void initDrumVoice(DrumVoice* voice, uint8_t shape, int16_t pitch) {
  if (!voice) return;

  voice->osc = new braids::MacroOscillator;
  if (!voice->osc) return;

  voice->osc->Init(SAMPLE_RATE);
  voice->osc->set_pitch(pitch);
  voice->osc->set_shape((braids::MacroOscillatorShape)shape);

  voice->shape = shape;
  voice->pitch = pitch;
  voice->timbre = Q15_MIDPOINT;
  voice->color = Q15_MIDPOINT;
  voice->last_trig = false;
  voice->trigger_pending = false;

  voice->prev_amp = 0;
  voice->amp = 0;
  voice->amp_decay = DECAY_ENVELOPE_DEFAULT;
  voice->prev_trans_amp = 0;
  voice->trans_amp = 0;
  voice->trans_decay = DECAY_FAST;
  voice->trans_gain_q15 = 0;

  voice->prev_pitch_env_amp = 0;
  voice->pitch_env_amp = 0;
  voice->pitch_env_decay = DECAY_FAST;
  voice->pitch_env_range_q7 = 0;

  voice->filt_type = FILT_OFF;
  voice->filt_k_q15 = Q15_UNITY;
  voice->filt_state = 0;

  voice->gain_q15 = Q15_UNITY;
  voice->velocity_q15 = Q15_UNITY;

  voice->decay_override_pending = false;
  voice->next_amp_decay = voice->amp_decay;

  memset(voice->buffer, 0, sizeof(voice->buffer));
  memset(voice->sync_buffer, 0, sizeof(voice->sync_buffer));
}

void triggerDrum(DrumVoice* voice) {
  if (!voice) return;
  voice->trigger_pending = true;
}

void updateDrum(DrumVoice* voice) {
  if (!voice || !voice->osc) return;

  const bool trigger = voice->trigger_pending;
  voice->trigger_pending = false;

  voice->prev_amp = voice->amp;
  voice->prev_trans_amp = voice->trans_amp;
  voice->prev_pitch_env_amp = voice->pitch_env_amp;

  const bool trigger_flag = trigger && !voice->last_trig;
  voice->last_trig = trigger;

  if (trigger_flag) {
    voice->osc->Strike();
    voice->prev_amp = Q32_UNITY;
    voice->amp = Q32_UNITY;
    voice->prev_trans_amp = Q32_UNITY;
    voice->trans_amp = Q32_UNITY;
    voice->prev_pitch_env_amp = Q32_UNITY;
    voice->pitch_env_amp = Q32_UNITY;
    voice->filt_state = 0;  // avoid pops from stale filter memory on re-trigger

    if (voice->decay_override_pending) {
      voice->amp_decay = voice->next_amp_decay;
      voice->decay_override_pending = false;
    }

    // Velocity jitter: 0..~6.25% downward random attenuation per hit. Keeps
    // repeated kicks/snares/hats from sounding identical — subtle enough that
    // the groove stays steady, loud enough to feel alive.
    s_trigger_rng = s_trigger_rng * 1664525u + 1013904223u;
    voice->velocity_q15 =
        (int16_t)((int32_t)Q15_UNITY - (int32_t)((s_trigger_rng >> 16) & 0x07FF));
  }

  // Block-rate envelope updates.
  voice->amp = (uint32_t)(((uint64_t)voice->amp * (uint64_t)voice->amp_decay) >> 32);
  voice->trans_amp = (uint32_t)(((uint64_t)voice->trans_amp * (uint64_t)voice->trans_decay) >> 32);
  voice->pitch_env_amp = (uint32_t)(((uint64_t)voice->pitch_env_amp * (uint64_t)voice->pitch_env_decay) >> 32);

  voice->osc->set_parameters(voice->timbre, voice->color);
  voice->osc->set_shape((braids::MacroOscillatorShape)voice->shape);
  const uint8_t render_chunk = (voice->pitch_env_range_q7 != 0) ? 8 : BLOCK_SIZE;
  uint8_t rendered = 0;
  while (rendered < BLOCK_SIZE) {
    const uint8_t remaining = (uint8_t)(BLOCK_SIZE - rendered);
    const uint8_t chunk = remaining < render_chunk ? remaining : render_chunk;
    const uint8_t chunk_center = (uint8_t)(rendered + (chunk >> 1));
    const uint32_t t_q16 = (BLOCK_SIZE > 1)
        ? ((uint32_t)chunk_center * Q16_UNITY / (uint32_t)(BLOCK_SIZE - 1))
        : Q16_UNITY;
    const uint32_t pitch_env_amp = lerpStateU32(voice->prev_pitch_env_amp, voice->pitch_env_amp, t_q16);

    // Pitch modulation: base + range * env (Q32). range_q7 is semitones*128
    // to match braids' internal pitch units (note << 7).
    const int32_t pitch_mod = (int32_t)(((int64_t)voice->pitch_env_range_q7 *
                                         (int64_t)pitch_env_amp) >> 32);
    voice->osc->set_pitch((int16_t)(voice->pitch + pitch_mod));
    voice->osc->Render(voice->sync_buffer + rendered, voice->buffer + rendered, chunk);
    rendered = (uint8_t)(rendered + chunk);
  }

  // Optional per-voice 1-pole filter.
  if (voice->filt_type != FILT_OFF) {
    int32_t state = voice->filt_state;
    const int32_t k = (int32_t)voice->filt_k_q15;
    const uint8_t type = voice->filt_type;
    for (uint8_t i = 0; i < BLOCK_SIZE; ++i) {
      const int32_t x = (int32_t)voice->buffer[i];
      state += ((x - state) * k) >> 15;
      int32_t y = (type == FILT_LP) ? state : (x - state);
      if (y > AUDIO_MAX) y = AUDIO_MAX;
      if (y < AUDIO_MIN) y = AUDIO_MIN;
      voice->buffer[i] = (int16_t)y;
    }
    voice->filt_state = state;
  }
}
