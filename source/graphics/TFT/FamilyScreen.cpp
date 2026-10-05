#if defined(FAMILY_UI) && defined(VIEW_320x240)

#include "graphics/view/TFT/FamilyScreen.h"
// the filesystem headers go before lv_i18n.h, whose _p() macro clashes with portduino FS.h
#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
#else
#include "LittleFS.h"
#endif
#include "Arduino.h"
#include "graphics/view/TFT/TFTView_320x240.h"
#include "family_strings.h"
#include "lv_i18n.h"
#include "lvgl_private.h"
#include "ui.h"
#include "util/ILog.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <time.h>

extern fs::FS &persistentFS; // ViewController.cpp, the filesystem that holds /messages

LV_FONT_DECLARE(family_font_28);

#define VALID_TIME(T) (T > 1000000 && T < UINT32_MAX)
#define LV_COLOR_HEX(C)                                                                                                          \
    {                                                                                                                            \
        .blue = (C >> 0) & 0xff, .green = (C >> 8) & 0xff, .red = (C >> 16) & 0xff                                               \
    }

constexpr const char *c_markerFile = "/family_read.bin"; // last-read marker, next to /messages
constexpr uint32_t c_markerMagic = 0x464d5231;           // "FMR1"
constexpr uint32_t c_chordHoldMs = 1000;                 // trackball held before P counts
#ifndef FAMILY_DEV_IDLE_MS
#define FAMILY_DEV_IDLE_MS (10 * 60 * 1000) // dev mode auto-exit; override only for a desktop test
#endif
constexpr uint32_t c_devModeIdleMs = FAMILY_DEV_IDLE_MS;
constexpr uint32_t c_nodeLabelIdx = 2;                   // long name label in a MUI node panel (see addNode)

constexpr lv_color_t colorBackground = LV_COLOR_HEX(0x000000);
constexpr lv_color_t colorRow = LV_COLOR_HEX(0x202020);
constexpr lv_color_t colorRowFocused = LV_COLOR_HEX(0x383838);
constexpr lv_color_t colorOwnCard = LV_COLOR_HEX(0x123a5a);
constexpr lv_color_t colorFocus = LV_COLOR_HEX(0xffd400);
constexpr lv_color_t colorText = LV_COLOR_HEX(0xffffff);
constexpr lv_color_t colorDimText = LV_COLOR_HEX(0x8c8c8c);
constexpr lv_color_t colorSender = LV_COLOR_HEX(0x9fd3ff);
constexpr lv_color_t colorOwnSender = LV_COLOR_HEX(0xa8f0a8);
constexpr lv_color_t colorFailed = LV_COLOR_HEX(0xff6b6b);

FamilyScreen *FamilyScreen::family = nullptr;

// keyboard indevs whose read callback is wrapped for the dev chord
static constexpr int c_maxKeyboards = 4;
static lv_indev_t *wrappedIndev[c_maxKeyboards] = {};
static lv_indev_read_cb_t wrappedRead[c_maxKeyboards] = {};
static bool chordLatched = false; // swallow the chord key until it is released
#if !defined(INPUTDRIVER_ENCODER_BTN) && defined(ARCH_PORTDUINO)
static uint32_t escPressedAt = 0; // desktop stand-in for the held trackball: Esc, then P
#endif

static lv_style_t styleRow, styleRowFocused, styleRowDisabled, styleCard, styleOwnCard, styleCardFocused;

static uint32_t uptimeSeconds(void)
{
    return millis() / 1000 + 1; // never 0, so 0 can mean "unknown"
}

static uint32_t fnv1a(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static size_t utf8Length(uint32_t letter)
{
    return letter < 0x80 ? 1 : letter < 0x800 ? 2 : letter < 0x10000 ? 3 : 4;
}

FamilyScreen::FamilyScreen(TFTView_320x240 *v) : view(v) {}

// ===== hooks =====

void FamilyScreen::attach(TFTView_320x240 *view)
{
    if (family)
        return;
    family = new FamilyScreen(view);
    family->loadMarker();
    family->build();
    family->hookInput();
    family->mainScreenActive = lv_screen_active() == objects.main_screen;
    lv_obj_add_event_cb(objects.main_screen, ui_event_screen, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_add_event_cb(objects.main_screen, ui_event_screen, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_timer_create(timer_tick, 1000, nullptr);
    ILOG_INFO("family screen attached (channel %d)", FAMILY_CHANNEL);
}

void FamilyScreen::newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t msgTime, bool restore)
{
    if (!family || restore || !family->isFamily(from, to, ch))
        return;
    FamilyScreen &f = *family;
    bool full = f.count == c_maxEntries; // the oldest message drops out and every index moves down by one
    f.add(from, to, ch, msgTime, msg, strnlen(msg, messagePayloadSize), false, true, 0);

    if (f.shown && f.page == eRead && f.mainScreenActive) {
        // the thread is on screen: follow it if the newest card was focused
        lv_obj_t *focused = lv_group_get_focused(f.group);
        bool atEnd = !focused || focused == lv_obj_get_child(f.readList, -1);
        int index = focused ? (int)(intptr_t)focused->user_data - (full ? 1 : 0) : f.count - 1;
        f.markRead();
        f.fillRead(atEnd ? f.count - 1 : index);
    } else {
        f.unread++;
        f.refreshHome(true);
    }
    if (f.shown)
        f.wake();
}

void FamilyScreen::restoreMessage(const LogMessage &msg)
{
    if (!family)
        return;
    FamilyScreen &f = *family;
    if (msg.trashFlag) {
        f.removeChat(msg.from, msg.to, msg.ch);
        return;
    }
    if (!f.isFamily(msg.from, msg.to, msg.ch))
        return;

    bool outgoing = msg.from == f.view->ownNode;
    f.add(msg.from, msg.to, msg.ch, (uint32_t)msg.time, (const char *)msg.bytes, strnlen((const char *)msg.bytes, msg.length()),
          outgoing, false, 0);
    // everything up to the marker has been read, and so has everything before an own message
    if (outgoing || f.matches(f.at(f.count - 1)))
        f.unread = 0;
    else
        f.unread++;
}

void FamilyScreen::sentMessage(uint32_t to, uint8_t ch, uint32_t msgTime, uint32_t requestId, const char *msg)
{
    if (!family || to != UINT32_MAX || ch != FAMILY_CHANNEL)
        return;
    FamilyScreen &f = *family;
    f.add(f.view->ownNode, to, ch, msgTime, msg, strnlen(msg, messagePayloadSize), true, true, requestId);
    f.markRead();
    if (f.shown && f.page == eRead)
        f.fillRead(f.count - 1);
}

void FamilyScreen::textMessageResponse(uint32_t channelOrNode, uint32_t id, bool ack, bool err)
{
    if (!family || channelOrNode != FAMILY_CHANNEL || !err)
        return;
    FamilyScreen &f = *family;
    for (int i = f.count - 1; i >= 0; i--) {
        Entry &e = f.at(i);
        if (e.outgoing && e.requestId == id) {
            e.failed = true;
            if (f.page == eRead)
                f.refreshRead();
            break;
        }
    }
}

bool FamilyScreen::hidesMessagePopup(void)
{
    if (!family || !family->shown)
        return false;
    // MUI's own wake for a popup; family messages already woke the screen in newMessage()
    if (family->view->db.module_config.external_notification.alert_message)
        lv_disp_trig_activity(NULL);
    return true;
}

void FamilyScreen::alert(const char *text, bool show)
{
    if (!family || !family->alertLabel)
        return;
    FamilyScreen &f = *family;
    if (!show || !text) {
        lv_label_set_text(f.alertLabel, "");
        f.alertHideAt = 0;
        return;
    }
    const char *shown = text;
    f.alertHideAt = 0;
    if (strcmp(text, _("Disconnected!")) == 0) {
        shown = FAMILY_STR_ALERT_DISCONNECTED;
    } else if (strcmp(text, _("Connected!")) == 0) {
        shown = FAMILY_STR_ALERT_CONNECTED;
        f.alertHideAt = millis() + 5000;
    } else if (strcmp(text, _("Rebooting ...")) == 0) {
        shown = FAMILY_STR_ALERT_REBOOTING;
    } else if (strcmp(text, _("Resync ...")) == 0) {
        shown = FAMILY_STR_ALERT_RESYNC;
    } else if (strcmp(text, _("Shutting down ...")) == 0) {
        shown = FAMILY_STR_ALERT_SHUTDOWN;
    }
    lv_label_set_text(f.alertLabel, shown);
}

// ===== model =====

bool FamilyScreen::isFamily(uint32_t from, uint32_t to, uint8_t ch) const
{
    if (from == view->ownNode)
        return to == UINT32_MAX && ch == FAMILY_CHANNEL;
    if (to == UINT32_MAX)
        return ch == FAMILY_CHANNEL;
    return to == view->ownNode;
}

FamilyScreen::Entry &FamilyScreen::push(void)
{
    Entry *e;
    if (count < c_maxEntries) {
        e = &at(count);
        count++;
    } else {
        e = &entries[head];
        head = (head + 1) % c_maxEntries;
    }
    memset(e, 0, sizeof(Entry));
    return *e;
}

void FamilyScreen::add(uint32_t from, uint32_t to, uint8_t ch, uint32_t time, const char *text, size_t len, bool outgoing,
                       bool live, uint32_t requestId)
{
    Entry &e = push();
    e.from = from;
    e.to = to;
    e.ch = ch;
    e.time = time;
    e.uptime = live ? uptimeSeconds() : 0;
    e.requestId = requestId;
    e.outgoing = outgoing;
    if (len > messagePayloadSize)
        len = messagePayloadSize;
    memcpy(e.text, text, len);
    e.text[len] = '\0';
}

/**
 * Apply a trash record of the /messages log, the same way MUI's restoreMessage() erases a chat.
 */
void FamilyScreen::removeChat(uint32_t from, uint32_t to, uint8_t ch)
{
    auto erased = [&](const Entry &e) {
        if (from == view->ownNode && to == UINT32_MAX) // own channel chat deleted
            return ch == FAMILY_CHANNEL && e.to == UINT32_MAX;
        uint32_t node = from == view->ownNode ? to : from; // direct chat with that node deleted
        return e.to == view->ownNode && e.from == node;
    };
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!erased(at(i))) {
            if (kept != i)
                at(kept) = at(i);
            kept++;
        }
    }
    if (kept != count)
        ILOG_DEBUG("family: chat trash removed %d messages", count - kept);
    count = kept;
    if (unread > (uint32_t)count)
        unread = count;
}

FamilyScreen::Marker FamilyScreen::fingerprint(const Entry &e) const
{
    size_t len = strlen(e.text);
    return Marker{c_markerMagic, e.from, e.to, e.time, (uint32_t)len, fnv1a(e.text, len)};
}

bool FamilyScreen::matches(const Entry &e) const
{
    if (marker.magic != c_markerMagic)
        return false;
    Marker m = fingerprint(e);
    return m.from == marker.from && m.to == marker.to && m.time == marker.time && m.len == marker.len && m.hash == marker.hash;
}

/**
 * Everything up to the newest message has been seen. Persist that, so the count survives a reboot.
 */
void FamilyScreen::markRead(void)
{
    if (!restored || count == 0)
        return;
    unread = 0;
    Marker m = fingerprint(at(count - 1));
    if (memcmp(&m, &marker, sizeof(Marker)) != 0) {
        marker = m;
        saveMarker();
    }
    refreshHome(false);
}

void FamilyScreen::loadMarker(void)
{
    if (!persistentFS.exists(c_markerFile))
        return;
    File file = persistentFS.open(c_markerFile, FILE_READ);
    if (!file)
        return;
    Marker m{};
    if (file.read((uint8_t *)&m, sizeof(m)) == sizeof(m) && m.magic == c_markerMagic)
        marker = m;
    file.close();
}

void FamilyScreen::saveMarker(void)
{
    File file = persistentFS.open(c_markerFile, FILE_WRITE);
    if (!file) {
        ILOG_ERROR("family: cannot write %s", c_markerFile);
        return;
    }
    file.write((const uint8_t *)&marker, sizeof(marker));
    file.close();
}

// ===== ui =====

static lv_obj_t *createPlain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *createLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *createPage(lv_obj_t *parent)
{
    lv_obj_t *page = createPlain(parent);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(page, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

void FamilyScreen::build(void)
{
    lv_style_init(&styleRow);
    lv_style_set_bg_opa(&styleRow, LV_OPA_COVER);
    lv_style_set_bg_color(&styleRow, colorRow);
    lv_style_set_radius(&styleRow, 8);
    lv_style_set_border_width(&styleRow, 4);
    lv_style_set_border_color(&styleRow, colorRow);
    lv_style_set_pad_left(&styleRow, 14);
    lv_style_set_pad_right(&styleRow, 10);
    lv_style_set_text_color(&styleRow, colorText);

    lv_style_init(&styleRowFocused);
    lv_style_set_bg_color(&styleRowFocused, colorRowFocused);
    lv_style_set_border_color(&styleRowFocused, colorFocus);

    lv_style_init(&styleRowDisabled);
    lv_style_set_text_color(&styleRowDisabled, colorDimText);

    lv_style_init(&styleCard);
    lv_style_set_bg_opa(&styleCard, LV_OPA_COVER);
    lv_style_set_bg_color(&styleCard, colorRow);
    lv_style_set_radius(&styleCard, 6);
    lv_style_set_border_width(&styleCard, 4);
    lv_style_set_border_color(&styleCard, colorRow);
    lv_style_set_pad_all(&styleCard, 6);
    lv_style_set_pad_row(&styleCard, 2);

    lv_style_init(&styleOwnCard);
    lv_style_set_bg_color(&styleOwnCard, colorOwnCard);
    lv_style_set_border_color(&styleOwnCard, colorOwnCard);

    lv_style_init(&styleCardFocused);
    lv_style_set_border_color(&styleCardFocused, colorFocus);

    group = lv_group_create();
    lv_group_set_wrap(group, false);

    root = createPlain(objects.main_screen);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(root, colorBackground, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(root, colorText, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE); // swallow pointer input meant for MUI underneath
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    buildHome();
    buildRead();
    buildSend();
}

void FamilyScreen::buildHome(void)
{
    homePage = createPage(root);

    // status strip: clock, MUI alert, battery
    lv_obj_t *strip = createPlain(homePage);
    lv_obj_set_size(strip, lv_pct(100), 20);
    lv_obj_set_style_pad_hor(strip, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    clockLabel = createLabel(strip, &ui_font_montserrat_16, colorDimText, "");
    lv_obj_set_align(clockLabel, LV_ALIGN_LEFT_MID);
    alertLabel = createLabel(strip, &ui_font_montserrat_16, colorFocus, "");
    lv_obj_set_align(alertLabel, LV_ALIGN_CENTER);
    batteryLabel = createLabel(strip, &ui_font_montserrat_16, colorDimText, "");
    lv_obj_set_align(batteryLabel, LV_ALIGN_RIGHT_MID);

    static const char *text[3] = {FAMILY_STR_MESSAGES_LOADING, FAMILY_STR_READ, FAMILY_STR_SEND};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = createPlain(homePage);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_add_style(row, &styleRow, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(row, &styleRowFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_style(row, &styleRowDisabled, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ui_event_row, LV_EVENT_ALL, (void *)(intptr_t)i);
        rows[i] = row;

        rowLabels[i] = lv_label_create(row);
        lv_obj_set_style_text_font(rowLabels[i], &family_font_28, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(rowLabels[i], text[i]);
        lv_obj_set_align(rowLabels[i], LV_ALIGN_LEFT_MID);
    }
    lv_obj_add_state(rows[0], LV_STATE_DISABLED);
}

void FamilyScreen::buildRead(void)
{
    readPage = createPage(root);
    lv_obj_t *hint = createLabel(readPage, &ui_font_montserrat_14, colorDimText, FAMILY_STR_READ_HINT);
    lv_obj_set_style_pad_left(hint, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    readList = createPlain(readPage);
    lv_obj_set_width(readList, lv_pct(100));
    lv_obj_set_flex_grow(readList, 1);
    lv_obj_set_flex_flow(readList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(readList, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(readList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(readList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(readList, LV_SCROLLBAR_MODE_ACTIVE);
}

void FamilyScreen::buildSend(void)
{
    sendPage = createPage(root);
    createLabel(sendPage, &ui_font_montserrat_16, colorSender, FAMILY_STR_SEND_TITLE);

    // the focusable box; the text area inside is fed by hand so LVGL's encoder edit mode never applies
    sendBox = createPlain(sendPage);
    lv_obj_set_width(sendBox, lv_pct(100));
    lv_obj_set_flex_grow(sendBox, 1);
    lv_obj_add_style(sendBox, &styleRow, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(sendBox, &styleRowFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(sendBox, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(sendBox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sendBox, ui_event_send_box, LV_EVENT_ALL, nullptr);

    textArea = lv_textarea_create(sendBox);
    lv_group_remove_obj(textArea); // textareas join the default (MUI) group on creation
    lv_obj_clear_flag(textArea, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(textArea); // no theme border or outline; the box around it shows the focus
    lv_obj_set_size(textArea, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(textArea, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(textArea, &ui_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(textArea, colorText, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(textArea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_color(textArea, colorFocus, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(textArea, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(textArea, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);

    lv_obj_t *bottom = createPlain(sendPage);
    lv_obj_set_size(bottom, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *hint = createLabel(bottom, &ui_font_montserrat_14, colorDimText, FAMILY_STR_SEND_HINT);
    lv_obj_set_align(hint, LV_ALIGN_LEFT_MID);
    sendCount = createLabel(bottom, &ui_font_montserrat_14, colorDimText, "");
    lv_obj_set_align(sendCount, LV_ALIGN_RIGHT_MID);
}

void FamilyScreen::show(void)
{
    ILOG_INFO("family screen shown");
    shown = true;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    goHome();
    refreshStrip();
    assignGroup();
}

void FamilyScreen::hide(void)
{
    ILOG_INFO("family screen hidden");
    shown = false;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    assignGroup();
}

void FamilyScreen::showPage(Page p)
{
    page = p;
    lv_obj_t *pages[3] = {homePage, readPage, sendPage};
    for (int i = 0; i < 3; i++) {
        if (i == p)
            lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (p != eSend) {
        lv_obj_remove_state(textArea, LV_STATE_FOCUSED);
        lv_obj_send_event(textArea, LV_EVENT_DEFOCUSED, nullptr);
    }
}

void FamilyScreen::goHome(void)
{
    showPage(eHome);
    refreshHome(true);
}

/**
 * Update the home rows. Row 1 is only focusable while there is something unread.
 * @param resetFocus put focus on row 1 if there is something unread, else on row 2
 */
void FamilyScreen::refreshHome(bool resetFocus)
{
    if (!homePage)
        return;
    bool hasUnread = restored && unread > 0;
    if (restored)
        lv_label_set_text_fmt(rowLabels[0], FAMILY_STR_MESSAGES, (unsigned)unread);
    else
        lv_label_set_text(rowLabels[0], FAMILY_STR_MESSAGES_LOADING);
    if (hasUnread)
        lv_obj_remove_state(rows[0], LV_STATE_DISABLED);
    else
        lv_obj_add_state(rows[0], LV_STATE_DISABLED);

    if (page != eHome)
        return;
    lv_obj_t *focused = lv_group_get_focused(group);
    lv_obj_t *focus = hasUnread ? rows[0] : rows[1];
    if (!resetFocus && focused && (focused != rows[0] || hasUnread))
        focus = focused;
    rebuildGroup(focus);
}

void FamilyScreen::refreshStrip(void)
{
    char buf[16] = "";
#ifdef ARCH_PORTDUINO
    time_t now = time(nullptr);
#else
    time_t now = view->actTime;
#endif
    if (VALID_TIME(now)) {
        std::tm tm{};
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
    }
    lv_label_set_text(clockLabel, buf);
    lv_label_set_text(batteryLabel, objects.battery_percentage_label ? lv_label_get_text(objects.battery_percentage_label) : "");
}

/**
 * Put exactly the focusable objects of the current page into the family group.
 */
void FamilyScreen::rebuildGroup(lv_obj_t *focus)
{
    lv_group_remove_all_objs(group);
    lv_group_set_editing(group, false);
    switch (page) {
    case eHome:
        for (int i = 0; i < 3; i++) {
            if (!lv_obj_has_state(rows[i], LV_STATE_DISABLED))
                lv_group_add_obj(group, rows[i]);
        }
        break;
    case eRead:
        for (uint32_t i = 0; i < lv_obj_get_child_count(readList); i++)
            lv_group_add_obj(group, lv_obj_get_child(readList, i));
        break;
    case eSend:
        lv_group_add_obj(group, sendBox);
        break;
    }
    if (focus && lv_obj_get_group(focus) == group)
        lv_group_focus_obj(focus);
}

/**
 * Keyboard and trackball belong to the family group while the overlay is on the loaded screen,
 * and to MUI's default group otherwise (dev mode, blank screen, lock screen, boot screen).
 */
void FamilyScreen::assignGroup(void)
{
    lv_group_t *want = (shown && mainScreenActive) ? group : lv_group_get_default();
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if ((type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER) && lv_indev_get_group(indev) != want)
            lv_indev_set_group(indev, want);
    }
}

/**
 * Bring a dimmed or blanked screen back. MUI only leaves power save once the blank screen
 * button was pressed, so set its unlock request as the button would.
 */
void FamilyScreen::wake(void)
{
    if (TFTView_320x240::screenLocked)
        TFTView_320x240::screenUnlockRequest = true;
    lv_display_trigger_activity(NULL);
}

void FamilyScreen::openRead(bool firstUnread)
{
    int focus = count - 1;
    if (firstUnread && unread > 0)
        focus = count - (int)std::min<uint32_t>(unread, count);
    markRead();
    showPage(eRead);
    fillRead(focus);
}

/**
 * Rebuild the thread, one focusable card per message, and focus the card at index.
 */
void FamilyScreen::fillRead(int focusIndex)
{
    lv_obj_clean(readList);

    if (!restored || count == 0) {
        lv_obj_t *card = createPlain(readList);
        lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_add_style(card, &styleCard, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(card, &styleCardFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, ui_event_card, LV_EVENT_ALL, nullptr);
        card->user_data = (void *)(intptr_t)-1;
        createLabel(card, &ui_font_montserrat_20, colorText, restored ? FAMILY_STR_NO_MESSAGES : FAMILY_STR_LOADING);
        rebuildGroup(card);
        return;
    }

    for (int i = 0; i < count; i++) {
        const Entry &e = at(i);
        lv_obj_t *card = createPlain(readList);
        lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_style(card, &styleCard, LV_PART_MAIN | LV_STATE_DEFAULT);
        if (e.outgoing)
            lv_obj_add_style(card, &styleOwnCard, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(card, &styleCardFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_flag(card, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS));
        lv_obj_add_event_cb(card, ui_event_card, LV_EVENT_ALL, nullptr);
        card->user_data = (void *)(intptr_t)i;

        createLabel(card, &ui_font_montserrat_16, e.outgoing ? colorOwnSender : colorSender, "");
        lv_obj_t *text = createLabel(card, &ui_font_montserrat_20, colorText, e.text);
        lv_obj_set_width(text, lv_pct(100));
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    }
    refreshRead();
    lv_obj_update_layout(readList);

    if (focusIndex < 0)
        focusIndex = 0;
    if (focusIndex >= count)
        focusIndex = count - 1;
    lv_obj_t *focus = lv_obj_get_child(readList, focusIndex);
    rebuildGroup(focus);
    // put the focused card at the top, so the messages after it (the rest of the unread) show below it
    int32_t maxY = lv_obj_get_scroll_y(readList) + lv_obj_get_scroll_bottom(readList);
    lv_obj_scroll_to_y(readList, std::min(lv_obj_get_y(focus), maxY), LV_ANIM_OFF);
}

/**
 * Rewrite the card headers: sender, status and time. Times are relative, so this runs periodically.
 */
void FamilyScreen::refreshRead(void)
{
    for (uint32_t c = 0; c < lv_obj_get_child_count(readList); c++) {
        lv_obj_t *card = lv_obj_get_child(readList, c);
        int i = (int)(intptr_t)card->user_data;
        if (i < 0 || i >= count)
            continue;
        const Entry &e = at(i);
        char name[48], when[32], buf[112];
        formatTime(e, when, sizeof(when));
        if (e.outgoing) {
            snprintf(buf, sizeof(buf), "%s · %s%s%s", FAMILY_STR_ME, e.failed ? FAMILY_STR_NOT_SENT : FAMILY_STR_SENT,
                     *when ? " · " : "", when);
        } else {
            snprintf(buf, sizeof(buf), "%s%s%s%s", senderName(e.from, name, sizeof(name)),
                     e.to != UINT32_MAX ? " · " FAMILY_STR_PRIVATE : "", *when ? " · " : "", when);
        }
        lv_obj_t *header = lv_obj_get_child(card, 0);
        lv_label_set_text(header, buf);
        lv_obj_set_style_text_color(header, e.outgoing ? (e.failed ? colorFailed : colorOwnSender) : colorSender,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/**
 * Long name as MUI shows it in the node list, else the hex short id that MUI's newMessage() prepends.
 */
const char *FamilyScreen::senderName(uint32_t nodeNum, char *buf, size_t len)
{
    auto it = view->nodes.find(nodeNum);
    if (it != view->nodes.end() && it->second && lv_obj_get_child_count(it->second) > c_nodeLabelIdx) {
        const char *name = lv_label_get_text(lv_obj_get_child(it->second, c_nodeLabelIdx));
        if (name && *name) {
            snprintf(buf, len, "%s", name);
            return buf;
        }
    }
    snprintf(buf, len, "%04x", nodeNum & 0xffff);
    return buf;
}

/**
 * Message time if it is valid, else the age since this device received it, else nothing.
 */
void FamilyScreen::formatTime(const Entry &e, char *buf, size_t len)
{
    buf[0] = '\0';
    if (VALID_TIME(e.time)) {
        time_t t = e.time;
#ifdef ARCH_PORTDUINO
        time_t now = time(nullptr);
#else
        time_t now = view->actTime;
#endif
        std::tm msg_tm{}, now_tm{};
        localtime_r(&t, &msg_tm);
        localtime_r(&now, &now_tm);
        bool today = VALID_TIME(now) && msg_tm.tm_year == now_tm.tm_year && msg_tm.tm_yday == now_tm.tm_yday;
        strftime(buf, len, today ? "%H:%M" : "%d-%m %H:%M", &msg_tm);
    } else if (e.uptime) {
        uint32_t age = uptimeSeconds() - e.uptime;
        if (age < 60)
            snprintf(buf, len, "%s", FAMILY_STR_JUST_NOW);
        else if (age < 3600)
            snprintf(buf, len, FAMILY_STR_MINUTES_AGO, (unsigned)(age / 60));
        else if (age < 86400)
            snprintf(buf, len, FAMILY_STR_HOURS_AGO, (unsigned)(age / 3600));
        else
            snprintf(buf, len, FAMILY_STR_DAYS_AGO, (unsigned)(age / 86400));
    }
}

void FamilyScreen::openSend(void)
{
    showPage(eSend);
    rebuildGroup(sendBox);
    // show the cursor although the text area itself is not in a group
    lv_obj_add_state(textArea, LV_STATE_FOCUSED);
    lv_obj_send_event(textArea, LV_EVENT_FOCUSED, nullptr);
    lv_label_set_text_fmt(sendCount, "%u/%u", (unsigned)strlen(lv_textarea_get_text(textArea)), (unsigned)c_maxSendBytes);
}

/**
 * Send through MUI, so ack tracking, the /messages log and the MUI chat stay consistent.
 */
void FamilyScreen::send(void)
{
    const char *text = lv_textarea_get_text(textArea);
    if (!text || !*text)
        return;
    char buf[c_maxSendBytes + 1];
    snprintf(buf, sizeof(buf), "%s", text);

    lv_obj_t *saved = view->activeMsgContainer;
    lv_obj_t *container = view->channelGroup[FAMILY_CHANNEL];
    if (!container)
        container = view->newMessageContainer(0, UINT32_MAX, FAMILY_CHANNEL);
    container->user_data = (void *)(uintptr_t)FAMILY_CHANNEL;
    if (container != saved)
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    view->activeMsgContainer = container;
    view->handleAddMessage(buf); // ends in sentMessage()
    view->activeMsgContainer = saved;

    lv_textarea_set_text(textArea, "");
    openRead(false);
}

// ===== readiness gate and dev mode =====

/**
 * The overlay only shows on a configured node: config received, region set, and a private
 * key on the family channel (size 1 is the public default key, size 0 no encryption).
 */
bool FamilyScreen::ready(void)
{
    if (!view->configComplete || view->db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        return false;
    uint32_t pskSize = view->db.channel[FAMILY_CHANNEL].settings.psk.size;
    return pskSize == 16 || pskSize == 32;
}

void FamilyScreen::tick(void)
{
    hookInput();

    if (!restored && view->messagesRestored) {
        restored = true;
        ILOG_INFO("family: %d messages, %u unread", count, (unsigned)unread);
        refreshHome(true);
        if (page == eRead)
            fillRead(count - 1);
    }

    if (devMode) {
        if (lv_display_get_inactive_time(NULL) > c_devModeIdleMs) {
            ILOG_INFO("family: dev mode idle, leaving");
            leaveDevMode();
        }
    } else {
        bool ok = ready();
        // never pop up over a MUI dialog, e.g. while the family channel is being set up
        if (ok && !shown && view->activeSettings == TFTView_320x240::eNone)
            show();
        else if (!ok && shown)
            hide();
    }

    if (shown) {
        if (lv_obj_get_index(root) != (int32_t)lv_obj_get_child_count(objects.main_screen) - 1)
            lv_obj_move_foreground(root);
        refreshStrip();
        if (page == eRead && ++ticks % 30 == 0) // relative times ("5 min geleden") move on
            refreshRead();
    }
    if (alertHideAt && (int32_t)(millis() - alertHideAt) >= 0)
        alert(nullptr, false);

    assignGroup();
}

void FamilyScreen::enterDevMode(void)
{
    ILOG_INFO("family: dev mode on");
    devMode = true;
    lv_display_trigger_activity(NULL); // the swallowed chord key did not count as activity; idle starts now
    hide();
    view->ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
}

void FamilyScreen::leaveDevMode(void)
{
    ILOG_INFO("family: dev mode off");
    devMode = false;
    // close whatever MUI has open, as screenSaving(false) does; a nested dialog needs more than one cancel
    for (int i = 0; i < 3 && view->activeSettings != TFTView_320x240::eNone; i++) {
        lv_event_t e{};
        e.code = LV_EVENT_CLICKED;
        TFTView_320x240::ui_event_cancel(&e);
    }
    lv_obj_add_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN);
    view->ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
    if (ready())
        show();
}

// ===== input =====

/**
 * Wrap the read callback of every keypad, so the chord works on the family screen and anywhere in MUI.
 */
void FamilyScreen::hookInput(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD || lv_indev_get_read_cb(indev) == keyboard_read)
            continue;
        for (int i = 0; i < c_maxKeyboards; i++) {
            if (!wrappedIndev[i] || wrappedIndev[i] == indev) {
                wrappedIndev[i] = indev;
                wrappedRead[i] = lv_indev_get_read_cb(indev);
                lv_indev_set_read_cb(indev, keyboard_read);
                break;
            }
        }
    }
}

void FamilyScreen::trackBall(void)
{
#if defined(INPUTDRIVER_ENCODER_BTN)
    // the trackball click is GPIO 0 (the boot button) and reads LOW while held
    if (digitalRead(INPUTDRIVER_ENCODER_BTN) == LOW) {
        if (!ballDownSince)
            ballDownSince = millis() ? millis() : 1;
    } else {
        ballDownSince = 0;
    }
#endif
}

bool FamilyScreen::ballHeld(uint32_t ms)
{
#if defined(INPUTDRIVER_ENCODER_BTN)
    return ballDownSince && millis() - ballDownSince >= ms;
#elif defined(ARCH_PORTDUINO)
    return escPressedAt && millis() - escPressedAt < 2000;
#else
    return false;
#endif
}

void FamilyScreen::keyboard_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    for (int i = 0; i < c_maxKeyboards; i++) {
        if (wrappedIndev[i] == indev) {
            wrappedRead[i](indev, data);
            break;
        }
    }
    if (!family)
        return;
    family->trackBall();

    if (data->state != LV_INDEV_STATE_PRESSED) {
        chordLatched = false;
        return;
    }
#if !defined(INPUTDRIVER_ENCODER_BTN) && defined(ARCH_PORTDUINO)
    if (data->key == LV_KEY_ESC)
        escPressedAt = millis();
#endif
    bool chordKey = data->key == 'p' || data->key == 'P';
    if (chordKey && (chordLatched || family->ballHeld(c_chordHoldMs))) {
        // swallow the key, and the held trackball must not land as a click on release
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
        if (!chordLatched) {
            chordLatched = true;
#if !defined(INPUTDRIVER_ENCODER_BTN) && defined(ARCH_PORTDUINO)
            escPressedAt = 0;
#endif
            for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) {
                if (lv_indev_get_type(i) == LV_INDEV_TYPE_ENCODER)
                    lv_indev_wait_release(i);
            }
            lv_async_call(async_toggle_dev_mode, nullptr);
        }
    }
}

void FamilyScreen::async_toggle_dev_mode(void *)
{
    if (!family)
        return;
    if (family->devMode)
        family->leaveDevMode();
    else if (family->shown)
        family->enterDevMode();
}

void FamilyScreen::timer_tick(lv_timer_t *)
{
    if (family)
        family->tick();
}

void FamilyScreen::ui_event_screen(lv_event_t *e)
{
    family->mainScreenActive = lv_event_get_code(e) == LV_EVENT_SCREEN_LOAD_START;
    family->assignGroup();
}

/**
 * Arrow keys of a desktop keyboard move the focus. Trackball left/right arrive as the same keys
 * from the encoder and stay inert.
 */
static bool keypadArrow(lv_event_t *e, lv_group_t *group)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY || !lv_indev_active() || lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_KEYPAD)
        return false;
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP)
        lv_group_focus_prev(group);
    else if (key == LV_KEY_DOWN)
        lv_group_focus_next(group);
    else
        return false;
    return true;
}

void FamilyScreen::ui_event_row(lv_event_t *e)
{
    if (keypadArrow(e, family->group) || lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0:
        family->openRead(true);
        break;
    case 1:
        family->openRead(false);
        break;
    case 2:
        family->openSend();
        break;
    }
}

void FamilyScreen::ui_event_card(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        family->goHome();
    } else if (code == LV_EVENT_KEY && !keypadArrow(e, family->group)) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_ESC)
            family->goHome();
    }
}

void FamilyScreen::ui_event_send_box(lv_event_t *e)
{
    FamilyScreen &f = *family;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        f.send(); // trackball press, or Enter (which arrives as KEY then CLICKED)
        return;
    }
    if (code != LV_EVENT_KEY)
        return;

    uint32_t key = lv_event_get_key(e);
    const char *text = lv_textarea_get_text(f.textArea);
    if (key == LV_KEY_BACKSPACE) {
        if (!*text)
            f.goHome();
        else
            lv_textarea_delete_char(f.textArea);
    } else if (key == LV_KEY_ESC) {
        f.goHome();
    } else if (key >= 0x20 && key != 0x7f) {
        bool keypad = lv_indev_active() && lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_KEYPAD;
        if (keypad && strlen(text) + utf8Length(key) <= c_maxSendBytes)
            lv_textarea_add_char(f.textArea, key);
    }
    lv_label_set_text_fmt(f.sendCount, "%u/%u", (unsigned)strlen(lv_textarea_get_text(f.textArea)), (unsigned)c_maxSendBytes);
}

#endif
