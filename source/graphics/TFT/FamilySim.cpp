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

#include "graphics/view/TFT/FamilyScreen.h"
#include "graphics/view/TFT/TFTView_320x240.h"
#include "ui.h"
#include "util/ILog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *simPath = nullptr;

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

void FamilyScreen::startSim(void)
{
    simPath = getenv("FAMILY_SIM_CMDS");
    if (!simPath || !*simPath)
        return;
    ILOG_INFO("family sim: reading commands from %s", simPath);
    lv_timer_create(timer_sim, 100, nullptr);
}

#endif
