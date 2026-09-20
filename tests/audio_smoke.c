/* Optional device test: exercise the real playback clock and concurrent controls.
 * Does not inspect microphones or change the desktop's volume/mute settings. */
#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    if (aud_init() != 0) { fprintf(stderr, "FAIL: no playback backend\n"); return 1; }
    char backend[16]; aud_backend_name(backend, sizeof(backend));
    aud_set_master(0.16f);
    struct timespec pause = {0, 16666667};
    for (int i = 0; i < 180; ++i) {
        if (i % 15 == 0) aud_play(SFX_LASER, 1.0f);
        if (i % 40 == 0) aud_play(SFX_ALLY_SHOT, 0.9f);
        if (i == 40 || i == 60) aud_toggle_mute();
        if (i == 80) aud_set_music(0);
        if (i == 100) aud_set_music(1);
        aud_pump(1.0 / 60.0);
        (void)aud_voices_active();
        (void)aud_frames_submitted();
        (void)aud_music_enabled();
        nanosleep(&pause, NULL);
    }
    /* These must never turn into invalid frame counts or corrupt synth state. */
    aud_pump(NAN); aud_pump(INFINITY); aud_pump(-1);
    uint64_t frames = aud_frames_submitted();
    unsigned errors = aud_output_errors();
    int rate = aud_sample_rate(), available = aud_available();
    aud_shutdown();
    printf("audio smoke: %s, %llu frames at %d Hz, %u errors\n", backend, (unsigned long long)frames, rate, errors);
    if (!available || frames < (uint64_t)rate * 2 || errors != 0) return 1;
    puts("ok - playback, shooting, concurrent mute/music controls and clean shutdown");
    return 0;
}
