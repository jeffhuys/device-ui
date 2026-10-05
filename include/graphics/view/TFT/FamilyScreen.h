#pragma once

#if defined(FAMILY_UI)
#if !defined(VIEW_320x240)
#error "FAMILY_UI needs VIEW_320x240"
#endif

#ifndef FAMILY_CHANNEL
#define FAMILY_CHANNEL 0
#endif

#include "lvgl.h"
#include "util/LogMessage.h"
#include <stdint.h>

class TFTView_320x240;

/**
 * @brief Three-row family screen (Berichten, Lezen, Bericht sturen) on top of MUI.
 *        The screen is a full-size overlay that is the last child of the main screen,
 *        with its own input group. MUI keeps running underneath and is reachable through
 *        the dev chord: hold the trackball for 1s, then press P.
 *        All entry points are static and called from guarded hooks in TFTView_320x240.
 */
class FamilyScreen
{
  public:
    // end of TFTView_320x240::init_screens(): build the overlay, hidden until the device is ready
    static void attach(TFTView_320x240 *view);
    // TFTView_320x240::newMessage(); only live messages are taken, restored ones come through restoreMessage()
    static void newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t msgTime, bool restore);
    // TFTView_320x240::restoreMessage(): a record of the /messages log, own or others
    static void restoreMessage(const LogMessage &msg);
    // TFTView_320x240::handleAddMessage(): an own message was handed to the radio
    static void sentMessage(uint32_t to, uint8_t ch, uint32_t msgTime, uint32_t requestId, const char *msg);
    // TFTView_320x240::handleTextMessageResponse(): routing result of an own message
    static void textMessageResponse(uint32_t channelOrNode, uint32_t id, bool ack, bool err);
    // TFTView_320x240::showMessagePopup(): true while the overlay is up, the popup is skipped
    static bool hidesMessagePopup(void);
    // TFTView_320x240::messageAlert(): mirror MUI alerts onto the family status line
    static void alert(const char *text, bool show);

  private:
    enum Page { eHome, eRead, eSend };

    struct Entry {
        uint32_t from;
        uint32_t to;
        uint32_t time;      // message time as logged, often invalid during an outage
        uint32_t uptime;    // seconds since boot + 1 at receipt; 0 for restored messages
        uint32_t requestId; // own messages sent in this session
        uint8_t ch;
        bool outgoing;
        bool failed;
        char text[messagePayloadSize + 1];
    };

    // identifies the newest message that has been read; persisted in littlefs
    struct Marker {
        uint32_t magic;
        uint32_t from;
        uint32_t to;
        uint32_t time;
        uint32_t len;
        uint32_t hash;
    };

    static constexpr int c_maxEntries = 50;
    static constexpr size_t c_maxSendBytes = 200;

    FamilyScreen(TFTView_320x240 *view);

    // model
    bool isFamily(uint32_t from, uint32_t to, uint8_t ch) const;
    Entry &push(void);
    Entry &at(int i) { return entries[(head + i) % c_maxEntries]; }
    void add(uint32_t from, uint32_t to, uint8_t ch, uint32_t time, const char *text, size_t len, bool outgoing, bool live,
             uint32_t requestId);
    void removeChat(uint32_t from, uint32_t to, uint8_t ch);
    Marker fingerprint(const Entry &e) const;
    bool matches(const Entry &e) const;
    void markRead(void);
    void loadMarker(void);
    void saveMarker(void);

    // ui
    void build(void);
    void buildHome(void);
    void buildRead(void);
    void buildSend(void);
    void styleBadge(void);
    void show(void);
    void hide(void);
    void showPage(Page page);
    void openRead(bool firstUnread);
    void fillRead(int focusIndex);
    void refreshRead(void);
    void openSend(void);
    void send(void);
    void goHome(void);
    void refreshHome(bool resetFocus);
    void refreshStrip(void);
    void rebuildGroup(lv_obj_t *focus);
    void assignGroup(void);
    void wake(void);
    const char *senderName(uint32_t nodeNum, char *buf, size_t len);
    void formatTime(const Entry &e, char *buf, size_t len);

    // theme (FamilyTheme.cpp): the VOXL black and white over all of MUI
    void installTheme(void);
    void restyleMui(void);

    // readiness gate and dev mode
    bool ready(void);
    void tick(void);
    void enterDevMode(void);
    void leaveDevMode(void);
    bool screenSaverActive(void);

    // input
    void hookInput(void);
    bool ballHeld(uint32_t ms);
    void trackBall(void);

    static void keyboard_read(lv_indev_t *indev, lv_indev_data_t *data);
    static void timer_tick(lv_timer_t *timer);

#ifdef ARCH_PORTDUINO
  public:
    // desktop build only (FamilySim.cpp): commands from the file named by FAMILY_SIM_CMDS
    static void simCommandStatic(const char *line);

  private:
    void startSim(void);
    void simCommand(const char *line);
#endif
    static void async_toggle_dev_mode(void *);
    static void ui_event_screen(lv_event_t *e);
    static void ui_event_row(lv_event_t *e);
    static void ui_event_card(lv_event_t *e);
    static void ui_event_send_box(lv_event_t *e);

    static FamilyScreen *family;

    TFTView_320x240 *view;
    lv_group_t *group = nullptr;

    // overlay objects
    lv_obj_t *root = nullptr;
    lv_obj_t *homePage = nullptr;
    lv_obj_t *readPage = nullptr;
    lv_obj_t *sendPage = nullptr;
    lv_obj_t *clockLabel = nullptr;
    lv_obj_t *alertChip = nullptr;
    lv_obj_t *alertLabel = nullptr;
    lv_obj_t *batteryLabel = nullptr;
    lv_obj_t *rows[3] = {};
    lv_obj_t *rowLabels[3] = {};
    lv_obj_t *badge = nullptr;
    lv_obj_t *badgeLabel = nullptr;
    lv_obj_t *readPosition = nullptr;
    lv_obj_t *readList = nullptr;
    lv_obj_t *textArea = nullptr;
    lv_obj_t *sendBox = nullptr;
    lv_obj_t *sendCount = nullptr;

    Page page = eHome;
    bool shown = false;            // overlay is up
    bool devMode = false;          // MUI is in front because of the chord
    bool mainScreenActive = false; // main screen (and so the overlay) is the loaded screen
    bool restored = false;         // /messages log has been read back
    uint32_t alertHideAt = 0;      // millis() to clear a transient alert, 0 = keep
    uint32_t ticks = 0;            // 1s family timer runs

    // messages, oldest first
    Entry entries[c_maxEntries];
    int head = 0;
    int count = 0;
    uint32_t unread = 0;
    Marker marker{};

    // trackball button (or the desktop stand-in)
    uint32_t ballDownSince = 0;
};

#endif
