/* Read-only diagnostic for the supported RX3 v1.20 process in the Linux guest. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../scripts/shims/rbp_abi.h"
static int memory;
static uint64_t guest_base;
static uint32_t word(uint32_t address)
{
    uint32_t value;
    if (pread(memory, &value, sizeof value, guest_base + address) != sizeof value) {
        fprintf(stderr, "CFX read 0x%08x: %s\n", address, strerror(errno)); exit(1);
    }
    return value;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char path[64]; snprintf(path, sizeof path, "/proc/%s/mem", argv[1]);
    memory = open(path, O_RDONLY);
    if (memory < 0) { perror("CFX process memory"); return 1; }
    /* qemu-arm can reserve a guest base (one page on this build). The stock
     * v1.20 executable's first ELF load is at 0x8000, file offset zero. */
    snprintf(path, sizeof path, "/proc/%s/maps", argv[1]);
    FILE *maps = fopen(path, "r");
    if (!maps) return 1;
    char line[1024], permissions[8]; unsigned long long start, end_address, offset;
    while (fgets(line, sizeof line, maps)) {
        if (strstr(line, "/root/pdj/rbp") &&
            sscanf(line, "%llx-%llx %7s %llx", &start, &end_address, permissions, &offset) == 4 &&
            offset == 0) { guest_base = start - 0x8000; break; }
    }
    fclose(maps);
    uint32_t engine = word(ME_SINGLETON);
    if (!engine) { puts("CFX mixer is not ready"); return 1; }
    uint32_t begin = word(engine + 12), end = word(engine + 16);
    printf("CFX engine=0x%08x channels=[0x%08x..0x%08x]\n", engine, begin, end);
    if (!begin || end < begin || end - begin > 64) return 1;
    for (uint32_t position = begin; position < end; position += 4) {
        uint32_t channel = word(position);
        if (!channel) continue;
        uint32_t manager = word(channel + 0x1e0);
        if (!manager) { puts("CFX manager is not ready"); continue; }
        uint32_t controls = word(manager), bits = controls ? word(controls + 28) : 0;
        float color; memcpy(&color, &bits, sizeof color);
        printf("CFX input=%u type=%u color=%.6f manager_state=%u\n",
               word(channel + 20), word(manager + 36), (double)color, word(manager + 128));
    }
    close(memory); return 0;
}
