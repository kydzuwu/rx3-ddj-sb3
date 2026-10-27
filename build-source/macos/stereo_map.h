#ifndef RX3_STEREO_MAP_H
#define RX3_STEREO_MAP_H
#include <stddef.h>

/* CoreAudio's first two device channels carry stereo master. Extra channels
 * remain silent until the player has a separate headphone output bridge. */
static inline void rx3_map_stereo(float *destination, unsigned channels,
                                 unsigned first_channel, const float *stereo,
                                 size_t frames)
{
    for (size_t frame = 0; frame < frames; ++frame)
        for (unsigned channel = 0; channel < channels; ++channel) {
            unsigned global = first_channel + channel;
            destination[frame * channels + channel] =
                global < 2 ? stereo[frame * 2 + global] : 0.0f;
        }
}
#endif
