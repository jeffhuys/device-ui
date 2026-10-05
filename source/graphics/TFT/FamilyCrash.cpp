#if defined(FAMILY_UI) && defined(VIEW_320x240) && defined(ARDUINO_ARCH_ESP32)

// Crash notes that survive a reset, for the T-Decks' watchdog restarts (docs/family-ui.md).
//
// A panic prints on UART0, which is not wired to USB, and the interrupt watchdog resets left no core
// dump. So this keeps two things in RTC memory, which a watchdog or software reset leaves alone:
// - from the panic handler (wrapped with -Wl,--wrap=esp_panic_handler): the reason, the core, and the
//   call chain of both cores, as raw program counters for addr2line against the flashed build's ELF;
// - breadcrumbs, refreshed all the time: each core's last FreeRTOS tick with the task it was running,
//   the family screen's last 1 s tick and input read, and since when the trackball was held. If the
//   panic handler never ran (a core stuck on the bus), these still say which core stopped and when.
// familyCrashReport() logs it all at the next boot and clears it.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_freertos_hooks.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "util/ILog.h"
#include "xtensa/xtensa_context.h"
#include <Arduino.h>
#include <cstring>

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
    uint32_t panicked, core, exception, pseudo, exccause, excvaddr;
    char reason[40];
    Core cores[2];
    // breadcrumbs
    uint32_t tick[2];
    char task[2][c_name];
    uint32_t screenTickMs, inputReadMs, ballDownMs;
};

RTC_NOINIT_ATTR Notes notes;
bool hooked = false;

void IRAM_ATTR copyName(char *to, const char *from)
{
    int i = 0;
    for (; from && from[i] && i < c_name - 1; i++)
        to[i] = from[i];
    to[i] = 0;
}

void IRAM_ATTR tickCore(int cpu)
{
    notes.tick[cpu] = xTaskGetTickCountFromISR();
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
            ILOG_WARN("family: before this reset: core %d last tick %u, running %s", cpu, (unsigned)notes.tick[cpu],
                      notes.task[cpu]);
        if (notes.panicked) {
            ILOG_WARN("family: panic on core %u: %s (exception %u%s, EXCCAUSE %u, EXCVADDR 0x%08x)", (unsigned)notes.core,
                      notes.reason, (unsigned)notes.exception, notes.pseudo ? ", pseudo" : "", (unsigned)notes.exccause,
                      (unsigned)notes.excvaddr);
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
    memset(&notes, 0, sizeof(notes));
    notes.magic = c_magic;
    if (!hooked) {
        hooked = true;
        esp_register_freertos_tick_hook_for_cpu(tick0, 0);
        esp_register_freertos_tick_hook_for_cpu(tick1, 1);
    }
}

#endif
