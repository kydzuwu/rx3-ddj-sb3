/* Regression: libc may close a GPIO descriptor without calling the preload's
 * close wrapper. Reusing it for a pipe must not suppress reads or readiness. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static void check(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

int main(void)
{
    char path[] = "/tmp/rx3-test-gpiodrv-XXXXXX";
    int initial = mkstemp(path);
    check(initial >= 0, "temporary file");
    close(initial);
    for (int mode = 0; mode < 2; ++mode) {
        int gpio = open(path, O_RDONLY);
        char byte = 'X';
        check(gpio >= 0, "GPIO placeholder open");
        check(read(gpio, &byte, 1) == 1 && byte == 0, "GPIO emulation active");
        check(syscall(SYS_close, gpio) == 0, "internal close bypass");
        int pipefd[2];
        check(pipe(pipefd) == 0 && pipefd[0] == gpio, "descriptor reused");
        check(write(pipefd[1], "Z", 1) == 1, "pipe payload");
        if (mode) {
            struct pollfd ready = { pipefd[0], POLLIN, 0 };
            check(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN),
                  "reused descriptor reports readiness");
        }
        check(read(pipefd[0], &byte, 1) == 1 && byte == 'Z', "real pipe read");
        close(pipefd[0]); close(pipefd[1]);
    }
    unlink(path);
    puts("GPIO descriptor reuse: read and poll passed");
    return 0;
}
