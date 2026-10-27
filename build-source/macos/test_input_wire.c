#include <assert.h>
#include <stdio.h>
#include "input_wire.h"
int main(void)
{
    assert(sizeof(struct rx3_input_event) == 16);
    assert(rx3_axis(0, 0, 32767) == 0);
    assert(rx3_axis(32767, 0, 32767) == 2047);
    assert(rx3_axis(16384, 0, 32767) == 1023);
    assert(rx3_axis(-5, 0, 32767) == 0);
    assert(rx3_axis(40000, 0, 32767) == 2047);
    assert(rx3_axis(0, -100, 100) == 1023);
    assert(rx3_axis(100, 100, 100) == 0);
    assert(rx3_axis(0, INT32_MIN, INT32_MAX) == 1023);
    puts("Native input wire size and axis boundaries: passed");
    return 0;
}
