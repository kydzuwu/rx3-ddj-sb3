#ifndef RX3_INPUT_WIRE_H
#define RX3_INPUT_WIRE_H
#include <stdint.h>
#include "../scripts/shims/point_xform.h"

/* Linux ARM32 input_event: the firmware cannot read ARM64's 24-byte records. */
struct rx3_input_event {
    int32_t seconds, microseconds;
    uint16_t type, code;
    int32_t value;
};
_Static_assert(sizeof(struct rx3_input_event) == 16, "ARM32 evdev wire size");

static inline int rx3_axis(int value, int minimum, int maximum)
{
    if (maximum <= minimum) return POINT_RAW_MIN_DEFAULT;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return POINT_RAW_MIN_DEFAULT + (int)(((int64_t)value - minimum) *
        (POINT_RAW_MAX_DEFAULT - POINT_RAW_MIN_DEFAULT) / ((int64_t)maximum - minimum));
}
#endif
