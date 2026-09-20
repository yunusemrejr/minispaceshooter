/* audio.h — calm procedural music and arcade sound effects for Mini Space Shooter.
 *
 * There is NO link-time audio dependency: the backend is resolved at runtime
 * with dlopen() against libpulse-simple.so.0, then libasound.so.2, and if
 * neither can be opened the game simply runs silent.  Volume defaults low.
 */
#ifndef MINI_AUDIO_H
#define MINI_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SFX_LASER = 0,      /* player shot */
    SFX_ENEMY_SHOT,     /* enemy shot */
    SFX_HIT_ENEMY,      /* bullet connects */
    SFX_EXPLODE,        /* small kill */
    SFX_EXPLODE_BIG,    /* brute / player death */
    SFX_PLAYER_HIT,     /* we took damage */
    SFX_LEVELUP,        /* level checkpoint */
    SFX_UI,             /* menu blip */
    SFX_MEDAL,          /* medal awarded */
    SFX_GAMEOVER,       /* run over */
    SFX_TRICK,          /* the director reveals a trick (very subtle) */
    SFX_ALLY_SHOT,
    SFX_RECRUIT,
    SFX_COUNT
} SfxId;

/* Returns 0 when a backend opened, 1 when the game runs silent. */
int aud_init(void);
void aud_shutdown(void);
int aud_available(void);
void aud_backend_name(char *out, int cap);

/* pitch is a multiplier around 1.0 (0 or negative means "no variation"). */
void aud_play(SfxId id, float pitch);
/* Generates and submits audio for the elapsed frame. Cheap when silent. */
void aud_pump(double dt);

void aud_set_master(float v); /* 0..1 */
float aud_get_master(void);
void aud_adjust_master(float delta);
void aud_toggle_mute(void);
int aud_muted(void);
void aud_reset_stats(void);
int aud_voices_active(void);
void aud_set_music(int enabled);
int aud_music_enabled(void);
uint64_t aud_frames_submitted(void);
unsigned aud_output_errors(void);

/* Offline synthesis uses the same mixer without opening any audio device. */
void aud_init_offline(void);
int aud_sample_rate(void);
void aud_render_samples(int16_t *out, int frames);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINI_AUDIO_H */
