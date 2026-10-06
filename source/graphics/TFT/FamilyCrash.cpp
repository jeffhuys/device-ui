#if defined(FAMILY_UI) && defined(VIEW_320x240) && defined(ARDUINO_ARCH_ESP32)

// Crash notes that survive a reset, for the T-Decks' watchdog restarts (docs/family-ui.md).
//
// A panic prints on UART0, which is not wired to USB, and the interrupt watchdog resets left no core
// dump. So this keeps two things in RTC memory, which a watchdog or software reset leaves alone:
// - from the panic handler (wrapped with -Wl,--wrap=esp_panic_handler): the reason, the core, and the
//   call chain of both cores, as raw program counters for addr2line against the flashed build's ELF;
//   Only the first entry counts: the handler goes on to write a core dump, which can panic again, and
//   that nested entry would describe the core dump code instead (seen on the first test crash);
// - breadcrumbs, refreshed all the time: how many ticks each core has taken (its own count; FreeRTOS's
//   tick count stops for both when core 0 stops) and the task it was running at the last one,
//   the family screen's last 1 s tick and input read, and since when the trackball was held. If the
//   panic handler never ran (a core stuck on the bus), these still say which core stopped and when.
// familyCrashReport() logs it all at the next boot, appends it to /family_crash.bin (the last
// c_keep restarts, so the notes outlive a boot nobody watched), and clears the RTC copy.
// familyCrashRecord() hands a stored record to the radio side (FamilyScreen, port c_crashPort), so
// the health monitor can fetch it over the air. Record layout: docs/family-ui.md, "Crash notes".

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_freertos_hooks.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "util/ILog.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "xtensa/xtensa_context.h"
#include <Arduino.h>
#include <FS.h>
#include <cstring>

extern fs::FS &persistentFS; // ViewController.cpp

namespace {
constexpr uint32_t c_magic = 0x564f584c; // "VOXL"
constexpr int c_depth = 16, c_name = 12;

struct Core {
    uint32_t pc[c_depth];
    uint32_t sp, depth, valid;
};

struct Notes {
    uint32_t magic;
    // panic
    uint32_t panicked, nested, core, exception, pseudo, exccause, excvaddr;
    char reason[40];
    Core cores[2];
    // breadcrumbs
    uint32_t tick[2];
    char task[2][c_name];
    uint32_t screenTickMs, inputReadMs, ballDownMs;
};

RTC_NOINIT_ATTR Notes notes;
bool hooked = false;

constexpr const char *c_file = "/family_crash.bin";
constexpr int c_keep = 8;         // records kept in the file
constexpr size_t c_maxRecord = 200; // fits one PKI-encrypted packet with room to spare
constexpr uint8_t c_version = 2; // 2: per-core tick counts instead of the shared tick count, nested panics

void put8(uint8_t *&p, uint32_t v)
{
    *p++ = (uint8_t)v;
}

void put32(uint8_t *&p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        *p++ = (uint8_t)(v >> (8 * i)); // little endian
}

uint32_t buildId(void)
{
    // the first four bytes of the SHA-256 that esptool appends to the app image: the same bytes end the .bin
    uint8_t sha[32] = {0};
    const esp_partition_t *app = esp_ota_get_running_partition();
    if (!app || esp_partition_get_sha256(app, sha) != ESP_OK)
        return 0;
    return (uint32_t)sha[0] << 24 | (uint32_t)sha[1] << 16 | (uint32_t)sha[2] << 8 | sha[3];
}

// one record of the previous run, as stored in the file and sent on the air
size_t pack(uint8_t *buf, uint32_t seq, int reason)
{
    uint8_t *p = buf;
    put8(p, c_version);
    put32(p, seq);
    put32(p, buildId());
    put8(p, reason);
    put8(p, (notes.panicked ? 1 : 0) | (notes.ballDownMs ? 2 : 0) | (notes.pseudo ? 4 : 0) | (notes.nested ? 8 : 0));
    put8(p, notes.core);
    put8(p, notes.exception);
    put8(p, notes.exccause);
    put32(p, notes.excvaddr);
    put32(p, notes.screenTickMs);
    put32(p, notes.inputReadMs);
    put32(p, notes.ballDownMs);
    for (int cpu = 0; cpu < 2; cpu++) {
        put32(p, notes.tick[cpu]);
        for (int i = 0; i < c_name - 1; i++) // the name, zero padded, without its terminator
            put8(p, notes.task[cpu][i]);
    }
    // the call chains last, cut to what fits
    for (int core = 0; core < 2; core++) {
        const Core &c = notes.cores[core];
        uint32_t depth = notes.panicked && c.valid && c.depth <= c_depth ? c.depth : 0;
        size_t room = (c_maxRecord - (p - buf) - 1 - (core == 0 ? 1 : 0)) / 4;
        if (depth > room)
            depth = room;
        put8(p, depth);
        for (uint32_t k = 0; k < depth; k++)
            put32(p, c.pc[k]);
    }
    return p - buf;
}

// the file: records one after another, each as [length byte][record]
int readAll(uint8_t recs[][c_maxRecord], uint8_t *lens)
{
    int n = 0;
    File f = persistentFS.open(c_file, FILE_READ);
    if (!f)
        return 0;
    while (n < c_keep + 1 && f.available()) {
        int len = f.read();
        if (len <= 0 || len > (int)c_maxRecord || f.read(recs[n], len) != (size_t)len)
            break;
        lens[n++] = len;
    }
    f.close();
    return n;
}

uint32_t seqOf(const uint8_t *rec)
{
    return rec[1] | rec[2] << 8 | rec[3] << 16 | (uint32_t)rec[4] << 24;
}

void save(int reason)
{
    static uint8_t recs[c_keep + 1][c_maxRecord];
    static uint8_t lens[c_keep + 1];
    int n = readAll(recs, lens);
    uint32_t seq = n ? seqOf(recs[n - 1]) + 1 : 1;
    lens[n] = pack(recs[n], seq, reason);
    n++;
    int first = n > c_keep ? n - c_keep : 0;
    File f = persistentFS.open(c_file, FILE_WRITE);
    if (!f) {
        ILOG_ERROR("family: cannot write %s", c_file);
        return;
    }
    for (int i = first; i < n; i++) {
        f.write(lens[i]);
        f.write(recs[i], lens[i]);
    }
    f.close();
    ILOG_INFO("family: crash notes saved as record %u (%u bytes)", (unsigned)seq, (unsigned)lens[n - 1]);
}

void IRAM_ATTR copyName(char *to, const char *from)
{
    int i = 0;
    for (; from && from[i] && i < c_name - 1; i++)
        to[i] = from[i];
    to[i] = 0;
}

void IRAM_ATTR tickCore(int cpu)
{
    notes.tick[cpu]++;
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCPU(cpu);
    copyName(notes.task[cpu], t ? pcTaskGetName(t) : "-");
}

void IRAM_ATTR tick0(void)
{
    tickCore(0);
}

void IRAM_ATTR tick1(void)
{
    tickCore(1);
}

void IRAM_ATTR walk(Core &c, const void *frame)
{
    const XtExcFrame *f = (const XtExcFrame *)frame;
    c.valid = 0;
    c.depth = 0;
    if (!f)
        return;
    esp_backtrace_frame_t bt = {.pc = (uint32_t)f->pc, .sp = (uint32_t)f->a1, .next_pc = (uint32_t)f->a0, .exc_frame = f};
    c.sp = bt.sp;
    c.pc[c.depth++] = bt.pc;
    while (c.depth < c_depth && bt.next_pc && esp_backtrace_get_next_frame(&bt))
        c.pc[c.depth++] = bt.pc;
    c.valid = 1;
}
} // namespace

extern "C" void __real_esp_panic_handler(panic_info_t *info);

extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (notes.magic == c_magic && notes.panicked) {
        notes.nested++;
        __real_esp_panic_handler(info);
        return;
    }
    notes.magic = c_magic;
    notes.panicked = 1;
    notes.core = info->core;
    notes.exception = info->exception;
    notes.pseudo = info->pseudo_excause;
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    notes.exccause = f ? f->exccause : 0;
    notes.excvaddr = f ? f->excvaddr : 0;
    int i = 0;
    for (; info->reason && info->reason[i] && i < (int)sizeof(notes.reason) - 1; i++)
        notes.reason[i] = info->reason[i];
    notes.reason[i] = 0;
    for (int core = 0; core < 2; core++)
        walk(notes.cores[core], core == info->core ? info->frame : g_exc_frames[core]);
    __real_esp_panic_handler(info);
}

void familyCrumb(int what, uint32_t value)
{
    if (what == 0)
        notes.screenTickMs = value;
    else if (what == 1)
        notes.inputReadMs = value;
    else
        notes.ballDownMs = value;
}

void familyCrashReport(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT && notes.magic == c_magic) {
        ILOG_WARN("family: before this reset (reason %d): screen tick at %u ms, input read at %u ms, trackball %s%u ms",
                  (int)reason, (unsigned)notes.screenTickMs, (unsigned)notes.inputReadMs,
                  notes.ballDownMs ? "held since " : "not held", (unsigned)notes.ballDownMs);
        for (int cpu = 0; cpu < 2; cpu++)
            ILOG_WARN("family: before this reset: core %d took %u ticks, the last one in %s", cpu, (unsigned)notes.tick[cpu],
                      notes.task[cpu]);
        if (notes.panicked) {
            ILOG_WARN("family: panic on core %u: %s (exception %u%s, EXCCAUSE %u, EXCVADDR 0x%08x), %u nested", (unsigned)notes.core,
                      notes.reason, (unsigned)notes.exception, notes.pseudo ? ", pseudo" : "", (unsigned)notes.exccause,
                      (unsigned)notes.excvaddr, (unsigned)notes.nested);
            for (int core = 0; core < 2; core++) {
                const Core &c = notes.cores[core];
                if (!c.valid || c.depth > c_depth) {
                    ILOG_WARN("family: backtrace core %d: none", core);
                    continue;
                }
                char line[16 * 12 + 1] = "";
                for (uint32_t k = 0; k < c.depth; k++)
                    snprintf(line + strlen(line), sizeof(line) - strlen(line), " 0x%08x", (unsigned)c.pc[k]);
                ILOG_WARN("family: backtrace core %d (sp 0x%08x):%s", core, (unsigned)c.sp, line);
            }
        } else {
            ILOG_WARN("family: the panic handler did not run before this reset");
        }
    }
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
        if (notes.magic != c_magic) // nothing kept (should not happen after a reset): store the reason alone
            memset(&notes, 0, sizeof(notes));
        save((int)reason);
    }
    memset(&notes, 0, sizeof(notes));
    notes.magic = c_magic;
    if (!hooked) {
        hooked = true;
        esp_register_freertos_tick_hook_for_cpu(tick0, 0);
        esp_register_freertos_tick_hook_for_cpu(tick1, 1);
    }
}

size_t familyCrashRecord(uint32_t after, uint8_t *out, size_t max, uint8_t *remaining)
{
    // the oldest stored record with a sequence number above `after`; *remaining says how many more follow
    static uint8_t recs[c_keep + 1][c_maxRecord];
    static uint8_t lens[c_keep + 1];
    int n = readAll(recs, lens);
    *remaining = 0;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        if (seqOf(recs[i]) <= after)
            continue;
        if (!len && lens[i] <= max) {
            memcpy(out, recs[i], lens[i]);
            len = lens[i];
        } else {
            (*remaining)++;
        }
    }
    return len;
}

#endif
