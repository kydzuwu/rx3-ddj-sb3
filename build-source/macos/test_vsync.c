/* Contract: virtual frame waits are paced; unrelated device ioctls stay fast. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static double milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
int main(void) {
    char path[] = "/tmp/rx3-vsync-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unsigned argument = 0;
    int device_result = syscall(SYS_ioctl, fd, 0x1234, &argument);
    int device_errno = errno;
    int frame_result = syscall(SYS_ioctl, fd, 0x40044620UL, &argument);
    int frame_errno = errno;
    double start = milliseconds();
    for (int i = 0; i < 6; ++i)
        assert(ioctl(fd, 0x1234, &argument) == device_result && errno == device_errno);
    double device_time = milliseconds() - start;
    assert(device_time < 60);
    start = milliseconds();
    for (int i = 0; i < 6; ++i)
        assert(ioctl(fd, 0x40044620UL, &argument) == frame_result && errno == frame_errno);
    double frame_time = milliseconds() - start;
    assert(frame_time >= 80);
    close(fd); unlink(path);
    printf("VSYNC pacing: six frame waits %.1f ms; unrelated ioctls %.1f ms\n",
           frame_time, device_time);
}
