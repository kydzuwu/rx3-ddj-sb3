#include <assert.h>
#include <stdio.h>
#include "stereo_map.h"
int main(void) {
    const float source[] = { 1, -1, 2, -2, 3, -3 };
    float four[12], left[3], right[3], headphone[6];
    rx3_map_stereo(four, 4, 0, source, 3);
    rx3_map_stereo(left, 1, 0, source, 3);
    rx3_map_stereo(right, 1, 1, source, 3);
    rx3_map_stereo(headphone, 2, 2, source, 3);
    for (int frame = 0; frame < 3; ++frame) {
        assert(four[frame * 4] == frame + 1);
        assert(four[frame * 4 + 1] == -(frame + 1));
        assert(four[frame * 4 + 2] == 0 && four[frame * 4 + 3] == 0);
        assert(left[frame] == frame + 1 && right[frame] == -(frame + 1));
        assert(headphone[frame * 2] == 0 && headphone[frame * 2 + 1] == 0);
    }
    puts("CoreAudio channel mapping: interleaved and split buffers passed");
}
