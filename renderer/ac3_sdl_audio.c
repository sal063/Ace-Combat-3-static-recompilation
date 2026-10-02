#include "spu_backend.h"
#include "spu_core.h"
#include <SDL3/SDL.h>
#include <stdio.h>

#define TARGET_FRAMES   (SPU_FIELD_NOMINAL * 4)
#define CEILING_FRAMES  (SPU_FIELD_NOMINAL * 12)
#define TRIM_LIMIT      (SPU_FIELD_NOMINAL / 16)

static SDL_AudioStream *stream;
static int audio_ok;
static int owns_subsystem;
static int primed;
static u32 underruns, overruns;
static float volume = 1.0f;

void ac3_audio_set_volume(float v) {
    volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    if (stream) SDL_SetAudioStreamGain(stream, volume);
}

static void sdl_audio_init(void) {
    if (stream) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "ac3: SDL_InitSubSystem(AUDIO) failed: %s\n",
                    SDL_GetError());
            return;
        }
        owns_subsystem = 1;
    }
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = SPU_SAMPLE_RATE;

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                       &spec, NULL, NULL);
    if (!stream) {
        fprintf(stderr, "ac3: cannot open audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(stream, volume);
    SDL_ResumeAudioStreamDevice(stream);
    audio_ok = 1;
    primed = 0;
    printf("audio: SDL3 %d Hz stereo, %.3f frames per NTSC field, "
           "target queue %d (%.0f ms)\n",
           SPU_SAMPLE_RATE, (double)SPU_FIELD_NUM / (double)SPU_FIELD_DEN,
           TARGET_FRAMES, 1000.0 * TARGET_FRAMES / SPU_SAMPLE_RATE);
}

static void sdl_audio_shutdown(void) {
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    if (owns_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        owns_subsystem = 0;
    }
    audio_ok = 0;
}

static int sdl_frames_wanted(void) {
    if (!audio_ok) return 0;
    int queued_bytes = SDL_GetAudioStreamQueued(stream);
    if (queued_bytes < 0) queued_bytes = 0;
    int queued = queued_bytes / (int)(2 * sizeof(s16));

    if (!primed) {
        primed = 1;
        return TARGET_FRAMES;
    }

    if (queued == 0) underruns++;
    if (queued >= CEILING_FRAMES) { overruns++; return 0; }

    int trim = (TARGET_FRAMES - queued) / 16;
    if (trim > TRIM_LIMIT) trim = TRIM_LIMIT;
    if (trim < -TRIM_LIMIT) trim = -TRIM_LIMIT;
    int want = ac3_spu_field_frames() + trim;
    return want > 0 ? want : 0;
}

static void sdl_submit(const s16 *frames, int nframes) {
    if (!audio_ok || nframes <= 0) return;
    SDL_PutAudioStreamData(stream, frames, nframes * (int)(2 * sizeof(s16)));
}

static void sdl_key_on(int v, const spu_voice_t *voice) { (void)v; (void)voice; }
static void sdl_key_off(int v) { (void)v; }
static void sdl_upload(u32 a, const u8 *d, u32 s) { (void)a; (void)d; (void)s; }
static void sdl_master(s16 l, s16 r) { (void)l; (void)r; }
static void sdl_cdvol(s16 l, s16 r) { (void)l; (void)r; }

const spu_backend_t spu_backend_sdl = {
    "sdl3",
    sdl_audio_init, sdl_audio_shutdown,
    sdl_key_on, sdl_key_off, sdl_upload, sdl_master, sdl_cdvol,
    sdl_frames_wanted, sdl_submit,
};

void ac3_audio_queue_stats(u32 *under, u32 *over) {
    if (under) *under = underruns;
    if (over) *over = overruns;
}
