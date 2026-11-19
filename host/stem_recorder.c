/* Set recorder: reads deck audio and position from the guest (rbp_stems.c) over
 * its virtio port. Each press of REC on the player makes one session folder in
 * ~/Music/RX3 Sets, and stopping builds the Ableton Set from it:
 *   deck1.wav deck2.wav          the deck as it goes to master
 *   deck1_clean.wav ...          the deck before trim, EQ, filter and fader
 *   master.wav                   the player's master output
 *   state.csv                    position, BPM, tempo master against audio frames
 *   grid.csv                     each loaded track's beat table, as the player holds it
 *   title_lines.csv              the player's title entry at each track load, as hex
 * All files share one frame counter: frame N of every WAV is the same moment, and
 * anything dropped in the guest is written as silence so the files stay in step.
 * Usage: stem-recorder <socket>. Ends when QEMU closes the socket, which also ends a
 * recording in progress. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <libgen.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x54535852u
#define RATE 44100u
#define MAX_FRAMES 1024
#define MAX_BEATS 40000

struct head { uint32_t magic, type, deck, frames; uint64_t start; };
struct wav { FILE *f; uint64_t frames; };

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_header(struct wav *w) {
    uint32_t bytes = (uint32_t)(w->frames * 8);
    long at = ftell(w->f);
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f); put32(w->f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, w->f);
    put32(w->f, 16); put16(w->f, 3); put16(w->f, 2); put32(w->f, RATE);
    put32(w->f, RATE * 8); put16(w->f, 8); put16(w->f, 32);
    fwrite("data", 1, 4, w->f); put32(w->f, bytes);
    if (at > 44) fseek(w->f, at, SEEK_SET);
    fflush(w->f);
}

static int read_all(int fd, void *data, size_t size) {
    char *p = data;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static void wav_put(struct wav *w, uint64_t at, const float *audio, uint32_t frames) {
    static const float zero[2];
    while (w->frames < at) { fwrite(zero, 8, 1, w->f); w->frames++; } /* dropped: silence */
    if (at < w->frames) return;
    fwrite(audio, 8, frames, w->f);
    w->frames += frames;
}

static char tool_path[1200];

struct session {
    char dir[900];
    struct wav wavs[5];
    FILE *csv, *grid, *titles;
    float peak[4];
    int have_base;
    uint64_t base, since;
};

static int session_open(struct session *s) {
    char path[1000], stamp[32];
    const char *home = getenv("HOME");
    time_t now = time(NULL);
    static const char *names[5] = {"deck1", "deck2", "deck1_clean", "deck2_clean", "master"};
    memset(s, 0, sizeof *s);
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", localtime(&now));
    snprintf(s->dir, sizeof s->dir, "%s/Music", home);
    mkdir(s->dir, 0755);
    snprintf(s->dir, sizeof s->dir, "%s/Music/RX3 Sets", home);
    mkdir(s->dir, 0755);
    snprintf(s->dir, sizeof s->dir, "%s/Music/RX3 Sets/%s", home, stamp);
    for (int n = 2; mkdir(s->dir, 0755) != 0; ++n) {   /* never reuse a folder */
        if (errno != EEXIST || n > 99) { perror(s->dir); return -1; }
        snprintf(s->dir, sizeof s->dir, "%s/Music/RX3 Sets/%s_%d", home, stamp, n);
    }
    for (int i = 0; i < 5; ++i) {
        snprintf(path, sizeof path, "%s/%s.wav", s->dir, names[i]);
        s->wavs[i].f = fopen(path, "wb");
        if (!s->wavs[i].f) { perror(path); return -1; }
        wav_header(&s->wavs[i]);
    }
    snprintf(path, sizeof path, "%s/state.csv", s->dir);
    s->csv = fopen(path, "w");
    snprintf(path, sizeof path, "%s/grid.csv", s->dir);
    s->grid = fopen(path, "w");
    snprintf(path, sizeof path, "%s/title_lines.csv", s->dir);
    s->titles = fopen(path, "w");
    if (!s->csv || !s->grid || !s->titles) { perror(path); return -1; }
    fputs("load_frame,deck,title_hex,artist_hex\n", s->titles);
    fputs("load_frame,deck,index,beat_in_bar,bpm_x100,time\n", s->grid);
    fputs("frame,deck,playing_time_raw,bpm_raw,grid_offset_raw,is_playing,tempo_x100_raw,total_time_raw,tempo_master,dropped_blocks,fader,trim,eq0,eq1,eq2,color,crossfader,color_type\n", s->csv);
    fprintf(stderr, "stem-recorder: recording to %s\n", s->dir);
    return 0;
}

/* Build the Set in its own process: a long set must not hold up the next recording. */
static void session_close(struct session *s) {
    for (int i = 0; i < 5; ++i) { wav_header(&s->wavs[i]); fclose(s->wavs[i].f); }
    fclose(s->csv);
    fclose(s->grid);
    fclose(s->titles);
    fprintf(stderr, "stem-recorder: stopped, %.1f s in %s\n", (double)s->wavs[0].frames / RATE, s->dir);
    if (s->wavs[0].frames < RATE) {   /* under a second: nothing was played, leave no folder */
        static const char *files[8] = {"deck1.wav", "deck2.wav", "deck1_clean.wav", "deck2_clean.wav", "master.wav", "state.csv", "grid.csv", "title_lines.csv"};
        char path[1000];
        for (int i = 0; i < 8; ++i) {
            snprintf(path, sizeof path, "%s/%s", s->dir, files[i]);
            unlink(path);
        }
        rmdir(s->dir);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/python3", "python3", tool_path, s->dir, (char *)NULL);
        _exit(127);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    /* Contents/Helpers/stem-recorder -> Contents/Resources/set_to_ableton.py */
    char self[1024];
    snprintf(self, sizeof self, "%s", argv[0]);
    snprintf(tool_path, sizeof tool_path, "%s/../Resources/set_to_ableton.py", dirname(self));
    signal(SIGCHLD, SIG_IGN);

    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof address.sun_path) return 2;
    strcpy(address.sun_path, argv[1]);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    while (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) sleep(1);
    fprintf(stderr, "stem-recorder: connected, waiting for REC\n");

    static float audio[MAX_FRAMES * 4];
    static unsigned char beats[MAX_BEATS * 8];
    static struct session s;
    int recording = 0;
    struct head h;
    while (!read_all(fd, &h, sizeof h)) {
        if (h.magic != MAGIC) { fprintf(stderr, "stem-recorder: lost sync\n"); break; }
        if (h.type == 4) {
            if (recording) session_close(&s);
            recording = !session_open(&s);
            continue;
        }
        if (h.type == 5) {
            if (recording) session_close(&s);
            recording = 0;
            continue;
        }
        if (h.type == 2) {
            uint32_t v[16];
            float m[7];
            if (h.frames != sizeof v || read_all(fd, v, sizeof v)) break;
            memcpy(m, v + 9, sizeof m);
            if (recording && s.have_base && h.start >= s.base)
                fprintf(s.csv, "%llu,%u,%u,0x%08x,%u,%u,%u,%u,%u,%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%u\n",
                        (unsigned long long)(h.start - s.base), h.deck + 1, v[0], v[1], v[2],
                        v[3], v[4], v[5], v[6], v[7], m[0], m[1], m[2], m[3], m[4], m[5], m[6], v[8]);
            continue;
        }
        if (h.type == 3) {
            if (h.frames > sizeof beats || h.frames % 8 || read_all(fd, beats, h.frames)) break;
            if (!recording) continue;
            for (uint32_t i = 0; i < h.frames / 8; ++i) {
                uint16_t beat, bpm;
                uint32_t at;
                memcpy(&beat, beats + i * 8, 2);
                memcpy(&bpm, beats + i * 8 + 2, 2);
                memcpy(&at, beats + i * 8 + 4, 4);
                fprintf(s.grid, "%llu,%u,%u,%u,%u,%u\n", (unsigned long long)(s.have_base && h.start >= s.base ? h.start - s.base : 0),
                        h.deck + 1, i, beat, bpm, at);
            }
            fflush(s.grid);
            fprintf(stderr, "stem-recorder: deck %u grid, %u beats\n", h.deck + 1, h.frames / 8);
            continue;
        }
        if (h.type == 7) {
            static unsigned char text[1024];
            if (h.frames != sizeof text || read_all(fd, text, sizeof text)) break;
            if (!recording) continue;
            fprintf(s.titles, "%llu,%u,", (unsigned long long)(s.have_base && h.start >= s.base ? h.start - s.base : 0), h.deck + 1);
            for (int i = 0; i < 512; ++i) fprintf(s.titles, "%02x", text[i]);
            fputc(',', s.titles);
            for (int i = 512; i < 1024; ++i) fprintf(s.titles, "%02x", text[i]);
            fputc('\n', s.titles);
            fflush(s.titles);
            continue;
        }
        if (h.type == 6) {
            if (h.frames > MAX_FRAMES || read_all(fd, audio, (size_t)h.frames * 8)) break;
            if (recording && s.have_base && h.start >= s.base)
                wav_put(&s.wavs[4], h.start - s.base, audio, h.frames);
            continue;
        }
        if (h.type != 1 || h.frames > MAX_FRAMES || h.deck > 1) { fprintf(stderr, "stem-recorder: bad packet\n"); break; }
        if (read_all(fd, audio, (size_t)h.frames * 16)) break;
        if (!recording) continue;
        if (!s.have_base) { s.have_base = 1; s.base = h.start; }
        if (h.start < s.base) continue;
        const float *post = audio, *clean = audio + h.frames * 2;
        wav_put(&s.wavs[h.deck], h.start - s.base, post, h.frames);
        wav_put(&s.wavs[h.deck + 2], h.start - s.base, clean, h.frames);
        for (uint32_t i = 0; i < h.frames * 2; ++i) {
            float a = post[i] < 0 ? -post[i] : post[i], b = clean[i] < 0 ? -clean[i] : clean[i];
            if (a > s.peak[h.deck]) s.peak[h.deck] = a;
            if (b > s.peak[h.deck + 2]) s.peak[h.deck + 2] = b;
        }
        s.since += h.frames;
        if (s.since >= RATE * 10) {   /* both decks counted: about every 5 s */
            for (int i = 0; i < 5; ++i) wav_header(&s.wavs[i]);
            fflush(s.csv);
            fprintf(stderr, "stem-recorder: %.0f s, peak deck1 %.3f deck2 %.3f, clean %.3f %.3f\n",
                    (double)s.wavs[0].frames / RATE, s.peak[0], s.peak[1], s.peak[2], s.peak[3]);
            memset(s.peak, 0, sizeof s.peak);
            s.since = 0;
        }
    }
    if (recording) session_close(&s);
    return 0;
}
