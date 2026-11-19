/*
 * rbp_stems.c -- record each deck's audio and position for the set recorder.
 *
 * rbp renders every deck in mixerengine::MixerChannel::update(int), before the
 * decks are mixed into master. That function's last step leaves the deck's audio
 * in the channel's own buffers, so a hook on it can copy them out:
 *   channel+0x20  Float2*  the deck on its way to master, as played: trim, EQ, filter, fader
 * and, before the call, MixerRouteMngr::getPlayerDataPointer holds the same block clean.
 * (+0x20 confirmed from recordings; the clean block is still to be checked on one.)
 *
 * The REC button on the menu band (pointsrc.c) starts and stops a recording; nothing
 * is copied or written in between. The port is nr=2 next to the MIDI port (nr=1). The audio thread only
 * copies into a per-deck ring; a separate thread does the slow write and polls
 * position and BPM through the same DjEngineIF getters rbp's UI uses. Every packet
 * carries the deck's frame counter, so the Mac side can keep the files in step
 * across anything dropped here.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "shimutil.h"
#include "rbp_transport.h"

extern int is_rbp_process(void);

#define ADDR_MIXERCHANNEL_UPDATE 0x0009e890UL
#define PROLOGUE_UPDATE_0        0xE59030D0u   /* ldr r3,[r0,#0xd0] */
#define PROLOGUE_UPDATE_1        0xE92D4070u   /* push {r4,r5,r6,lr} */
/* What the firmware's own recording dot and timer read: MixerEngine::getRecordingStatus and
 * getRecElapsedSec. Pointed at this recorder, so the player shows it as its own. */
#define ADDR_MIXER_REC_STATUS    0x00057854UL
#define ADDR_MIXER_REC_ELAPSED   0x000578d8UL
#define PROLOGUE_REC_0           0xE5900054u   /* ldr r0,[r0,#0x54] */
#define PROLOGUE_REC_1           0xE3500000u   /* cmp r0,#0 */
#define CH_OFF_INPUT             0x14          /* EnMixerInput: 0/1 = decks */
/* MasterOutChannel::update (read from 0x59490): channel+0x20 is the summed output it leaves. */
#define ADDR_MASTER_UPDATE       0x00059490UL
#define PROLOGUE_MASTER_0        0xE92D40F8u   /* push {r3,r4,r5,r6,r7,lr} */
#define PROLOGUE_MASTER_1        0xE1A04000u   /* mov r4,r0 */
#define MASTER_RING              2
/* What the screen draws as the deck's title: getUiPlayTitleLinePointer @0x112530, one fixed
 * 568-byte entry per deck. The UiHid title getters return nothing without a PC link (measured). */
#define ADDR_TITLE_LINES         0x0326ef78UL
#define TITLE_LINE_BYTES         568
#define TITLE_BYTES              512
#define ADDR_PLAYER_DATA         0x00085898UL   /* MixerRouteMngr::getPlayerDataPointer */
#define CH_OFF_POST              0x20

/* DjEngineIF's getters ignore `this` and use this PlayEngine singleton, creating
 * it if it is missing, so it is checked first and they are never the creator. */
#define ADDR_PLAYENGINE_SINGLETON 0x011497d0UL
#define ADDR_GET_PLAYING_TIME     0x00045aecUL
#define ADDR_IS_PLAYING           0x00045984UL
#define ADDR_GET_TEMPO_X100       0x00045dbcUL
#define ADDR_GET_GRID_OFFSET      0x0004a7b0UL
#define ADDR_GET_PLAYER_BPM       0x0004aca0UL
#define ADDR_GET_TOTAL_TIME       0x00045454UL
#define ADDR_GET_SYNC_MASTER      0x0004b450UL

/* Where each mixer control really is, whoever moved it. MixerEngine reads its own channel
 * list, keyed by EnMixerInput (0/1 = decks). Floats, returned in r0 (soft float). */
#define ADDR_MIXER_SINGLETON      0x011493c0UL
#define ADDR_MIXER_GET_FADER      0x00056f18UL
#define ADDR_MIXER_GET_TRIM       0x00056e58UL
#define ADDR_MIXER_GET_EQ         0x00057134UL
#define ADDR_MIXER_GET_COLOR      0x00057480UL
#define ADDR_MIXER_GET_XFADER     0x00056fd4UL
#define ADDR_MIXER_GET_COLOR_TYPE 0x00057408UL

#define STEM_MAGIC     0x54535852u   /* "RXST" */
#define STEM_T_AUDIO   1u
#define STEM_T_STATE   2u
#define STEM_T_GRID    3u
#define STEM_T_START   4u
#define STEM_T_STOP    5u
#define STEM_T_MASTER  6u
#define STEM_T_TITLE   7u
#define MAX_BEATS      40000
#define MAX_FRAMES     1024
#define RING_SLOTS     256
#define STATE_EVERY_US 20000

struct packet_head {
     uint32_t magic, type, deck, frames;
     uint64_t start;
};

struct slot {
     uint32_t frames;
     uint64_t start;
     float post[MAX_FRAMES * 2];
     float clean[MAX_FRAMES * 2];
};

struct ring {
     volatile uint32_t head, tail;
     volatile uint32_t dropped;
     volatile uint64_t frames;
     struct slot *slots;
};

static struct ring rings[3];   /* deck 1, deck 2, master */
static int stem_fd = -1;
static void (*orig_update)(void *, int);
static void (*orig_master)(void *, int);

/* The shim builds for armv5t but rbp itself needs NEON, so the CPU has dmb. */
static inline void barrier(void)
{
     __asm__ volatile(".arch armv7-a\n\tdmb ish\n\t.arch armv5t" : : : "memory");
}

static volatile uint64_t rec_started_at;

static int rec_status_hook(void *engine)
{
     (void)engine;
     return rbp_stems_get() ? 1 : 0;
}

static unsigned rec_elapsed_hook(void *engine)
{
     (void)engine;
     return rbp_stems_get() ? (unsigned)((rings[0].frames - rec_started_at) / 44100u) : 0;
}

static void stem_update_hook(void *self, int frames)
{
     struct slot *s = NULL;
     struct ring *r = NULL;
     int deck = *(int *)((char *)self + CH_OFF_INPUT);

     if (deck >= 0 && deck <= 1 && frames > 0) {
          r = &rings[deck];
          uint64_t start = r->frames;
          r->frames = start + (uint64_t)frames;
          if (rbp_stems_get()) {
               uint32_t head = r->head;
               if (frames > MAX_FRAMES || head - r->tail >= RING_SLOTS) {
                    r->dropped++;
               } else {
                    /* Clean: what the player hands the mixer, before trim, EQ, filter, fader.
                     * update() reads this same block, so it has to be taken here. */
                    const void *raw = ((const void *(*)(int))ADDR_PLAYER_DATA)(deck);
                    s = &r->slots[head % RING_SLOTS];
                    s->frames = (uint32_t)frames;
                    s->start = start;
                    if (raw)
                         memcpy(s->clean, raw, (size_t)frames * 8);
                    else
                         memset(s->clean, 0, (size_t)frames * 8);
               }
          }
     }
     orig_update(self, frames);
     if (s) {
          memcpy(s->post, *(void **)((char *)self + CH_OFF_POST), (size_t)frames * 8);
          barrier();
          r->head = r->head + 1;
     }
}

static void master_update_hook(void *self, int frames)
{
     orig_master(self, frames);
     struct ring *r = &rings[MASTER_RING];
     uint32_t head = r->head;
     if (!rbp_stems_get() || frames <= 0)
          return;
     if (frames > MAX_FRAMES || head - r->tail >= RING_SLOTS) {
          r->dropped++;
          return;
     }
     /* The mixer updates the decks first, so this block started where deck 1's did. */
     struct slot *s = &r->slots[head % RING_SLOTS];
     s->frames = (uint32_t)frames;
     s->start = rings[0].frames - (uint64_t)frames;
     memcpy(s->post, *(void **)((char *)self + CH_OFF_POST), (size_t)frames * 8);
     barrier();
     r->head = head + 1;
}

static int write_all(const void *data, size_t size)
{
     const char *p = data;
     while (size) {
          ssize_t n = write(stem_fd, p, size);
          if (n < 0)
               return -1;
          p += n;
          size -= (size_t)n;
     }
     return 0;
}

/* The loaded track's beat grid (common::BeatPosition, the layout read from
 * Player::getQuantizedBpm @0x68534): Player+0xc8 -> ReaderOwner, +4 -> reader,
 * +0x69c -> BeatPosition, whose vector<myBeat> is at +0x34/+0x38. Each myBeat is
 * 8 bytes: u16 beat in bar, u16 BPM x 100, u32 time. Sent once per loaded track. */
static const void *beat_table(int deck, uint32_t *count)
{
     char *engine = *(char **)ADDR_PLAYENGINE_SINGLETON;
     char **begin = *(char ***)(engine + 0xc), **end = *(char ***)(engine + 0x10);
     *count = 0;
     if (!begin || end - begin <= deck || !begin[deck])
          return NULL;
     char *owner = *(char **)(begin[deck] + 0xc8);
     char *reader = owner ? *(char **)(owner + 4) : NULL;
     char *beats = reader ? *(char **)(reader + 0x69c) : NULL;
     if (!beats)
          return NULL;
     char *first = *(char **)(beats + 0x34), *last = *(char **)(beats + 0x38);
     if (!first || last <= first || (size_t)(last - first) > MAX_BEATS * 8u || (last - first) % 8)
          return NULL;
     *count = (uint32_t)(last - first) / 8;
     return first;
}

static const void *sent[2];   /* cleared at each start: tracks already loaded count too */
static uint32_t sent_count[2], sent_total[2], seen_count[2], seen_polls[2];

static void send_grid(int deck, uint32_t total_time)
{
     static char copy[MAX_BEATS * 8];
     uint32_t count;
     const void *table = beat_table(deck, &count);
     if (!table || (table == sent[deck] && count == sent_count[deck] &&
                    total_time == sent_total[deck]))
          return;
     /* Wait for a table that holds still: a track may still be loading. */
     if (count != seen_count[deck]) {
          seen_count[deck] = count;
          seen_polls[deck] = 0;
          return;
     }
     if (++seen_polls[deck] < 10)
          return;
     memcpy(copy, table, count * 8u);
     sent[deck] = table;
     sent_count[deck] = count;
     sent_total[deck] = total_time;
     struct packet_head h = { STEM_MAGIC, STEM_T_GRID, (uint32_t)deck,
                              count * 8u, rings[deck].frames };
     write_all(&h, sizeof(h));
     write_all(copy, count * 8u);

     /* A new track, so its name as well. Empty if the player has none to give. */
     static char text[TITLE_BYTES * 2];
     memset(text, 0, sizeof(text));
     memcpy(text, (const char *)ADDR_TITLE_LINES + deck * TITLE_LINE_BYTES, TITLE_LINE_BYTES);
     struct packet_head t = { STEM_MAGIC, STEM_T_TITLE, (uint32_t)deck,
                              sizeof(text), rings[deck].frames };
     write_all(&t, sizeof(t));
     write_all(text, sizeof(text));
}

static void send_state(int deck)
{
     typedef uint32_t (*getter)(void *, int);
     uint32_t state[16];
     state[0] = ((getter)ADDR_GET_PLAYING_TIME)(NULL, deck);
     state[1] = ((uint32_t (*)(void *, int, int))ADDR_GET_PLAYER_BPM)(NULL, deck, 0);
     state[2] = ((getter)ADDR_GET_GRID_OFFSET)(NULL, deck);
     state[3] = ((getter)ADDR_IS_PLAYING)(NULL, deck) & 0xff;
     state[4] = ((getter)ADDR_GET_TEMPO_X100)(NULL, deck);
     state[5] = ((getter)ADDR_GET_TOTAL_TIME)(NULL, deck);
     state[6] = ((uint32_t (*)(void *))ADDR_GET_SYNC_MASTER)(NULL);
     state[7] = rings[deck].dropped;
     state[8] = 0;
     void *mixer = *(void **)ADDR_MIXER_SINGLETON;
     if (mixer) {
          typedef uint32_t (*mixer_getter)(void *, int);
          state[9] = ((mixer_getter)ADDR_MIXER_GET_FADER)(mixer, deck);
          state[10] = ((mixer_getter)ADDR_MIXER_GET_TRIM)(mixer, deck);
          state[11] = ((uint32_t (*)(void *, int, int))ADDR_MIXER_GET_EQ)(mixer, deck, 0);
          state[12] = ((uint32_t (*)(void *, int, int))ADDR_MIXER_GET_EQ)(mixer, deck, 1);
          state[13] = ((uint32_t (*)(void *, int, int))ADDR_MIXER_GET_EQ)(mixer, deck, 2);
          state[14] = ((mixer_getter)ADDR_MIXER_GET_COLOR)(mixer, deck);
          state[15] = ((uint32_t (*)(void *))ADDR_MIXER_GET_XFADER)(mixer);
          state[8] = ((mixer_getter)ADDR_MIXER_GET_COLOR_TYPE)(mixer, deck);
     } else {
          memset(state + 9, 0, 7 * sizeof(state[0]));
     }
     send_grid(deck, state[5]);
     struct packet_head h = { STEM_MAGIC, STEM_T_STATE, (uint32_t)deck,
                              sizeof(state), rings[deck].frames };
     write_all(&h, sizeof(h));
     write_all(state, sizeof(state));
}

static void *stem_thread(void *arg)
{
     (void)arg;
     unsigned idle_us = 0;
     int recording = 0;
     for (;;) {
          int moved = 0;
          int on = rbp_stems_get();
          if (on && !recording) {
               struct packet_head h = { STEM_MAGIC, STEM_T_START, 0, 0, rings[0].frames };
               memset(sent, 0, sizeof(sent));
               memset(sent_count, 0, sizeof(sent_count));
               memset(sent_total, 0, sizeof(sent_total));
               memset(seen_count, 0, sizeof(seen_count));
               memset(seen_polls, 0, sizeof(seen_polls));
               rec_started_at = rings[0].frames;
               write_all(&h, sizeof(h));
               recording = 1;
          }
          for (int deck = 0; deck < 3; deck++) {
               struct ring *r = &rings[deck];
               while (r->tail != r->head) {
                    barrier();
                    struct slot *s = &r->slots[r->tail % RING_SLOTS];
                    struct packet_head h = { STEM_MAGIC, deck == MASTER_RING ? STEM_T_MASTER : STEM_T_AUDIO,
                                             (uint32_t)deck, s->frames, s->start };
                    if (write_all(&h, sizeof(h)) ||
                        write_all(s->post, s->frames * 8) ||
                        (deck != MASTER_RING && write_all(s->clean, s->frames * 8))) {
                         klog("knobshim2: stems: write failed, recording stopped\n");
                         return NULL;
                    }
                    barrier();
                    r->tail++;
                    moved = 1;
               }
          }
          if (!on && recording && !moved) {
               struct packet_head h = { STEM_MAGIC, STEM_T_STOP, 0, 0, rings[0].frames };
               write_all(&h, sizeof(h));
               recording = 0;
          }
          if (!recording) {
               usleep(20000);
               continue;
          }
          if (!moved) {
               usleep(2000);
               idle_us += 2000;
          }
          if (idle_us >= STATE_EVERY_US || moved) {
               static uint64_t last;
               uint64_t now = rings[0].frames;
               if (idle_us >= STATE_EVERY_US || now - last >= 882) {
                    last = now;
                    idle_us = 0;
                    if (*(void **)ADDR_PLAYENGINE_SINGLETON) {
                         send_state(0);
                         send_state(1);
                    }
               }
          }
     }
     return NULL;
}

void stems_start(void)
{
     uint32_t *p = (uint32_t *)ADDR_MIXERCHANNEL_UPDATE;
     const size_t pgsz = 4096;
     char path[64];
     const char *midi = env_text("MIDI_RAW_DEV", NULL);

     if (!is_rbp_process() || !midi)
          return;
     /* /dev/vport1p1 is MIDI (nr=1); the recorder's port is nr=2 on the same bus. */
     size_t len = strlen(midi);
     if (len < 2 || len >= sizeof(path) || midi[len - 1] != '1')
          return;
     memcpy(path, midi, len + 1);
     path[len - 1] = '2';
     stem_fd = open(path, O_WRONLY | O_CLOEXEC);
     if (stem_fd < 0)
          return;                                /* normal: recording is off */
     if (p[0] != PROLOGUE_UPDATE_0 || p[1] != PROLOGUE_UPDATE_1) {
          klog("knobshim2: stems: unexpected prologue %08x %08x\n", p[0], p[1]);
          return;
     }
     for (int deck = 0; deck < 3; deck++) {
          rings[deck].slots = mmap(NULL, sizeof(struct slot) * RING_SLOTS,
                                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
          if (rings[deck].slots == MAP_FAILED) {
               klog("knobshim2: stems: mmap failed\n");
               return;
          }
     }
     uint32_t *tramp = mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
     if (tramp == MAP_FAILED)
          return;
     tramp[0] = p[0];                            /* displaced, position independent */
     tramp[1] = p[1];
     tramp[2] = 0xE51FF004u;                     /* ldr pc,[pc,#-4] */
     tramp[3] = (uint32_t)(p + 2);
     __builtin___clear_cache((char *)tramp, (char *)(tramp + 4));
     orig_update = (void (*)(void *, int))tramp;

     pthread_t thread;
     if (pthread_create(&thread, NULL, stem_thread, NULL))
          return;
     pthread_detach(thread);

     unsigned long pg = (unsigned long)p & ~(unsigned long)(pgsz - 1);
     if (mprotect((void *)pg, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
          klog("knobshim2: stems: mprotect failed\n");
          return;
     }
     p[1] = (uint32_t)&stem_update_hook;
     barrier();
     p[0] = 0xE51FF004u;                         /* ldr pc,[pc,#-4] */
     mprotect((void *)pg, pgsz, PROT_READ | PROT_EXEC);
     __builtin___clear_cache((char *)p, (char *)(p + 2));

     /* The master, the same way: its first two instructions run from a trampoline. */
     uint32_t *master = (uint32_t *)ADDR_MASTER_UPDATE;
     if (master[0] == PROLOGUE_MASTER_0 && master[1] == PROLOGUE_MASTER_1) {
          tramp[4] = master[0];
          tramp[5] = master[1];
          tramp[6] = 0xE51FF004u;
          tramp[7] = (uint32_t)(master + 2);
          __builtin___clear_cache((char *)(tramp + 4), (char *)(tramp + 8));
          orig_master = (void (*)(void *, int))(tramp + 4);
          pg = (unsigned long)master & ~(unsigned long)(pgsz - 1);
          if (mprotect((void *)pg, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
               master[1] = (uint32_t)&master_update_hook;
               barrier();
               master[0] = 0xE51FF004u;
               mprotect((void *)pg, pgsz, PROT_READ | PROT_EXEC);
               __builtin___clear_cache((char *)master, (char *)(master + 2));
          }
     } else {
          klog("knobshim2: stems: master output not hooked\n");
     }

     /* Whole functions replaced: both are four instructions and need nothing kept. */
     uint32_t *status = (uint32_t *)ADDR_MIXER_REC_STATUS, *elapsed = (uint32_t *)ADDR_MIXER_REC_ELAPSED;
     if (status[0] == PROLOGUE_REC_0 && status[1] == PROLOGUE_REC_1 &&
         elapsed[0] == PROLOGUE_REC_0 && elapsed[1] == PROLOGUE_REC_1) {
          pg = (unsigned long)status & ~(unsigned long)(pgsz - 1);
          if (mprotect((void *)pg, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
               status[1] = (uint32_t)&rec_status_hook;
               elapsed[1] = (uint32_t)&rec_elapsed_hook;
               barrier();
               status[0] = 0xE51FF004u;
               elapsed[0] = 0xE51FF004u;
               mprotect((void *)pg, pgsz, PROT_READ | PROT_EXEC);
               __builtin___clear_cache((char *)status, (char *)(elapsed + 2));
          }
     } else {
          klog("knobshim2: stems: firmware record display not hooked\n");
     }
     klog("knobshim2: stems: recording on %s\n", path);
}
