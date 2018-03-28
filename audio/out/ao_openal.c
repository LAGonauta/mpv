/*
 * OpenAL audio output driver for MPlayer
 *
 * Copyleft 2006 by Reimar Döffinger (Reimar.Doeffinger@stud.uni-karlsruhe.de)
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#define AL_ALEXT_PROTOTYPES
#ifdef __APPLE__
#ifndef AL_FORMAT_MONO_FLOAT32
#define AL_FORMAT_MONO_FLOAT32 0x10010
#endif
#ifndef AL_FORMAT_STEREO_FLOAT32
#define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif
#ifndef AL_FORMAT_MONO_DOUBLE_EXT
#define AL_FORMAT_MONO_DOUBLE_EXT 0x10012
#endif
#include <OpenAL/MacOSX_OALExtensions.h>
#else
#ifdef OPENAL_AL_H
#include <OpenAL/alc.h>
#include <OpenAL/al.h>
#include <OpenAL/alext.h>
#else
#include <AL/alc.h>
#include <AL/al.h>
#include <AL/alext.h>
#endif
#endif // __APPLE__

#include "common/msg.h"

#include "ao.h"
#include "internal.h"
#include "audio/format.h"
#include "osdep/timer.h"
#include "options/m_option.h"

#define MAX_CHANS MP_NUM_CHANNELS
#define NUM_BUF 128
#define CHUNK_SAMPLES 256
static ALuint buffers[NUM_BUF];
static ALuint source;

static int cur_buf;
static int unqueue_buf;

static struct ao *ao_data;

struct priv {
    ALenum al_format;
    int chunk_size;
    int direct_channels;
};

static void reset(struct ao *ao);

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    ALboolean mute;
    switch (cmd) {
    case AOCONTROL_GET_VOLUME:
    case AOCONTROL_SET_VOLUME: {
        ALfloat volume;
        ao_control_vol_t *vol = (ao_control_vol_t *)arg;
        if (cmd == AOCONTROL_SET_VOLUME) {
            volume = (vol->left + vol->right) / 200.0;
            alListenerf(AL_GAIN, volume);
        }
        alGetListenerf(AL_GAIN, &volume);
        vol->left = vol->right = volume * 100;
        return CONTROL_TRUE;
    }
    case AOCONTROL_GET_MUTE:
    case AOCONTROL_SET_MUTE:
        mute = *(ALboolean *)arg;
        ALfloat al_mute = (ALfloat)(!mute);
        if (cmd == AOCONTROL_SET_MUTE) {
            alSourcef(source, AL_GAIN, al_mute);
        }
        alGetSourcef(source, AL_GAIN, &al_mute);
        *(ALboolean *)arg = !((ALboolean)al_mute);
        return CONTROL_TRUE;

    case AOCONTROL_HAS_SOFT_VOLUME:
        return CONTROL_TRUE;
    }
    return CONTROL_UNKNOWN;
}

struct speaker {
    int id;
    float pos[3];
};

static const struct speaker speaker_pos[] = {
    {MP_SPEAKER_ID_FL,   {-0.500,  0, -0.866}}, // -30 deg
    {MP_SPEAKER_ID_FR,   { 0.500,  0, -0.866}}, //  30 deg
    {MP_SPEAKER_ID_FC,   {     0,  0,     -1}}, //   0 deg
    {MP_SPEAKER_ID_LFE,  {     0, -1,      0}}, //   below
    {MP_SPEAKER_ID_BL,   {-0.609,  0,  0.793}}, // -142.5 deg
    {MP_SPEAKER_ID_BR,   { 0.609,  0,  0.793}}, //  142.5 deg
    {MP_SPEAKER_ID_BC,   {     0,  0,      1}}, //  180 deg
    {MP_SPEAKER_ID_SL,   {-0.985,  0,  0.174}}, // -100 deg
    {MP_SPEAKER_ID_SR,   { 0.985,  0,  0.174}}, //  100 deg
    {-1},
};

static enum af_format get_supported_format(int format)
{
    switch (format) {
    case AF_FORMAT_U8:
        if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO8"))
            return AF_FORMAT_U8;
        break;

    case AF_FORMAT_S16:
        if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO16"))
            return AF_FORMAT_S16;
        break;

    case AF_FORMAT_S32:
        if (strstr(alGetString(AL_RENDERER), "X-Fi") != NULL)
            return AF_FORMAT_S32;
        break;

    case AF_FORMAT_FLOAT:
        if (alIsExtensionPresent((ALchar*)"AL_EXT_float32") == AL_TRUE)
            return AF_FORMAT_FLOAT;
        break;
    }
    return AL_FALSE;
}

static ALenum get_supported_layout(int format, int channels)
{
    switch (format) {
    case AF_FORMAT_U8:
        switch (channels) {
        case 8:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_71CHN8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_71CHN8");
            }
            break;
        case 7:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_61CHN8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_61CHN8");
            }
            break;
        case 6:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_51CHN8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_51CHN8");
            }
            break;
        case 4:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_QUAD8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_QUAD8");
            }
            break;
        case 2:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_STEREO8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_STEREO8");
            }
            break;
        case 1:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO8")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_MONO8");
            }
            break;
        }

    case AF_FORMAT_S16:
        switch (channels) {
        case 8:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_71CHN16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_71CHN16");
            }
            break;
        case 7:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_61CHN16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_61CHN16");
            }
            break;
        case 6:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_51CHN16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_51CHN16");
            }
            break;
        case 4:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_QUAD16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_QUAD16");
            }
            break;
        case 2:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_STEREO16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_STEREO16");
            }
            break;
        case 1:
            if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO16")) {
                return alGetEnumValue((ALchar*)"AL_FORMAT_MONO16");
            }
            break;
        }

    case AF_FORMAT_S32:
        if (strstr(alGetString(AL_RENDERER), "X-Fi") != NULL) {
            switch (channels) {
            case 8:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_71CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_71CHN32");
                }
                break;
            case 7:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_61CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_61CHN32");
                }
                break;
            case 6:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_51CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_51CHN32");
                }
                break;
            case 4:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_QUAD32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_QUAD32");
                }
                break;
            case 2:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_STEREO32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_STEREO32");
                }
                break;
            case 1:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_MONO32");
                }
                break;
            }
        }

    case AF_FORMAT_FLOAT:
        if (alIsExtensionPresent((ALchar*)"AL_EXT_float32") == AL_TRUE) {
            switch (channels) {
            case 8:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_71CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_71CHN32");
                }
                break;
            case 7:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_61CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_61CHN32");
                }
                break;
            case 6:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_51CHN32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_51CHN32");
                }
                break;
            case 4:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_QUAD32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_QUAD32");
                }
            case 2:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_STEREO_FLOAT32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_STEREO_FLOAT32");
                }
            case 1:
                if (alGetEnumValue((ALchar*)"AL_FORMAT_MONO_FLOAT32")) {
                    return alGetEnumValue((ALchar*)"AL_FORMAT_MONO_FLOAT32");
                }
                break;
            }
        }
        break;
    }
    return AL_FALSE;
}

// close audio device
static void uninit(struct ao *ao)
{
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);

    alDeleteBuffers(NUM_BUF, buffers);
    alDeleteSources(1, &source);

    ALCcontext *ctx = alcGetCurrentContext();
    ALCdevice *dev = alcGetContextsDevice(ctx);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    ao_data = NULL;
}

static int init(struct ao *ao)
{
    float position[3] = {0, 0, 0};
    float direction[6] = {0, 0, -1, 0, 1, 0};
    ALCdevice *dev = NULL;
    ALCcontext *ctx = NULL;
    ALCint freq = 0;
    ALCint attribs[] = {ALC_FREQUENCY, ao->samplerate, 0, 0};
    struct priv *p = ao->priv;
    if (ao_data) {
        MP_FATAL(ao, "Not reentrant!\n");
        return -1;
    }
    ao_data = ao;
    char *dev_name = ao->device;
    dev = alcOpenDevice(dev_name && dev_name[0] ? dev_name : NULL);
    if (!dev) {
        MP_FATAL(ao, "could not open device\n");
        goto err_out;
    }
    ctx = alcCreateContext(dev, attribs);
    alcMakeContextCurrent(ctx);
    alListenerfv(AL_POSITION, position);
    alListenerfv(AL_ORIENTATION, direction);

    alGenSources(1, &source);
    if (p->direct_channels && alGetEnumValue((ALchar*)"AL_DIRECT_CHANNELS_SOFT")) {
        alSourcei(source, alGetEnumValue((ALchar*)"AL_DIRECT_CHANNELS_SOFT"), AL_TRUE);
    }

    cur_buf = 0;
    unqueue_buf = 0;
    alGenBuffers(NUM_BUF, buffers);

    alcGetIntegerv(dev, ALC_FREQUENCY, 1, &freq);
    if (alcGetError(dev) == ALC_NO_ERROR && freq)
        ao->samplerate = freq;

    // Check sample format
    int try_formats[AF_FORMAT_COUNT];
    enum af_format sample_format = 0;
    af_get_best_sample_formats(ao->format, try_formats);
    for (int n = 0; try_formats[n]; n++) {
        sample_format = get_supported_format(try_formats[n]);
        if (sample_format != AF_FORMAT_UNKNOWN) {
            ao->format = try_formats[n];
            break;
        }
    }

    if (sample_format == AF_FORMAT_UNKNOWN) {
        MP_FATAL(ao, "Can't find appropriate sample format.\n");
        uninit(ao);
        goto err_out;
    }

    // Some OpenAL drivers can down-mix 7.1 to any other number of speakers if they
    // support the layout, but not OpenAL Soft when AL_DIRECT_CHANNELS_SOFT is set.
    // There is no way to get how many channels OpenAL has, thus there is an option
    // to enable AL_DIRECT_CHANNELS_SOFT.
    // Check what formats the OpenAL driver supports.
    p->al_format = AL_FALSE;
    int num_channels = ao->channels.num;

    do {
        p->al_format = get_supported_layout(sample_format, num_channels);
        if (p->al_format == AL_FALSE) {
            num_channels = num_channels - 1;
        }
    } while (p->al_format == AL_FALSE && num_channels > 1);

    if (p->al_format == AL_FALSE) {
        MP_FATAL(ao, "Can't find appropriate channel layout.\n");
        uninit(ao);
        goto err_out;
    }

    // Request number of speakers for output from ao.
    struct mp_chmap_sel sel = {0};
    switch (num_channels) {
        case 8:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[0].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[1].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[2].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[3].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[4].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[5].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[7].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[8].id);
            break;
        case 7:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[0].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[1].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[2].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[3].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[4].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[5].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[6].id);
            break;
        case 6:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[0].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[1].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[2].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[3].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[4].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[5].id);
            break;
        case 4:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[0].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[1].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[3].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[4].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[5].id);
            break;
        case 2:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[0].id);
            mp_chmap_sel_add_speaker(&sel, speaker_pos[1].id);
            break;
        case 1:
            mp_chmap_sel_add_speaker(&sel, speaker_pos[3].id);
            break;
    }

    if (!ao_chmap_sel_adjust(ao, &sel, &ao->channels))
        goto err_out;

    if (ao->channels.num != num_channels) {
        num_channels = ao->channels.num;
        do {
            p->al_format = get_supported_layout(sample_format, num_channels);
            if (p->al_format == AL_FALSE) {
                num_channels = num_channels - 1;
            }
        } while (p->al_format == AL_FALSE && num_channels > 1);

        if (p->al_format == AL_FALSE) {
            MP_FATAL(ao, "Can't find appropriate channel layout.\n");
            uninit(ao);
            goto err_out;
        }
    }

    p->chunk_size = CHUNK_SAMPLES * af_fmt_to_bytes(ao->format);
    ao->period_size = CHUNK_SAMPLES;
    return 0;

err_out:
    ao_data = NULL;
    return -1;
}

static void drain(struct ao *ao)
{
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    while (state == AL_PLAYING) {
        mp_sleep_us(10000);
        alGetSourcei(source, AL_SOURCE_STATE, &state);
    }
}

static void unqueue_buffers(void)
{
    ALint p;
    int till_wrap = NUM_BUF - unqueue_buf;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &p);
    if (p >= till_wrap) {
        alSourceUnqueueBuffers(source, till_wrap, &buffers[unqueue_buf]);
        unqueue_buf = 0;
        p -= till_wrap;
    }
    if (p) {
        alSourceUnqueueBuffers(source, p, &buffers[unqueue_buf]);
        unqueue_buf += p;
    }
}

/**
 * \brief stop playing and empty buffers (for seeking/pause)
 */
static void reset(struct ao *ao)
{
    alSourceStop(source);
    unqueue_buffers();
}

/**
 * \brief stop playing, keep buffers (for pause)
 */
static void audio_pause(struct ao *ao)
{
    alSourcePause(source);
}

/**
 * \brief resume playing, after audio_pause()
 */
static void audio_resume(struct ao *ao)
{
    alSourcePlay(source);
}

static int get_space(struct ao *ao)
{
    ALint queued;
    unqueue_buffers();
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    queued = NUM_BUF - queued - 3;
    if (queued < 0)
        return 0;
    return queued * CHUNK_SAMPLES;
}

/**
 * \brief write data into buffer and reset underrun flag
 */
static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct priv *p = ao->priv;
    ALint state;
    int num = samples / CHUNK_SAMPLES;
    for (int i = 0; i < num; i++) {
        char *d = data[0];
        d += i * p->chunk_size * ao->channels.num;
        alBufferData(buffers[cur_buf], p->al_format, d, p->chunk_size * ao->channels.num, ao->samplerate);
        alSourceQueueBuffers(source, 1, &buffers[cur_buf]);
        cur_buf = (cur_buf + 1) % NUM_BUF;
    }
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) // checked here in case of an underrun
        alSourcePlay(source);
    return num * CHUNK_SAMPLES;
}

static double get_delay(struct ao *ao)
{
    ALint queued;
    unqueue_buffers();
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);

    double soft_source_latency = 0;
    if(alIsExtensionPresent("AL_SOFT_source_latency")) {
        ALdouble offsets[2];
        alGetSourcedvSOFT(source, AL_SEC_OFFSET_LATENCY_SOFT, offsets);
        // Additional latency to the play buffer, the remaining seconds to be
        // played minus the offset (seconds already played)
        soft_source_latency = offsets[1] - offsets[0];
    }
    else
    {
        float offset = 0;
        alGetSourcef(source, AL_SEC_OFFSET, &offset);
        soft_source_latency = -offset;
    }

    return (queued * CHUNK_SAMPLES / (double)ao->samplerate) + soft_source_latency;
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_openal = {
    .description = "OpenAL audio output",
    .name      = "openal",
    .init      = init,
    .uninit    = uninit,
    .control   = control,
    .get_space = get_space,
    .play      = play,
    .get_delay = get_delay,
    .pause     = audio_pause,
    .resume    = audio_resume,
    .reset     = reset,
    .drain     = drain,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_FLAG("direct-channels", direct_channels, 0),
        {0}
    },
    .options_prefix = "openal",
};
