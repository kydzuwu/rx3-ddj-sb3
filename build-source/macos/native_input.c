/* Native evdev reader for the ARM64 guest. Only QEMU's virtual tablet/keyboard
 * are read; normalized ARM32 records go to the translated firmware's FIFO. */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "input_wire.h"

static int emit(int fd, unsigned type, unsigned code, int value)
{
    struct rx3_input_event event = { 0, 0, (uint16_t)type, (uint16_t)code, value };
    ssize_t result;
    do { result = write(fd, &event, sizeof event); } while (result < 0 && errno == EINTR);
    return result == (ssize_t)sizeof event ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "tablet") && strcmp(argv[1], "keyboard"))) return 2;
    int tablet = !strcmp(argv[1], "tablet"), input = -1;
    struct input_absinfo x, y;
    char name[256], path[64];
    for (int attempt = 0; attempt < 100 && input < 0; ++attempt) {
        for (int i = 0; i < 32; ++i) {
            snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            memset(name, 0, sizeof name);
            if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 && strstr(name, "QEMU") &&
                strstr(name, tablet ? "USB Tablet" : "USB Keyboard") &&
                (!tablet || (ioctl(fd, EVIOCGABS(ABS_X), &x) >= 0 &&
                             ioctl(fd, EVIOCGABS(ABS_Y), &y) >= 0))) {
                input = fd; break;
            }
            close(fd);
        }
        if (input < 0) usleep(100000);
    }
    if (input < 0) { fprintf(stderr, "Native %s: virtual input missing\n", argv[1]); return 1; }
    int output = open(argv[2], O_WRONLY);
    if (output < 0) { perror("Native input FIFO"); close(input); return 1; }
    fprintf(stderr, "Native %s ready: %s (%s) -> %s, ARM32 events\n", argv[1], path, name, argv[2]);
    if (tablet && (emit(output, EV_ABS, ABS_X, rx3_axis(x.value, x.minimum, x.maximum)) ||
                   emit(output, EV_ABS, ABS_Y, rx3_axis(y.value, y.minimum, y.maximum)) ||
                   emit(output, EV_SYN, SYN_REPORT, 0))) return 1;
    for (;;) {
        struct input_event event;
        ssize_t bytes = read(input, &event, sizeof event);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes != (ssize_t)sizeof event) break;
        int value = event.value;
        if (tablet && event.type == EV_ABS) {
            if (event.code == ABS_X) value = rx3_axis(value, x.minimum, x.maximum);
            if (event.code == ABS_Y) value = rx3_axis(value, y.minimum, y.maximum);
        }
        if (emit(output, event.type, event.code, value)) break;
    }
    perror("Native input stopped"); close(input); close(output); return 1;
}
