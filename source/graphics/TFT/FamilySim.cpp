#if defined(FAMILY_UI) && defined(VIEW_320x240) && defined(ARCH_PORTDUINO)

// Desktop build only: drive the family screen and MUI from a command file, so a test run needs no
// real keystrokes or mouse moves on the developer's desktop. Set FAMILY_SIM_CMDS to a file path;
// every 100 ms its lines are executed and the file is emptied. One command per line:
//   next | prev          move the focus in the group that has the keyboard
//   click                click the focused object
//   key <code>           send a key code (8 = Backspace, 27 = Esc, 10 = Enter)
//   type <text>          send the text, one key per character
//   chord                the dev chord: family screen <-> MUI
//   tap <panel>          click a MUI navigation button: home nodes groups messages map settings
//   open <name>          click a MUI object: chat0 (first chat), user, region, role, timeout, reboot
//   touch <x> <y>        a finger tap at screen pixel x, y, through a pointer input of its own (as the
//                        T-Deck's touch screen: LVGL's press, release and click, and the indev type)
//   swipe <x> <y> <dy>   press at x, y, move dy pixels (negative: up) in steps, release: a scroll
// Set FAMILY_SIM_FRAMES to a directory to record the boot animation: every refresh from its start
// until 600 ms after its end is written there as a PPM, named by frame number and milliseconds.

#include "graphics/view/TFT/FamilyScreen.h"
#include "graphics/view/TFT/TFTView_320x240.h"
#include "ui.h"
#include "util/ILog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *simPath = nullptr;
static const char *framesDir = nullptr;
static uint8_t frame[320 * 240 * 3]; // what the panel shows, RGB
static uint32_t frameNo = 0;

// the pretend touch screen: a list of (x, y, pressed) samples, one taken per read
struct TouchSample {
    int16_t x, y;
    bool pressed;
};
static TouchSample touchQueue[64];
static int touchHead = 0, touchTail = 0;
static TouchSample touchLast = {0, 0, false};

static void touchPush(int x, int y, bool pressed)
{
    int next = (touchTail + 1) % 64;
    if (next == touchHead)
        return;
    touchQueue[touchTail] = {(int16_t)x, (int16_t)y, pressed};
    touchTail = next;
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data)
{
    if (touchHead != touchTail) {
        touchLast = touchQueue[touchHead];
        touchHead = (touchHead + 1) % 64;
    }
    data->point.x = touchLast.x;
    data->point.y = touchLast.y;
    data->state = touchLast.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_group_t *keyboardGroup(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD && lv_indev_get_group(indev))
            return lv_indev_get_group(indev);
    }
    return lv_group_get_default();
}

void FamilyScreen::simCommand(const char *line)
{
    lv_group_t *g = keyboardGroup();
    if (strcmp(line, "next") == 0) {
        lv_group_focus_next(g);
    } else if (strcmp(line, "prev") == 0) {
        lv_group_focus_prev(g);
    } else if (strcmp(line, "click") == 0) {
        if (lv_obj_t *obj = lv_group_get_focused(g))
            lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    } else if (strncmp(line, "key ", 4) == 0) {
        lv_group_send_data(g, (uint32_t)atoi(line + 4));
    } else if (strncmp(line, "type ", 5) == 0) {
        for (const char *c = line + 5; *c; c++)
            lv_group_send_data(g, (uint8_t)*c);
    } else if (strcmp(line, "chord") == 0) {
        async_toggle_dev_mode(nullptr);
    } else if (strncmp(line, "tap ", 4) == 0) {
        struct {
            const char *name;
            lv_obj_t *button;
        } nav[] = {{"home", objects.home_button}, {"nodes", objects.nodes_button}, {"groups", objects.groups_button},
                   {"messages", objects.messages_button}, {"map", objects.map_button}, {"settings", objects.settings_button}};
        for (auto &n : nav) {
            if (strcmp(line + 4, n.name) == 0)
                lv_obj_send_event(n.button, LV_EVENT_CLICKED, nullptr);
        }
    } else if (strncmp(line, "open ", 5) == 0) {
        const char *name = line + 5;
        lv_obj_t *obj = nullptr;
        if (strcmp(name, "chat0") == 0 && lv_obj_get_child_count(objects.chats_panel) > 0)
            obj = lv_obj_get_child(objects.chats_panel, 0);
        else if (strcmp(name, "user") == 0)
            obj = objects.basic_settings_user_button;
        else if (strcmp(name, "region") == 0)
            obj = objects.basic_settings_region_button;
        else if (strcmp(name, "role") == 0)
            obj = objects.basic_settings_role_button;
        else if (strcmp(name, "timeout") == 0)
            obj = objects.basic_settings_timeout_button;
        else if (strcmp(name, "reboot") == 0)
            obj = objects.basic_settings_reboot_button;
        if (obj)
            lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        else
            ILOG_WARN("family sim: nothing to open for '%s'", name);
    } else if (strncmp(line, "touch ", 6) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
            for (int i = 0; i < 3; i++)
                touchPush(x, y, true);
            touchPush(x, y, false);
        }
    } else if (strncmp(line, "swipe ", 6) == 0) {
        int x = 0, y = 0, dy = 0;
        if (sscanf(line + 6, "%d %d %d", &x, &y, &dy) == 3) {
            touchPush(x, y, true);
            for (int i = 1; i <= 10; i++)
                touchPush(x, y + dy * i / 10, true);
            touchPush(x, y + dy, false);
        }
    } else if (*line) {
        ILOG_WARN("family sim: unknown command '%s'", line);
    }
}

static void timer_sim(lv_timer_t *)
{
    FILE *f = fopen(simPath, "r");
    if (!f)
        return;
    char line[256];
    bool any = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        FamilyScreen::simCommandStatic(line);
        any = true;
    }
    fclose(f);
    if (any) {
        f = fopen(simPath, "w");
        if (f)
            fclose(f);
    }
}

void FamilyScreen::simCommandStatic(const char *line)
{
    if (family)
        family->simCommand(line);
}

void FamilyScreen::simFrame(lv_display_t *disp, const lv_area_t *area, const uint8_t *px_map)
{
    if (!framesDir || !family || !family->bootStartTick)
        return;
    if (!family->booting && lv_tick_elaps(family->bootEndTick) > 600)
        return;
    lv_color_format_t cf = lv_display_get_color_format(disp);
    uint32_t size = lv_color_format_get_size(cf);
    const uint8_t *p = px_map;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++, p += size) {
            if (x < 0 || y < 0 || x >= 320 || y >= 240)
                continue;
            uint8_t *o = &frame[(y * 320 + x) * 3];
            if (size == 2) {
                uint16_t c = cf == LV_COLOR_FORMAT_RGB565_SWAPPED ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
                o[0] = ((c >> 11) & 0x1f) << 3;
                o[1] = ((c >> 5) & 0x3f) << 2;
                o[2] = (c & 0x1f) << 3;
            } else {
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
            }
        }
    }
    if (!lv_display_flush_is_last(disp))
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/f%04u_%05u.ppm", framesDir, (unsigned)frameNo++, (unsigned)lv_tick_elaps(family->bootStartTick));
    if (FILE *f = fopen(path, "wb")) {
        fprintf(f, "P6\n320 240\n255\n");
        fwrite(frame, 1, sizeof(frame), f);
        fclose(f);
    }
}

void FamilyScreen::startSim(void)
{
    framesDir = getenv("FAMILY_SIM_FRAMES");
    if (framesDir && !*framesDir)
        framesDir = nullptr;
    simPath = getenv("FAMILY_SIM_CMDS");
    if (!simPath || !*simPath)
        return;
    ILOG_INFO("family sim: reading commands from %s", simPath);
    lv_timer_create(timer_sim, 100, nullptr);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
}

#endif
