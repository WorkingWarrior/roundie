#ifndef REP_AUDIO_V1_H
#define REP_AUDIO_V1_H
#include "rep_app_v3.h"

/* Speaker output for ABI 3: one PCM stream at a time, played by the system
 * audio task. A second import beside rep_abi_v3, like rep_storage_v1; firmware
 * without it rejects the ELF before loading. rep_audio_v1 returns NULL when the
 * board has no working audio; the app then runs silent.
 *
 * The app feeds the stream from its callbacks, usually a periodic timer:
 * writable() says how many bytes fit, write() copies them into the system ring
 * and never blocks. Playback starts once enough is buffered (at most 100 ms); a
 * drained ring plays silence and, after 500 ms, turns the amplifier off until
 * more data arrives. PCM pointers must be app data sections or SDK allocations,
 * not the stack. The system closes the stream when the app stops or unloads.
 * Volume is capped by the system. See docs/audio.md. */
#define REP_AUDIO_V1 UINT32_C(1)

typedef struct {
    uint32_t version; /* 1 */
    uint32_t struct_size;
    uint32_t buffer_bytes; /* ring capacity of a stream */
    /* 16-bit little-endian signed samples, interleaved when channels is 2.
     * sample_rate 8000, 16000, 24000, 32000 or 48000; channels 1 or 2;
     * volume_percent 0..100 of the system level. Returns a stream handle, or 0
     * when refused: bad arguments, audio busy (another stream, a system sound)
     * or a stream of this app already open. */
    uint32_t (*open)(uint32_t instance, uint32_t sample_rate, uint32_t channels, uint32_t volume_percent);
    /* Bytes that write() would accept now, or -1 for a stale handle. */
    int32_t (*writable)(uint32_t instance, uint32_t stream);
    /* Copies whole frames; returns the bytes accepted (0 when the ring is
     * full), or -1 for a stale handle or a bad pointer. */
    int32_t (*write)(uint32_t instance, uint32_t stream, const void *pcm, uint32_t bytes);
    /* drain != 0 plays what is buffered first; 0 stops at once. The handle is
     * invalid afterwards. 0 on success, -1 for a stale handle. */
    int32_t (*close)(uint32_t instance, uint32_t stream, uint32_t drain);
} rep_audio_v1_t;

REP2_EXPORT const rep_audio_v1_t *rep_audio_v1(uint32_t required_version);
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(rep_audio_v1_t) == 28, "REP audio API");
_Static_assert(offsetof(rep_audio_v1_t, close) == 24, "REP audio close");
#elif defined(__x86_64__) && UINTPTR_MAX == UINT64_MAX /* linux-x64.rep1, System V LP64 */
_Static_assert(sizeof(rep_audio_v1_t) == 48, "REP audio API");
_Static_assert(offsetof(rep_audio_v1_t, close) == 40, "REP audio close");
#endif
#endif
