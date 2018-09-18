/*
 * QEMU Audio subsystem stubs
 *
 * Copyright (c) 2018, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "audio.h"
#include "ui/qemu-spice.h"

void AUD_help (void)
{
}

void AUD_log (const char *cap, const char *fmt, ...)
{
}

void AUD_close_out (QEMUSoundCard *card, SWVoiceOut *sw)
{
}

void AUD_register_card (const char *name, QEMUSoundCard *card)
{
}

void AUD_remove_card (QEMUSoundCard *card)
{
}

void AUD_set_active_out (SWVoiceOut *sw, int on)
{
}

int  AUD_is_active_out (SWVoiceOut *sw)
{
    return -1;
}

void AUD_set_volume_out (SWVoiceOut *sw, int mute, uint8_t lvol, uint8_t rvol)
{
}

void AUD_set_volume_in (SWVoiceIn *sw, int mute, uint8_t lvol, uint8_t rvol)
{
}

CaptureVoiceOut *AUD_add_capture (
    struct audsettings *as,
    struct audio_capture_ops *ops,
    void *opaque
    )
{
    return NULL;
}

void AUD_del_capture (CaptureVoiceOut *cap, void *cb_opaque)
{
}

SWVoiceOut *AUD_open_out (
    QEMUSoundCard *card,
    SWVoiceOut *sw,
    const char *name,
    void *callback_opaque,
    audio_callback_fn callback_fn,
    struct audsettings *settings
    )
{
    return NULL;
}

int  AUD_write (SWVoiceOut *sw, void *pcm_buf, int size)
{
    return -1;

}

int  AUD_get_buffer_size_out (SWVoiceOut *sw)
{
    return -1;
}

SWVoiceIn *AUD_open_in (
    QEMUSoundCard *card,
    SWVoiceIn *sw,
    const char *name,
    void *callback_opaque,
    audio_callback_fn callback_fn,
    struct audsettings *settings
    )
{
    return NULL;
}

void AUD_close_in (QEMUSoundCard *card, SWVoiceIn *sw)
{
}

int  AUD_read (SWVoiceIn *sw, void *pcm_buf, int size)
{
    return -1;
}

void AUD_set_active_in (SWVoiceIn *sw, int on)
{
}

int  AUD_is_active_in (SWVoiceIn *sw)
{
    return -1;
}

void AUD_init_time_stamp_in (SWVoiceIn *sw, QEMUAudioTimeStamp *ts)
{
}

uint64_t AUD_get_elapsed_usec_in (SWVoiceIn *sw, QEMUAudioTimeStamp *ts)
{
    return 0;
}

void AUD_init_time_stamp_out (SWVoiceOut *sw, QEMUAudioTimeStamp *ts)
{
}

uint64_t AUD_get_elapsed_usec_out (SWVoiceOut *sw, QEMUAudioTimeStamp *ts)
{
    return 0;
}

void audio_cleanup(void)
{
}

void audio_sample_to_uint64(void *samples, int pos,
                            uint64_t *left, uint64_t *right)
{
}

void audio_sample_from_uint64(void *samples, int pos,
                            uint64_t left, uint64_t right)
{
}

int wav_start_capture (CaptureState *s, const char *path, int freq,
                       int bits, int nchannels)
{
    return -1;
}

#ifdef CONFIG_SPICE
void qemu_spice_audio_init (void)
{
}
#endif
