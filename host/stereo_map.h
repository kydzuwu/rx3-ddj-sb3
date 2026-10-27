#ifndef RX3_STEREO_MAP_H
#define RX3_STEREO_MAP_H
#include <stddef.h>

/* QEMU's voice is either stereo master (copied to device channels 1/2 and 3/4,
 * the SB3 headphones) or multichannel from the guest: 0/1 master, 2/3 cue mix,
 * 4/5 padding (the QEMU USB card has no 4-channel mode). Source channel n goes
 * to device channel n; missing ones stay silent. */
static inline void rx3_map_channels(float *destination, unsigned channels,
                                    unsigned first_channel, const float *source,
                                    unsigned source_channels, size_t frames)
{
    for (size_t frame = 0; frame < frames; ++frame)
        for (unsigned channel = 0; channel < channels; ++channel) {
            unsigned global = first_channel + channel;
            float sample = 0.0f;
            if (source_channels == 2) {
                if (global < 4) sample = source[frame * 2 + (global & 1)];
            } else if (global < 4 && global < source_channels) {
                sample = source[frame * source_channels + global];
            }
            destination[frame * channels + channel] = sample;
        }
}

/* Write only device channels [low, high) (global numbering) from the source; leave the
 * rest untouched so another IOProc's channels survive in a shared buffer. Stereo
 * sources map their pair onto every pair in range (master -> 1/2 and 3/4). */
static inline void rx3_map_range(float *destination, unsigned channels,
                                 unsigned first_channel, const float *source,
                                 unsigned source_channels, size_t frames,
                                 unsigned low, unsigned high)
{
    for (size_t frame = 0; frame < frames; ++frame)
        for (unsigned channel = 0; channel < channels; ++channel) {
            unsigned global = first_channel + channel;
            if (global < low || global >= high) continue;
            float sample = 0.0f;
            if (source_channels == 2) {
                if (global < 4) sample = source[frame * 2 + (global & 1)];
            } else if (global < 4 && global < source_channels) {
                sample = source[frame * source_channels + global];
            }
            destination[frame * channels + channel] = sample;
        }
}

static inline void rx3_map_stereo(float *destination, unsigned channels,
                                 unsigned first_channel, const float *stereo,
                                 size_t frames)
{
    rx3_map_channels(destination, channels, first_channel, stereo, 2, frames);
}
#endif
