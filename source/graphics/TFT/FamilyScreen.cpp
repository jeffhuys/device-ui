#if defined(FAMILY_UI) && defined(VIEW_320x240)

#include "graphics/view/TFT/FamilyScreen.h"
// the filesystem headers go before lv_i18n.h, whose _p() macro clashes with portduino FS.h
#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
#else
#include "LittleFS.h"
#endif
#include "Arduino.h"
#include "graphics/driver/DisplayDriver.h"
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
#if defined(T_DECK) && !defined(ARCH_PORTDUINO)
#include <Wire.h>
#endif

extern fs::FS &persistentFS; // ViewController.cpp, the filesystem that holds /messages

LV_FONT_DECLARE(family_font_28);
LV_FONT_DECLARE(family_font_20);
LV_FONT_DECLARE(family_font_16);
LV_FONT_DECLARE(family_font_14);
LV_IMAGE_DECLARE(family_wordmark_16);

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

// the logo's palette: black, white, and two greys for what is switched off
constexpr lv_color_t colorBlack = LV_COLOR_HEX(0x000000);
constexpr lv_color_t colorWhite = LV_COLOR_HEX(0xffffff);
// the display filter (FamilyTheme.cpp) crushes dark greys; these come out at about 0x46 and 0x80
constexpr lv_color_t colorMuted = LV_COLOR_HEX(0x5c5c5c);     // frame of a disabled row
constexpr lv_color_t colorMutedText = LV_COLOR_HEX(0x787878); // its text
constexpr int32_t c_frame = 3;      // block frame width
constexpr int32_t c_cut = 12;       // corner cut, along each edge
constexpr int32_t c_gap = 6;        // page padding and spacing
constexpr int32_t c_barHeight = 22; // bars and the status strip

FamilyScreen *FamilyScreen::family = nullptr;

// keyboard indevs whose read callback is wrapped for the dev chord
static constexpr int c_maxKeyboards = 4;
static lv_indev_t *wrappedIndev[c_maxKeyboards] = {};
static lv_indev_read_cb_t wrappedRead[c_maxKeyboards] = {};
static bool chordLatched = false; // swallow the chord key until it is released
#if !defined(INPUTDRIVER_ENCODER_BTN) && defined(ARCH_PORTDUINO)
static uint32_t escPressedAt = 0; // desktop stand-in for the held trackball: Esc, then P
#endif

static lv_style_t styleBlock, styleBlockFocused, styleBlockDisabled, styleCard, styleOwnCard, styleCardFocused,
    styleOwnCardFocused, styleBar, styleChip;

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
    family->installTheme();
    family->hookInput();
    family->mainScreenActive = lv_screen_active() == objects.main_screen;
    lv_obj_add_event_cb(objects.main_screen, ui_event_screen, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_add_event_cb(objects.main_screen, ui_event_screen, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_timer_create(timer_tick, 1000, nullptr);
#ifdef ARCH_PORTDUINO
    family->startSim();
#endif
    family->startBoot();
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
    if (!family || !(family->shown || family->booting))
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
        lv_obj_add_flag(f.alertChip, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_clear_flag(f.alertChip, LV_OBJ_FLAG_HIDDEN);
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

/**
 * A label that takes its colour from its parent, so an inverted (focused) block turns its text black.
 */
static lv_obj_t *createLabel(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *createPage(lv_obj_t *parent)
{
    lv_obj_t *page = createPlain(parent);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, c_gap, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(page, c_gap, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

/**
 * Inverted bar, like the NETWORK block of the logo: white, bold black title left, a label right.
 * Returns the right-hand label.
 */
static lv_obj_t *createBar(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = createPlain(parent);
    lv_obj_set_size(bar, lv_pct(100), c_barHeight);
    lv_obj_add_style(bar, &styleBar, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *left = createLabel(bar, &family_font_16, title);
    lv_obj_set_align(left, LV_ALIGN_LEFT_MID);
    lv_obj_t *right = createLabel(bar, &family_font_14, "");
    lv_obj_set_align(right, LV_ALIGN_RIGHT_MID);
    return right;
}

/**
 * A small inverted key cap: [ENTER].
 */
static lv_obj_t *createChip(lv_obj_t *parent, const char *text)
{
    lv_obj_t *chip = createPlain(parent);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_style(chip, &styleChip, LV_PART_MAIN | LV_STATE_DEFAULT);
    createLabel(chip, &family_font_14, text);
    return chip;
}

/**
 * Cut the bottom-right corner at 45 degrees, the angle of the logo's letters. Runs after the block
 * and its children are drawn: a border-coloured band along the cut, then the cut itself in black.
 */
static void ui_event_chamfer(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t x = a.x2 + 1, y = a.y2 + 1, c = c_cut;

    int32_t w = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    if (w > 0 && lv_obj_get_style_border_side(obj, LV_PART_MAIN) == LV_BORDER_SIDE_FULL) {
        int32_t s = w * 3 / 2; // the band's width measured along the edges (w * sqrt 2)
        lv_draw_triangle_dsc_t band;
        lv_draw_triangle_dsc_init(&band);
        band.color = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
        band.opa = LV_OPA_COVER;
        band.p[0] = {x - c - s, y};
        band.p[1] = {x - c, y};
        band.p[2] = {x, y - c - s};
        lv_draw_triangle(layer, &band);
        band.p[0] = {x - c, y};
        band.p[1] = {x, y - c};
        band.p[2] = {x, y - c - s};
        lv_draw_triangle(layer, &band);
    }
    lv_draw_triangle_dsc_t cut;
    lv_draw_triangle_dsc_init(&cut);
    cut.color = colorBlack;
    cut.opa = LV_OPA_COVER;
    cut.p[0] = {x - c, y};
    cut.p[1] = {x, y - c};
    cut.p[2] = {x, y};
    lv_draw_triangle(layer, &cut);
}

static void chamfer(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, ui_event_chamfer, LV_EVENT_DRAW_POST, nullptr);
}

void FamilyScreen::build(void)
{
    // blocks: rows and the send box. Black, thick white frame, square; focused rows invert
    lv_style_init(&styleBlock);
    lv_style_set_bg_opa(&styleBlock, LV_OPA_COVER);
    lv_style_set_bg_color(&styleBlock, colorBlack);
    lv_style_set_radius(&styleBlock, 0);
    lv_style_set_border_width(&styleBlock, c_frame);
    lv_style_set_border_color(&styleBlock, colorWhite);
    lv_style_set_pad_left(&styleBlock, 14);
    lv_style_set_pad_right(&styleBlock, 10);
    lv_style_set_text_color(&styleBlock, colorWhite);

    lv_style_init(&styleBlockFocused);
    lv_style_set_bg_color(&styleBlockFocused, colorWhite);
    lv_style_set_text_color(&styleBlockFocused, colorBlack);

    lv_style_init(&styleBlockDisabled);
    lv_style_set_border_color(&styleBlockDisabled, colorMuted);
    lv_style_set_text_color(&styleBlockDisabled, colorMutedText);

    // cards: a white rule on the sender's side; the focused card becomes a white block
    lv_style_init(&styleCard);
    lv_style_set_bg_opa(&styleCard, LV_OPA_COVER);
    lv_style_set_bg_color(&styleCard, colorBlack);
    lv_style_set_radius(&styleCard, 0);
    lv_style_set_border_width(&styleCard, 4);
    lv_style_set_border_color(&styleCard, colorWhite);
    lv_style_set_border_side(&styleCard, LV_BORDER_SIDE_LEFT);
    lv_style_set_pad_top(&styleCard, 5);
    lv_style_set_pad_bottom(&styleCard, 7);
    lv_style_set_pad_hor(&styleCard, 10);
    lv_style_set_pad_row(&styleCard, 1);
    lv_style_set_text_color(&styleCard, colorWhite);

    lv_style_init(&styleOwnCard);
    lv_style_set_border_side(&styleOwnCard, LV_BORDER_SIDE_RIGHT);

    // a focused card gains a frame on all sides; trim the padding so the text keeps its width and
    // does not rewrap under the reader's eyes
    lv_style_init(&styleCardFocused);
    lv_style_set_bg_color(&styleCardFocused, colorWhite);
    lv_style_set_text_color(&styleCardFocused, colorBlack);
    lv_style_set_border_side(&styleCardFocused, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_right(&styleCardFocused, 6);

    lv_style_init(&styleOwnCardFocused);
    lv_style_set_pad_left(&styleOwnCardFocused, 6);
    lv_style_set_pad_right(&styleOwnCardFocused, 10);

    lv_style_init(&styleBar);
    lv_style_set_bg_opa(&styleBar, LV_OPA_COVER);
    lv_style_set_bg_color(&styleBar, colorWhite);
    lv_style_set_text_color(&styleBar, colorBlack);
    lv_style_set_pad_hor(&styleBar, 6);

    lv_style_init(&styleChip);
    lv_style_set_bg_opa(&styleChip, LV_OPA_COVER);
    lv_style_set_bg_color(&styleChip, colorWhite);
    lv_style_set_text_color(&styleChip, colorBlack);
    lv_style_set_pad_hor(&styleChip, 4);
    lv_style_set_pad_ver(&styleChip, 1);

    group = lv_group_create();
    lv_group_set_wrap(group, false);

    root = createPlain(objects.main_screen);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(root, colorBlack, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(root, colorWhite, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE); // swallow pointer input meant for MUI underneath
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    buildHome();
    buildRead();
    buildSend();
}

void FamilyScreen::buildHome(void)
{
    homePage = createPage(root);

    // status strip: wordmark, MUI alert, clock and battery
    lv_obj_t *strip = createPlain(homePage);
    lv_obj_set_size(strip, lv_pct(100), c_barHeight);
    lv_obj_t *mark = lv_image_create(strip);
    lv_image_set_src(mark, &family_wordmark_16);
    lv_obj_set_align(mark, LV_ALIGN_LEFT_MID);
    alertChip = createChip(strip, "");
    alertLabel = lv_obj_get_child(alertChip, 0);
    lv_obj_set_align(alertChip, LV_ALIGN_CENTER);
    lv_obj_add_flag(alertChip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *status = createPlain(strip);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(status, LV_ALIGN_RIGHT_MID);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    batteryLabel = createLabel(status, &family_font_14, "");
    clockLabel = createLabel(status, &family_font_16, "");

    static const char *text[3] = {FAMILY_STR_MESSAGES, FAMILY_STR_READ, FAMILY_STR_SEND};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = createPlain(homePage);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_add_style(row, &styleBlock, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(row, &styleBlockFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_add_style(row, &styleBlockDisabled, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ui_event_row, LV_EVENT_ALL, (void *)(intptr_t)i);
        chamfer(row);
        rows[i] = row;

        rowLabels[i] = createLabel(row, &family_font_28, text[i]);
        lv_obj_set_align(rowLabels[i], LV_ALIGN_LEFT_MID);
    }
    lv_obj_add_state(rows[0], LV_STATE_DISABLED);

    // the unread count, a solid block at the end of row 1
    badge = createPlain(rows[0]);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(badge, 44, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_hor(badge, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(badge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(badge, LV_ALIGN_RIGHT_MID);
    badgeLabel = createLabel(badge, &family_font_28, FAMILY_STR_COUNT_LOADING);
    lv_obj_set_align(badgeLabel, LV_ALIGN_CENTER);
    styleBadge();
}

/**
 * The badge inverts against its row: a white block on a black row, a black block on a focused (white)
 * row, and only a muted frame while there is nothing unread.
 */
void FamilyScreen::styleBadge(void)
{
    bool disabled = lv_obj_has_state(rows[0], LV_STATE_DISABLED);
    bool focused = lv_obj_has_state(rows[0], LV_STATE_FOCUSED);
    lv_color_t fill = disabled ? colorBlack : focused ? colorBlack : colorWhite;
    lv_color_t ink = disabled ? colorMutedText : focused ? colorWhite : colorBlack;
    lv_obj_set_style_bg_color(badge, fill, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(badge, disabled ? colorMuted : fill, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(badgeLabel, ink, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void FamilyScreen::buildRead(void)
{
    readPage = createPage(root);
    readPosition = createBar(readPage, FAMILY_STR_READ_TITLE);

    readList = createPlain(readPage);
    lv_obj_set_width(readList, lv_pct(100));
    lv_obj_set_flex_grow(readList, 1);
    lv_obj_set_flex_flow(readList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(readList, c_gap, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(readList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(readList, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(readList, LV_SCROLL_SNAP_START); // a card starts right under the bar, no slivers
    lv_obj_set_scrollbar_mode(readList, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(readList, colorWhite, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(readList, LV_OPA_COVER, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(readList, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(readList, 3, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
}

void FamilyScreen::buildSend(void)
{
    sendPage = createPage(root);
    sendCount = createBar(sendPage, FAMILY_STR_SEND_TITLE);

    // the focusable box; the text area inside is fed by hand so LVGL's encoder edit mode never applies
    sendBox = createPlain(sendPage);
    lv_obj_set_width(sendBox, lv_pct(100));
    lv_obj_set_flex_grow(sendBox, 1);
    lv_obj_add_style(sendBox, &styleBlock, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sendBox, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(sendBox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sendBox, ui_event_send_box, LV_EVENT_ALL, nullptr);
    chamfer(sendBox);

    textArea = lv_textarea_create(sendBox);
    lv_group_remove_obj(textArea); // textareas join the default (MUI) group on creation
    lv_obj_clear_flag(textArea, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style_all(textArea); // no theme border or outline; the box around it shows the focus
    lv_obj_set_size(textArea, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(textArea, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(textArea, &family_font_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(textArea, colorWhite, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(textArea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_color(textArea, colorWhite, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(textArea, 3, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(textArea, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);

    // key hints: [ENTER] VERSTUREN   [WIS] TERUG
    lv_obj_t *keys = createPlain(sendPage);
    lv_obj_set_size(keys, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(keys, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    createChip(keys, FAMILY_STR_KEY_SEND);
    createLabel(keys, &family_font_14, FAMILY_STR_HINT_SEND);
    lv_obj_t *spacer = createPlain(keys);
    lv_obj_set_size(spacer, 10, 1);
    createChip(keys, FAMILY_STR_KEY_BACK);
    createLabel(keys, &family_font_14, FAMILY_STR_HINT_BACK);
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
        lv_label_set_text_fmt(badgeLabel, FAMILY_STR_COUNT, (unsigned)unread);
    else
        lv_label_set_text(badgeLabel, FAMILY_STR_COUNT_LOADING);
    if (hasUnread)
        lv_obj_remove_state(rows[0], LV_STATE_DISABLED);
    else
        lv_obj_add_state(rows[0], LV_STATE_DISABLED);

    if (page == eHome) {
        lv_obj_t *focused = lv_group_get_focused(group);
        lv_obj_t *focus = hasUnread ? rows[0] : rows[1];
        if (!resetFocus && focused && (focused != rows[0] || hasUnread))
            focus = focused;
        rebuildGroup(focus);
    }
    styleBadge();
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
 * to nobody during the boot animation, and to MUI's default group otherwise (dev mode, blank
 * screen, lock screen, boot screen).
 */
void FamilyScreen::assignGroup(void)
{
    lv_group_t *want = !mainScreenActive ? lv_group_get_default() : booting ? bootGroup : shown ? group : lv_group_get_default();
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

/**
 * The T-Deck's keyboard is an ESP32-C3 at I2C 0x55. LilyGO's keyboard firmware (since 2024-12-25)
 * sets its backlight PWM on a write of 0x01 and a duty; at power-up the light is off, and only
 * Alt+B toggles it. Keep it lit at the screen's brightness while the screen is on, and dark from
 * the moment MUI starts dimming the screen for its timeout. Sent when it changes, and again every
 * 5 s while lit, so Alt+B cannot leave it off. Older keyboard firmware ignores the write.
 * The main screen's load and unload decide by themselves: on wake, MUI loads the main screen before
 * it clears its lock flag and resets the idle time.
 */
void FamilyScreen::keyboardLight(int force)
{
#if defined(T_DECK) && !defined(ARCH_PORTDUINO)
    constexpr uint8_t c_keyboardAddr = 0x55, c_brightnessCmd = 0x01;
    constexpr uint32_t c_resendMs = 5000;
    DisplayDriver *display = view->getDisplayDriver();
    uint32_t timeoutMs = display ? display->getScreenTimeout() * 1000 : 0;
    bool dimming = timeoutMs > 0 && lv_display_get_inactive_time(NULL) > timeoutMs;
    bool on = force >= 0 ? force == 1 : !screenSaverActive() && !dimming && !(display && display->isPowersaving());
    int16_t duty = on ? LV_MAX(view->db.uiConfig.screen_brightness, 64) : 0;
    if (force < 0 && duty == keyboardDuty && (duty == 0 || millis() - keyboardSentAt < c_resendMs))
        return;
    Wire.beginTransmission(c_keyboardAddr);
    Wire.write(c_brightnessCmd);
    Wire.write((uint8_t)duty);
    uint8_t err = Wire.endTransmission();
    if (duty != keyboardDuty)
        ILOG_DEBUG("family: keyboard light %d (i2c %d)", duty, err);
    keyboardDuty = duty;
    keyboardSentAt = millis();
#endif
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
 * One card: a header (sender left, status and time right) over the message text.
 * Labels inherit the card's text colour, so a focused (inverted) card turns them black.
 */
static lv_obj_t *createCard(lv_obj_t *parent, bool own, const char *text)
{
    lv_obj_t *card = createPlain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(card, &styleCard, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (own)
        lv_obj_add_style(card, &styleOwnCard, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(card, &styleCardFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
    if (own)
        lv_obj_add_style(card, &styleOwnCardFocused, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_flag(card, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    chamfer(card);

    lv_obj_t *header = createPlain(card);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *name = createLabel(header, &family_font_16, "");
    lv_obj_set_align(name, LV_ALIGN_LEFT_MID);
    lv_obj_t *when = createLabel(header, &family_font_14, "");
    lv_obj_set_align(when, LV_ALIGN_RIGHT_MID);

    lv_obj_t *body = createLabel(card, &family_font_20, text);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    return card;
}

/**
 * Rebuild the thread, one focusable card per message, and focus the card at index.
 */
void FamilyScreen::fillRead(int focusIndex)
{
    lv_obj_clean(readList);

    if (!restored || count == 0) {
        lv_obj_t *card = createCard(readList, false, restored ? FAMILY_STR_NO_MESSAGES : FAMILY_STR_LOADING);
        lv_obj_add_flag(lv_obj_get_child(card, 0), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(card, ui_event_card, LV_EVENT_ALL, nullptr);
        card->user_data = (void *)(intptr_t)-1;
        lv_label_set_text(readPosition, "");
        rebuildGroup(card);
        return;
    }

    for (int i = 0; i < count; i++) {
        const Entry &e = at(i);
        lv_obj_t *card = createCard(readList, e.outgoing, e.text);
        lv_obj_add_event_cb(card, ui_event_card, LV_EVENT_ALL, nullptr);
        card->user_data = (void *)(intptr_t)i;
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
 * Upper-case ASCII letters for the raw headers; other bytes (é, UTF-8) pass through.
 */
static void upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s -= 'a' - 'A';
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
        char name[48], when[32], right[80];
        formatTime(e, when, sizeof(when));
        if (e.outgoing) {
            snprintf(name, sizeof(name), "%s", FAMILY_STR_ME);
            snprintf(right, sizeof(right), "%s%s%s", e.failed ? "! " FAMILY_STR_NOT_SENT : FAMILY_STR_SENT, *when ? " · " : "",
                     when);
        } else {
            senderName(e.from, name, sizeof(name));
            upper(name);
            snprintf(right, sizeof(right), "%s%s%s", e.to != UINT32_MAX ? FAMILY_STR_PRIVATE : "",
                     e.to != UINT32_MAX && *when ? " · " : "", when);
        }
        lv_obj_t *header = lv_obj_get_child(card, 0);
        lv_label_set_text(lv_obj_get_child(header, 0), name);
        lv_label_set_text(lv_obj_get_child(header, 1), right);
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

void FamilyScreen::checkRestored(void)
{
    if (!restored && view->messagesRestored) {
        restored = true;
        ILOG_INFO("family: %d messages, %u unread", count, (unsigned)unread);
        refreshHome(true);
        if (page == eRead)
            fillRead(count - 1);
    }
}

void FamilyScreen::tick(void)
{
    hookInput();
    checkRestored();
    keyboardLight(-1);

    if (booting) {
        // the boot animation brings the family screen (or MUI's setup) up when it ends
    } else if (devMode) {
        restyleMui(); // MUI recolours and creates objects at runtime
        // MUI does not outlast the screen saver: whoever wakes the device gets the family screen
        if (screenSaverActive()) {
            ILOG_INFO("family: screen saver started, leaving dev mode");
            leaveDevMode();
        } else if (lv_display_get_inactive_time(NULL) > c_devModeIdleMs) {
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
        if (!booting && lv_obj_get_index(root) != (int32_t)lv_obj_get_child_count(objects.main_screen) - 1)
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
    restyleMui();
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
    // ui_set_active() moved MUI's focus to its home panel; while the screen sleeps, the wake press
    // must land on the blank screen button again, or the screen never unlocks
    if (screenSaverActive())
        lv_group_focus_obj(objects.blank_screen_button);
    if (ready())
        show();
}

/**
 * True while MUI shows its blank screen: dimmed into power save (screenSaving) or blanked.
 */
bool FamilyScreen::screenSaverActive(void)
{
    return TFTView_320x240::screenLocked || lv_screen_active() == objects.blank_screen;
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
    if (family->devMode) {
        family->leaveDevMode();
    } else if (family->booting) {
        family->endBoot();
        family->enterDevMode();
    } else if (family->shown) {
        family->enterDevMode();
    }
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
    family->keyboardLight(family->mainScreenActive ? 1 : 0); // the blank screen comes and goes with these
}

/**
 * Arrow keys of a desktop keyboard move the focus. Trackball left/right arrive as the same keys
 * from the encoder and stay inert.
 */
static bool keypadArrow(lv_event_t *e, lv_group_t *group)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY || (lv_indev_active() && lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_KEYPAD))
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
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_DEFOCUSED) {
        family->styleBadge(); // the badge inverts with row 1
        return;
    }
    if (keypadArrow(e, family->group) || code != LV_EVENT_CLICKED)
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
    } else if (code == LV_EVENT_FOCUSED) {
        int i = (int)(intptr_t)((lv_obj_t *)lv_event_get_current_target(e))->user_data;
        if (i >= 0)
            lv_label_set_text_fmt(family->readPosition, FAMILY_STR_READ_POSITION, i + 1, family->count);
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
        bool keypad = !lv_indev_active() || lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_KEYPAD; // no indev: sent by code
        if (keypad && strlen(text) + utf8Length(key) <= c_maxSendBytes)
            lv_textarea_add_char(f.textArea, key);
    }
    lv_label_set_text_fmt(f.sendCount, "%u/%u", (unsigned)strlen(lv_textarea_get_text(f.textArea)), (unsigned)c_maxSendBytes);
}

#endif
