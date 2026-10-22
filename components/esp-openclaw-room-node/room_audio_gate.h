#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    atomic_bool playing;
    atomic_uint tail_us;
} room_audio_gate_t;

void room_audio_gate_init(room_audio_gate_t *gate, const char *mic_layout);
/* FIELD 2026-10-21 (Fredrik): force the half-duplex gate ON even when the
 * mic layout claims a reference channel. The layout rule assumed a real
 * speaker reference (so AEC could cancel the echo and the uplink could
 * stay open during playback). On boards whose 'R' channel is just another
 * microphone (no loopback of the speaker) that assumption is wrong: the
 * speaker's own voice reaches the uplink, the gateway hears itself, and
 * the voice session tears itself down. Forcing the gate mutes the mic
 * during playback + the tail, which breaks the loop. */
void room_audio_gate_init_ex(room_audio_gate_t *gate, const char *mic_layout,
                             bool force_half_duplex);
/* Returns true when a non-silent PCM16 block needs a matching end call. */
bool room_audio_gate_begin_playback(room_audio_gate_t *gate,
                                    const int16_t *pcm, size_t sample_count);
void room_audio_gate_end_playback(room_audio_gate_t *gate);

/* Called once per raw capture block, before AFE/VAD. The capture clock drains
 * the tail even when the block is silent; playback never stops capture. */
void room_audio_gate_filter(room_audio_gate_t *gate, void *pcm, size_t size,
                            unsigned duration_us);
