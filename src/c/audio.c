/* audio.c — calm procedural music and SFX with runtime-resolved playback.
 *
 * The synth is deliberately tiny: a fixed bank of voices, each a square /
 * triangle / sine / filtered-noise oscillator with a curved amplitude envelope.
 * Everything lives in static arrays, generation is O(frames) per frame, and
 * when no backend can be opened the whole thing costs a few compares.
 *
 * Backends (dlopen, no headers, no link flags):
 *   1. libpulse-simple.so.0 — device-paced playback on a dedicated thread.
 *   2. libasound.so.2 — nonblocking "default" PCM playback.
 *   3. silent.
 */
#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define AUD_RATE 22050
#define AUD_VOICES 12
#define AUD_RING 8192 /* mono frames (~0.37 s) */

typedef enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE, WAVE_SINE } Wave;

typedef struct {
    float freq0, freq1; /* Hz sweep */
    float dur;          /* seconds */
    float amp;
    float attack;       /* seconds */
    Wave wave;
    float phase;
    float t;
    float lp;
    int active;
} Voice;

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread;
static atomic_int audio_stop;
static int audio_thread_started;
static void *pulse_worker(void *unused);
static void generate(int frames, int16_t *out);

static struct {
    int initialized, rate, music, failed;
    double fractional_frames;
    uint32_t music_frame;
    float music_phase[5], music_gain;
    uint64_t submitted;
    unsigned errors;
    int backend; /* 0 none, 1 alsa, 2 pulse */
    int muted;
    float master;
    float master_saved;
    Voice v[AUD_VOICES];
    int next_voice;
    int16_t ring[AUD_RING];
    int ring_head, ring_tail;
    uint32_t noise_state;
    int voices_peak;
    void *lib;
    void *pcm;
    void *pa;
} A;

/* ---------------------------------------------------------------- backend */
/* ALSA: typed function pointers only, so no varargs prototype mismatches. */
typedef struct {
    int (*pcm_open)(void **, const char *, int, int);
    int (*pcm_close)(void *);
    int (*pcm_prepare)(void *);
    long (*pcm_writei)(void *, const void *, unsigned long);
    int (*pcm_nonblock)(void *, int);
    int (*pcm_recover)(void *, int, int);
    int (*hw_params_malloc)(void **);
    void (*hw_params_free)(void *);
    int (*hw_params_any)(void *, void *);
    int (*hw_params_set_access)(void *, void *, int);
    int (*hw_params_set_format)(void *, void *, int);
    int (*hw_params_set_channels)(void *, void *, unsigned);
    int (*hw_params_set_rate_near)(void *, void *, unsigned *, int *);
    int (*hw_params_set_period_size_near)(void *, void *, unsigned long *, int *);
    int (*hw_params_set_buffer_size_near)(void *, void *, unsigned long *);
    int (*hw_params)(void *, void *);
} AlsaApi;

/* PulseAudio simple API. */
typedef struct {
    int format;
    uint32_t rate;
    uint8_t channels;
} PaSampleSpec;

typedef struct {
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
} PaBufferAttr;

typedef struct {
    void *(*simple_new)(const char *, const char *, int, const char *, const char *, const PaSampleSpec *,
                        const void *, const PaBufferAttr *, int *);
    void (*simple_free)(void *);
    int (*simple_write)(void *, const void *, size_t, int *);
} PulseApi;

static AlsaApi AS;
static PulseApi PA;

#define ALSA_STREAM_PLAYBACK 0
#define ALSA_ACCESS_RW_INTERLEAVED 3
#define ALSA_FORMAT_S16_LE 2
#define PA_STREAM_PLAYBACK 1
#define PA_SAMPLE_S16LE 3
#define PA_BUF_DEFAULT ((uint32_t)-1)

static void *sym(void *lib, const char *name)
{
    return dlsym(lib, name);
}

static int try_alsa(void)
{
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1;
    int ok = 1;
#define LOAD(field, name)                              \
    do {                                               \
        *(void **)(&AS.field) = sym(lib, name);         \
        if (!AS.field) ok = 0;                          \
    } while (0)
    LOAD(pcm_open, "snd_pcm_open");
    LOAD(pcm_close, "snd_pcm_close");
    LOAD(pcm_prepare, "snd_pcm_prepare");
    LOAD(pcm_writei, "snd_pcm_writei");
    LOAD(pcm_nonblock, "snd_pcm_nonblock");
    LOAD(pcm_recover, "snd_pcm_recover");
    LOAD(hw_params_malloc, "snd_pcm_hw_params_malloc");
    LOAD(hw_params_free, "snd_pcm_hw_params_free");
    LOAD(hw_params_any, "snd_pcm_hw_params_any");
    LOAD(hw_params_set_access, "snd_pcm_hw_params_set_access");
    LOAD(hw_params_set_format, "snd_pcm_hw_params_set_format");
    LOAD(hw_params_set_channels, "snd_pcm_hw_params_set_channels");
    LOAD(hw_params_set_rate_near, "snd_pcm_hw_params_set_rate_near");
    LOAD(hw_params_set_period_size_near, "snd_pcm_hw_params_set_period_size_near");
    LOAD(hw_params_set_buffer_size_near, "snd_pcm_hw_params_set_buffer_size_near");
    LOAD(hw_params, "snd_pcm_hw_params");
#undef LOAD
    if (!ok) { dlclose(lib); return -1; }

    void *pcm = NULL;
    if (AS.pcm_open(&pcm, "default", ALSA_STREAM_PLAYBACK, 0) < 0) {
        dlclose(lib);
        return -1;
    }
    void *params = NULL;
    if (AS.hw_params_malloc(&params) < 0) { AS.pcm_close(pcm); dlclose(lib); return -1; }

    unsigned rate = AUD_RATE;
    int dir = 0;
    unsigned long period = 512, buffer = 4096;
    int err = 0;
    err |= AS.hw_params_any(pcm, params);
    err |= AS.hw_params_set_access(pcm, params, ALSA_ACCESS_RW_INTERLEAVED);
    err |= AS.hw_params_set_format(pcm, params, ALSA_FORMAT_S16_LE);
    err |= AS.hw_params_set_channels(pcm, params, 1);
    err |= AS.hw_params_set_rate_near(pcm, params, &rate, &dir);
    err |= AS.hw_params_set_period_size_near(pcm, params, &period, &dir);
    err |= AS.hw_params_set_buffer_size_near(pcm, params, &buffer);
    err |= AS.hw_params(pcm, params);
    AS.hw_params_free(params);
    if (err < 0) { AS.pcm_close(pcm); dlclose(lib); return -1; }
    if (rate < 8000 || rate > 192000 || AS.pcm_nonblock(pcm, 1) < 0 || AS.pcm_prepare(pcm) < 0) {
        AS.pcm_close(pcm); dlclose(lib); return -1;
    }
    A.rate = (int)rate;

    A.lib = lib;
    A.pcm = pcm;
    return 0;
}

static int try_pulse(void)
{
    void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1;
    *(void **)(&PA.simple_new) = sym(lib, "pa_simple_new");
    *(void **)(&PA.simple_free) = sym(lib, "pa_simple_free");
    *(void **)(&PA.simple_write) = sym(lib, "pa_simple_write");
    if (!PA.simple_new || !PA.simple_free || !PA.simple_write) { dlclose(lib); return -1; }

    PaSampleSpec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = AUD_RATE;
    ss.channels = 1;
    PaBufferAttr attr;
    attr.maxlength = PA_BUF_DEFAULT;
    attr.tlength = AUD_RATE / 12 * 2; /* ~83 ms, in bytes */
    attr.prebuf = PA_BUF_DEFAULT;
    attr.minreq = 256 * 2;
    attr.fragsize = PA_BUF_DEFAULT;
    int err = 0;
    void *s = PA.simple_new(NULL, "mini-space-shooter", PA_STREAM_PLAYBACK, NULL, "game", &ss, NULL, &attr, &err);
    if (!s) { dlclose(lib); return -1; }
    A.lib = lib;
    A.pa = s;
    return 0;
}

void aud_init_offline(void)
{
    aud_shutdown();
    memset(&A, 0, sizeof(A));
    A.initialized = 1;
    A.rate = AUD_RATE;
    A.music = 1;
    A.master = A.master_saved = 0.28f;
    A.noise_state = 0x1234567u;
}

int aud_init(void)
{
    aud_init_offline();
    const char *choice = getenv("MSS_AUDIO_BACKEND");
    if (choice && strcmp(choice, "silent") == 0) return 1;
    /* Prefer the desktop mixer; ALSA remains usable without a sound server. */
    if ((!choice || strcmp(choice, "pulse") == 0) && try_pulse() == 0) A.backend = 2;
    else if ((!choice || strcmp(choice, "alsa") == 0) && try_alsa() == 0) A.backend = 1;
    if (!A.backend) {
        fprintf(stderr, "[audio] no playback device; running silently\n");
        return 1;
    }
    if (A.backend == 2) {
        atomic_store(&audio_stop, 0);
        if (pthread_create(&audio_thread, NULL, pulse_worker, NULL) == 0) {
            audio_thread_started = 1;
        } else {
            /* Never fall back to blocking writes on the render thread. */
            PA.simple_free(A.pa); A.pa = NULL;
            dlclose(A.lib); A.lib = NULL; A.backend = 0;
            if ((!choice || strcmp(choice, "alsa") == 0) && try_alsa() == 0) A.backend = 1;
            if (!A.backend) return 1;
        }
    }
    /* ALSA's nonblocking path gets a short safety cushion. */
    if (A.backend == 1) aud_pump(0.075);
    return 0;
}

void aud_shutdown(void)
{
    if (audio_thread_started) {
        atomic_store(&audio_stop, 1);
        pthread_join(audio_thread, NULL);
        audio_thread_started = 0;
    }
    if (A.backend == 1 && A.pcm) { AS.pcm_close(A.pcm); }
    if (A.backend == 2 && A.pa) { PA.simple_free(A.pa); }
    if (A.lib) dlclose(A.lib);
    A.pcm = NULL;
    A.pa = NULL;
    A.lib = NULL;
    A.backend = 0;
    A.initialized = 0;
}

int aud_available(void)
{
    pthread_mutex_lock(&audio_lock);
    int available = A.backend != 0 && !A.failed;
    pthread_mutex_unlock(&audio_lock);
    return available;
}

void aud_backend_name(char *out, int cap)
{
    if (!out || cap <= 0) return;
    pthread_mutex_lock(&audio_lock);
    const char *n = A.failed ? "failed" : A.backend == 1 ? "alsa" : A.backend == 2 ? "pulse" : "silent";
    snprintf(out, (size_t)cap, "%s", n);
    pthread_mutex_unlock(&audio_lock);
}

int aud_muted(void)
{
    pthread_mutex_lock(&audio_lock); int v = A.muted; pthread_mutex_unlock(&audio_lock); return v;
}
float aud_get_master(void)
{
    pthread_mutex_lock(&audio_lock);
    float v = A.muted ? 0.0f : A.master;
    pthread_mutex_unlock(&audio_lock);
    return v;
}
void aud_set_master(float v)
{
    if (!isfinite(v)) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    pthread_mutex_lock(&audio_lock);
    A.master = v;
    if (!A.muted) A.master_saved = v;
    pthread_mutex_unlock(&audio_lock);
}
void aud_adjust_master(float delta)
{
    if (aud_muted()) aud_toggle_mute();
    aud_set_master(aud_get_master() + delta);
}
void aud_toggle_mute(void)
{
    pthread_mutex_lock(&audio_lock);
    A.muted = !A.muted;
    if (!A.muted) A.master = A.master_saved;
    pthread_mutex_unlock(&audio_lock);
}
void aud_reset_stats(void)
{
    pthread_mutex_lock(&audio_lock); A.voices_peak = 0; pthread_mutex_unlock(&audio_lock);
}
int aud_voices_active(void)
{
    pthread_mutex_lock(&audio_lock); int v = A.voices_peak; pthread_mutex_unlock(&audio_lock); return v;
}
void aud_set_music(int enabled)
{
    pthread_mutex_lock(&audio_lock); A.music = enabled != 0; pthread_mutex_unlock(&audio_lock);
}
int aud_music_enabled(void)
{
    pthread_mutex_lock(&audio_lock); int v = A.music; pthread_mutex_unlock(&audio_lock); return v;
}
int aud_sample_rate(void) { return A.rate ? A.rate : AUD_RATE; }
uint64_t aud_frames_submitted(void)
{
    pthread_mutex_lock(&audio_lock); uint64_t v = A.submitted; pthread_mutex_unlock(&audio_lock); return v;
}
unsigned aud_output_errors(void)
{
    pthread_mutex_lock(&audio_lock); unsigned v = A.errors; pthread_mutex_unlock(&audio_lock); return v;
}

/* ------------------------------------------------------------------ voices */
static float rnd_unit(void)
{
    A.noise_state = A.noise_state * 1664525u + 1013904223u;
    return (float)((A.noise_state >> 8) & 0xFFFFu) / 32768.0f - 1.0f;
}

static Voice *voice_alloc(void);

static Voice *voice_start(Voice *v, Wave wave, float f0, float f1, float dur, float amp, float attack)
{
    v->wave = wave;
    v->freq0 = f0;
    v->freq1 = f1;
    v->dur = dur;
    v->amp = amp;
    v->attack = attack;
    v->phase = 0.0f;
    v->t = 0.0f;
    v->lp = 0.0f;
    v->active = 1;
    return v;
}

/* Starts a note `delay` seconds from now by using a negative start time. */
static void voice_start_arp(Wave wave, float f, float dur, float amp, float delay)
{
    voice_start(voice_alloc(), wave, f, f, dur, amp, 0.005f)->t = -delay;
}

static Voice *voice_alloc(void)
{
    /* round robin: oldest/loudest gets recycled, no allocation anywhere */
    for (int i = 0; i < AUD_VOICES; ++i) {
        Voice *v = &A.v[A.next_voice];
        A.next_voice = (A.next_voice + 1) % AUD_VOICES;
        if (!v->active) return v;
    }
    Voice *v = &A.v[A.next_voice];
    A.next_voice = (A.next_voice + 1) % AUD_VOICES;
    return v;
}

static void play_unlocked(SfxId id, float pitch)
{
    if (!A.initialized || A.muted || id < 0 || id >= SFX_COUNT) return;
    if (!isfinite(pitch) || pitch <= 0.0f) pitch = 1.0f;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 3.0f) pitch = 3.0f;
    Voice *v = voice_alloc();
    switch (id) {
    case SFX_LASER:
        voice_start(v, WAVE_TRI, 1250.0f * pitch, 340.0f * pitch, 0.105f, 0.60f, 0.002f);
        break;
    case SFX_ALLY_SHOT:
        voice_start(v, WAVE_SINE, 740.0f * pitch, 270.0f * pitch, 0.10f, 0.32f, 0.003f);
        break;
    case SFX_RECRUIT:
        voice_start(v, WAVE_SINE, 523.25f * pitch, 523.25f * pitch, 0.32f, 0.36f, 0.01f);
        voice_start_arp(WAVE_SINE, 659.25f * pitch, 0.32f, 0.32f, 0.10f);
        voice_start_arp(WAVE_SINE, 783.99f * pitch, 0.42f, 0.30f, 0.20f);
        break;
    case SFX_ENEMY_SHOT:
        voice_start(v, WAVE_SQUARE, 430.0f * pitch, 250.0f * pitch, 0.06f, 0.22f, 0.004f);
        break;
    case SFX_HIT_ENEMY:
        voice_start(v, WAVE_NOISE, 1.0f, 1.0f, 0.05f, 0.26f, 0.001f);
        break;
    case SFX_EXPLODE:
        voice_start(v, WAVE_NOISE, 1.0f, 1.0f, 0.26f, 0.5f, 0.004f);
        break;
    case SFX_EXPLODE_BIG:
        voice_start(v, WAVE_NOISE, 1.0f, 1.0f, 0.5f, 0.6f, 0.006f);
        voice_start(voice_alloc(), WAVE_TRI, 180.0f, 60.0f, 0.45f, 0.35f, 0.01f);
        break;
    case SFX_PLAYER_HIT:
        voice_start(v, WAVE_SQUARE, 300.0f, 90.0f, 0.34f, 0.5f, 0.006f);
        break;
    case SFX_LEVELUP:
        voice_start(v, WAVE_SQUARE, 523.0f * pitch, 523.0f * pitch, 0.1f, 0.3f, 0.005f);
        voice_start_arp(WAVE_SQUARE, 659.0f * pitch, 0.1f, 0.3f, 0.1f);
        voice_start_arp(WAVE_SQUARE, 784.0f * pitch, 0.16f, 0.3f, 0.2f);
        break;
    case SFX_UI:
        voice_start(v, WAVE_SQUARE, 640.0f, 640.0f, 0.035f, 0.2f, 0.002f);
        break;
    case SFX_MEDAL:
        voice_start(v, WAVE_SQUARE, 659.0f, 659.0f, 0.1f, 0.26f, 0.004f);
        voice_start_arp(WAVE_SQUARE, 880.0f, 0.1f, 0.26f, 0.09f);
        voice_start_arp(WAVE_TRI, 1319.0f, 0.26f, 0.3f, 0.18f);
        break;
    case SFX_GAMEOVER:
        voice_start(v, WAVE_TRI, 440.0f, 440.0f, 0.18f, 0.34f, 0.01f);
        voice_start_arp(WAVE_TRI, 330.0f, 0.18f, 0.34f, 0.17f);
        voice_start_arp(WAVE_TRI, 196.0f, 0.5f, 0.36f, 0.34f);
        break;
    case SFX_TRICK:
        voice_start(v, WAVE_SQUARE, 1250.0f, 900.0f, 0.05f, 0.14f, 0.002f);
        break;
    default:
        break;
    }
}

void aud_play(SfxId id, float pitch)
{
    pthread_mutex_lock(&audio_lock);
    play_unlocked(id, pitch);
    pthread_mutex_unlock(&audio_lock);
}

/* -------------------------------------------------------------- generation */
static float voice_sample(Voice *v, float dt)
{
    if (!v->active) return 0.0f;
    if (v->t < 0.0f) { v->t += dt; return 0.0f; } /* delayed note start */
    if (v->t >= v->dur) { v->active = 0; return 0.0f; }
    float k = v->t / v->dur;
    float f = v->freq0 + (v->freq1 - v->freq0) * k;
    float s = 0.0f;
    switch (v->wave) {
    case WAVE_SQUARE:
        v->phase += f * dt;
        if (v->phase >= 1.0f) v->phase -= (float)(long)v->phase;
        s = v->phase < 0.5f ? 1.0f : -1.0f;
        break;
    case WAVE_TRI:
        v->phase += f * dt;
        if (v->phase >= 1.0f) v->phase -= (float)(long)v->phase;
        s = 4.0f * fabsf(v->phase - 0.5f) - 1.0f;
        break;
    case WAVE_SINE:
        v->phase += f * dt;
        v->phase -= floorf(v->phase);
        s = sinf(v->phase * 6.2831853f);
        break;
    case WAVE_NOISE:
        s = rnd_unit();
        v->lp += (s - v->lp) * (0.25f - 0.22f * k); /* darkens as it decays */
        s = v->lp * 2.2f;
        break;
    }
    float env = 1.0f - k;
    if (v->attack > 0.0f && v->t < v->attack) env *= v->t / v->attack;
    v->t += dt;
    return s * env * v->amp * env; /* slightly curved decay */
}

/* Original, slow Cmaj7 / Am7 / Fmaj7 / Gsus ambience. Music has its own
 * oscillators, so gunfire cannot steal notes. All clocks wrap each phrase. */
static float music_sample(float dt)
{
    static const float chord[4][4] = {
        {130.8128f, 164.8138f, 195.9977f, 246.9417f},
        {110.0000f, 130.8128f, 164.8138f, 195.9977f},
        { 87.3071f, 130.8128f, 164.8138f, 220.0000f},
        { 97.9989f, 146.8324f, 195.9977f, 261.6256f},
    };
    static const int arpeggio[8] = {0, 2, 1, 3, 2, 1, 3, 2};
    uint32_t beat_frames = (uint32_t)A.rate * 3u / 5u; /* 100 BPM */
    uint32_t chord_frames = beat_frames * 8u;
    int c = (int)(A.music_frame / chord_frames);
    uint32_t local = A.music_frame % chord_frames;
    float t = (float)local * dt;
    float left = (float)(chord_frames - local) * dt;
    float envelope = fminf(1.0f, fminf(t, left) / 0.45f);
    float sample = 0.0f;
    for (int n = 0; n < 4; ++n) {
        A.music_phase[n] += chord[c][n] * dt;
        A.music_phase[n] -= floorf(A.music_phase[n]);
        sample += sinf(A.music_phase[n] * 6.2831853f) * 0.065f * envelope;
    }
    float beat_t = (float)(local % beat_frames) * dt;
    float freq = chord[c][arpeggio[local / beat_frames]] * 2.0f;
    A.music_phase[4] += freq * dt;
    A.music_phase[4] -= floorf(A.music_phase[4]);
    float bell_env = fminf(1.0f, beat_t / 0.02f) * expf(-beat_t * 6.0f) * fminf(1.0f, (0.6f - beat_t) / 0.05f);
    sample += sinf(A.music_phase[4] * 6.2831853f) * bell_env * 0.09f;
    A.music_frame = (A.music_frame + 1u) % (chord_frames * 4u);
    A.music_gain += ((A.music ? 1.0f : 0.0f) - A.music_gain) * fminf(1.0f, dt * 30.0f);
    return sample * A.music_gain;
}

static void generate(int frames, int16_t *out)
{
    if (!out || frames <= 0 || !A.initialized) return;
    float dt = 1.0f / (float)A.rate;
    float g = A.muted ? 0.0f : A.master;
    int active = 0;
    for (int i = 0; i < AUD_VOICES; ++i) if (A.v[i].active) active++;
    if (active > A.voices_peak) A.voices_peak = active;
    for (int n = 0; n < frames; ++n) {
        float mix = music_sample(dt);
        for (int i = 0; i < AUD_VOICES; ++i) mix += voice_sample(&A.v[i], dt);
        float x = mix * g;
        x /= 1.0f + fabsf(x); /* smooth saturation keeps stacked volleys clean */
        out[n] = (int16_t)(x * 32000.0f);
    }
}

void aud_render_samples(int16_t *out, int frames)
{
    pthread_mutex_lock(&audio_lock);
    generate(frames, out);
    pthread_mutex_unlock(&audio_lock);
}

static void *pulse_worker(void *unused)
{
    (void)unused;
    int16_t chunk[512];
    /* The device clocks this thread. No GUI timing, ring drift, or gameplay
     * stalls can interrupt the synth; the mutex is never held during I/O. */
    while (!atomic_load(&audio_stop)) {
        pthread_mutex_lock(&audio_lock);
        generate(512, chunk);
        pthread_mutex_unlock(&audio_lock);
        int err = 0;
        int failed = PA.simple_write(A.pa, chunk, sizeof(chunk), &err) < 0;
        pthread_mutex_lock(&audio_lock);
        if (failed) { if (A.errors < UINT32_MAX) ++A.errors; A.failed = 1; }
        else A.submitted += 512;
        pthread_mutex_unlock(&audio_lock);
        if (failed) {
            fprintf(stderr, "[audio] Pulse playback failed (%d); audio disabled\n", err);
            break;
        }
    }
    return NULL;
}

static int ring_space(void) { return AUD_RING - ((A.ring_head - A.ring_tail + AUD_RING) % AUD_RING) - 1; }

static void ring_push(const int16_t *src, int n)
{
    for (int i = 0; i < n; ++i) {
        A.ring[A.ring_head] = src[i];
        A.ring_head = (A.ring_head + 1) % AUD_RING;
    }
}

static int ring_pop(int16_t *dst, int maxn)
{
    int n = 0;
    while (n < maxn && A.ring_tail != A.ring_head) {
        dst[n++] = A.ring[A.ring_tail];
        A.ring_tail = (A.ring_tail + 1) % AUD_RING;
    }
    return n;
}

static void flush_to_device(void)
{
    int16_t chunk[512];
    if (A.backend == 1) {
        for (int guard = 0; guard < 4; ++guard) {
            int n = ring_pop(chunk, (int)(sizeof(chunk) / sizeof(chunk[0])));
            if (n == 0) return;
            long wrote = AS.pcm_writei(A.pcm, chunk, (unsigned long)n);
            if (wrote < 0) {
                /* Nothing accepted (usually -EAGAIN): restore and retry next frame. */
                A.ring_tail = (A.ring_tail - n + AUD_RING) % AUD_RING;
                if (wrote != -EAGAIN) {
                    if (A.errors < UINT32_MAX) ++A.errors;
                    if (AS.pcm_recover(A.pcm, (int)wrote, 1) < 0) {
                        fprintf(stderr, "[audio] ALSA playback failed (%ld); audio disabled\n", wrote);
                        aud_shutdown();
                    }
                }
                return;
            }
            A.submitted += (uint64_t)wrote;
            if (wrote < n) {
                A.ring_tail = (A.ring_tail - (n - (int)wrote) + AUD_RING) % AUD_RING;
                return;
            }
        }
        return;
    }

}

void aud_pump(double dt)
{
    if (audio_thread_started || !A.backend || !isfinite(dt) || dt <= 0.0) return;
    if (dt > 0.1) dt = 0.1;
    double exact = dt * (double)A.rate + A.fractional_frames;
    int frames = (int)exact;
    A.fractional_frames = exact - (double)frames;
    if (frames > 2048) frames = 2048; /* never let a hitch generate a burst */
    if (frames > 0) {
        int16_t tmp[2048];
        aud_render_samples(tmp, frames);
        int space = ring_space();
        if (frames <= space) ring_push(tmp, frames);
        /* else: drop the chunk — keeps latency bounded instead of queuing up */
    }
    flush_to_device();
}
