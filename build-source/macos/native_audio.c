/* Runs natively in the ARM64 guest. Receives 44.1 kHz S16_LE packets (2 channels,
 * or 4 = master + headphone cue mix) from the translated player, and owns all ALSA
 * operations and resampling. QEMU's audio mixer is stereo-only, so 4-channel input
 * goes to two stereo cards: channels 0/1 to MASTER_PCM, 2/3 to PHONES_PCM.
 * Usage: native-audio SOCKET [PERIODS [CHANNELS [MASTER_PCM PHONES_PCM]]] */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static snd_pcm_t *open_stereo(const char *name, unsigned periods)
{
    snd_pcm_t *pcm;
    int error = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) {
        fprintf(stderr, "Native audio open %s: %s\n", name, snd_strerror(error));
        return NULL;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    unsigned rate = 44100;
    snd_pcm_uframes_t period = 256, buffer;
    if ((error = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (error = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (error = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (error = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (error = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
        rate != 44100 ||
        (error = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
        (error = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, NULL)) < 0 ||
        (error = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "Native audio configure %s: %s, rate %u\n", name, snd_strerror(error), rate);
        snd_pcm_close(pcm); return NULL;
    }
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer / 2);
    snd_pcm_sw_params_set_stop_threshold(pcm, sw, buffer);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    if ((error = snd_pcm_sw_params(pcm, sw)) < 0) {
        fprintf(stderr, "Native audio thresholds %s: %s\n", name, snd_strerror(error));
        snd_pcm_close(pcm); return NULL;
    }
    fprintf(stderr, "Native audio ready: %s 44100 Hz stereo S16_LE, period %lu, buffer %lu frames\n",
            name, (unsigned long)period, (unsigned long)buffer);
    return pcm;
}

/* Blocking write of one 256-frame stereo block; 0 when the PCM is lost. */
static int deliver(snd_pcm_t *pcm, const short *block, unsigned long long *recoveries)
{
    size_t delivered = 0;
    while (delivered < 256) {
        snd_pcm_sframes_t result = snd_pcm_writei(pcm, block + delivered * 2, 256 - delivered);
        if (result < 0) {
            if (snd_pcm_recover(pcm, (int)result, 1) < 0) {
                fprintf(stderr, "Native audio lost: %s\n", snd_strerror((int)result));
                return 0;
            }
            ++*recoveries;
        } else if (result > 0) delivered += (size_t)result;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc == 5 || argc > 6) return 2;
    unsigned periods = 4;
    if (argc >= 3) {
        char *end;
        unsigned long requested = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || requested < 4 || requested > 64) {
            fprintf(stderr, "Native audio periods must be 4..64\n");
            return 2;
        }
        periods = (unsigned)requested;
    }
    unsigned channels = 2;
    if (argc >= 4) {
        if (strcmp(argv[3], "2") && strcmp(argv[3], "4")) {
            fprintf(stderr, "Native audio channels must be 2 or 4\n");
            return 2;
        }
        channels = (unsigned)(argv[3][0] - '0');
    }
    const char *master_name = argc == 6 ? argv[4] : "plughw:0,0";
    const char *phones_name = argc == 6 ? argv[5] : "plughw:1,0";
    snd_pcm_t *master = open_stereo(master_name, periods), *phones = NULL;
    if (!master) return 1;
    if (channels == 4 && !(phones = open_stereo(phones_name, periods))) {
        snd_pcm_close(master); return 1;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(argv[1]) >= sizeof address.sun_path) return 2;
    strcpy(address.sun_path, argv[1]);
    unlink(argv[1]);
    int capacity = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof capacity);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address)) {
        perror("Native audio socket"); return 1;
    }
    fprintf(stderr, "Native audio input: %u-channel\n", channels);
    short packet[8192], block[256 * 4], master_block[256 * 2], phones_block[256 * 2];
    const size_t block_shorts = 256 * (size_t)channels;
    size_t used = 0;
    unsigned long long frames = 0, recoveries = 0;
    for (;;) {
        ssize_t bytes = recv(fd, packet, sizeof packet, MSG_TRUNC);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0 || bytes > (ssize_t)sizeof packet || bytes % (ssize_t)(2 * channels)) continue;
        size_t count = (size_t)bytes / sizeof(short), offset = 0;
        while (offset < count) {
            size_t take = count - offset;
            if (take > block_shorts - used) take = block_shorts - used;
            memcpy(block + used, packet + offset, take * sizeof(short));
            used += take; offset += take;
            if (used != block_shorts) continue;
            const short *master_out = block;
            if (phones) {
                for (size_t frame = 0; frame < 256; ++frame) {
                    memcpy(master_block + frame * 2, block + frame * 4, 2 * sizeof(short));
                    memcpy(phones_block + frame * 2, block + frame * 4 + 2, 2 * sizeof(short));
                }
                master_out = master_block;
            }
            if (!deliver(master, master_out, &recoveries) ||
                (phones && !deliver(phones, phones_block, &recoveries))) {
                close(fd); unlink(argv[1]); return 1;
            }
            used = 0;
            frames += 256;
            if (frames % (256 * 1723) == 0)
                fprintf(stderr, "Native audio: %llu frames delivered, %llu recoveries\n", frames, recoveries);
        }
    }
}
