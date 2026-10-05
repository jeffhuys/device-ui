#if defined(FAMILY_UI) && defined(VIEW_320x240)

// The VOXL look over all of MUI: black and white, square, raw.
//  - every pixel goes through a grey contrast curve on its way to the panel, so MUI's colours,
//    icons and map tiles come out black and white without touching the generated EEZ code;
//  - MUI's objects lose their rounded corners and shadows, top bars invert, node icons get a
//    black tile with a white frame;
//  - the home top bar reads VOXLnet: the wordmark cut from the logo, then "net".

#include "graphics/view/TFT/FamilyScreen.h"
#include "graphics/view/TFT/TFTView_320x240.h"
#include "family_strings.h"
#include "lvgl_private.h"
#include "ui.h"
#include "util/ILog.h"

LV_IMAGE_DECLARE(family_wordmark_18);
LV_FONT_DECLARE(family_font_20);

// luminance at or below c_lowY becomes black, at or above c_highY white, smooth in between so
// anti-aliased text stays soft. Picked on screenshots of every MUI panel.
constexpr int c_lowY = 40;
constexpr int c_highY = 200;

static lv_display_flush_cb_t muiFlush = nullptr;
static uint8_t curve[256];

static void buildCurve(void)
{
    for (int y = 0; y < 256; y++) {
        if (y <= c_lowY) {
            curve[y] = 0;
        } else if (y >= c_highY) {
            curve[y] = 255;
        } else {
            int t = (y - c_lowY) * 1024 / (c_highY - c_lowY);       // 0..1024
            int s = t * t / 1024 * (3 * 1024 - 2 * t) / 1024;     // smoothstep, 0..1024
            curve[y] = (uint8_t)(s * 255 / 1024);
        }
    }
}

static inline uint8_t monoY(uint8_t r, uint8_t g, uint8_t b)
{
    return curve[(r * 77 + g * 150 + b * 29) >> 8];
}

static inline uint16_t mono565(uint16_t c)
{
    uint8_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    uint8_t v = monoY((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
    return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
}

/**
 * Wraps the driver's flush callback: turn the rendered area grey-scale through the curve, then hand
 * it on. Only installed in partial render mode, where every flushed pixel was freshly drawn.
 */
static void mono_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t n = lv_area_get_size(area);
    switch (lv_display_get_color_format(disp)) {
    case LV_COLOR_FORMAT_RGB565: {
        uint16_t *p = (uint16_t *)px_map;
        for (uint32_t i = 0; i < n; i++)
            p[i] = mono565(p[i]);
        break;
    }
    case LV_COLOR_FORMAT_RGB565_SWAPPED: {
        uint16_t *p = (uint16_t *)px_map;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t c = mono565((uint16_t)((p[i] >> 8) | (p[i] << 8)));
            p[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        break;
    }
    case LV_COLOR_FORMAT_XRGB8888:
    case LV_COLOR_FORMAT_ARGB8888:
    case LV_COLOR_FORMAT_RGB888: {
        uint32_t size = lv_color_format_get_size(lv_display_get_color_format(disp)); // bytes B, G, R[, A]
        uint8_t *p = px_map;
        for (uint32_t i = 0; i < n; i++, p += size)
            p[0] = p[1] = p[2] = monoY(p[2], p[1], p[0]);
        break;
    }
    default:
        break;
    }
#ifdef ARCH_PORTDUINO
    FamilyScreen::simFrame(disp, area, px_map);
#endif
    muiFlush(disp, area, px_map);
}

// ===== small helpers that only touch a style when it differs, so a periodic pass does not redraw =====

static void setBg(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c) || lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != LV_OPA_COVER) {
        lv_obj_set_style_bg_color(obj, c, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void setText(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(obj, c, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void setRecolor(lv_obj_t *obj, lv_color_t c)
{
    // MUI icons are either an image object or a background image on a button
    if (!lv_color_eq(lv_obj_get_style_image_recolor(obj, LV_PART_MAIN), c) ||
        lv_obj_get_style_image_recolor_opa(obj, LV_PART_MAIN) != LV_OPA_COVER) {
        lv_obj_set_style_image_recolor(obj, c, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (!lv_color_eq(lv_obj_get_style_bg_image_recolor(obj, LV_PART_MAIN), c) ||
        lv_obj_get_style_bg_image_recolor_opa(obj, LV_PART_MAIN) != LV_OPA_COVER) {
        lv_obj_set_style_bg_image_recolor(obj, c, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_image_recolor_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/**
 * Square corners and no shadows, once per object (LV_OBJ_FLAG_USER_4 marks the done ones).
 */
static void squareTree(lv_obj_t *obj, lv_obj_t *skip)
{
    if (obj == skip)
        return;
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_4)) {
        static const lv_style_selector_t states[] = {LV_STATE_DEFAULT, LV_STATE_FOCUSED, LV_STATE_PRESSED, LV_STATE_CHECKED};
        for (lv_style_selector_t st : states)
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | st);
        lv_obj_set_style_radius(obj, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(obj, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(obj, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_color(obj, lv_color_white(), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        // filled buttons (settings, channels, dialogs): black with a white frame instead of a grey slab
        if (lv_obj_check_type(obj, &lv_button_class) && lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) >= LV_OPA_50 &&
            lv_obj_get_style_bg_image_src(obj, LV_PART_MAIN) == nullptr) {
            lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(obj, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            // behind an open dialog MUI disables them: black, a grey frame, grey text (not a light slab)
            lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN | LV_STATE_DISABLED);
            lv_obj_set_style_border_color(obj, lv_color_hex(0x5c5c5c), LV_PART_MAIN | LV_STATE_DISABLED);
            lv_obj_set_style_text_color(obj, lv_color_hex(0x787878), LV_PART_MAIN | LV_STATE_DISABLED);
            lv_obj_set_style_recolor_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DISABLED); // the theme's 50 % grey wash
        }
        lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_4);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        squareTree(lv_obj_get_child(obj, i), skip);
}

/**
 * A top bar as an inverted block, like NETWORK under the logo: white, with black text and icons.
 */
static void invertBar(lv_obj_t *bar)
{
    if (!bar)
        return;
    setBg(bar, lv_color_white());
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *child = lv_obj_get_child(bar, i);
        setText(child, lv_color_black());
        setRecolor(child, lv_color_black());
    }
}

void FamilyScreen::installTheme(void)
{
    buildCurve();
    lv_display_t *disp = lv_display_get_default();
    if (disp && disp->render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL && disp->flush_cb != mono_flush) {
        muiFlush = disp->flush_cb;
        lv_display_set_flush_cb(disp, mono_flush);
        ILOG_INFO("family: black and white display filter on");
    } else {
        ILOG_WARN("family: display not in partial render mode, no black and white filter");
    }

    // VOXLnet instead of the Meshtastic mark and name on MUI's home top bar
    lv_image_set_src(objects.meshtastic_image, &family_wordmark_18);
    lv_obj_set_size(objects.meshtastic_image, family_wordmark_18.header.w, family_wordmark_18.header.h);
    lv_obj_set_pos(objects.meshtastic_image, -20, 0);
    lv_label_set_text(objects.meshtastic_label, FAMILY_STR_BRAND_SUFFIX);
    lv_obj_set_style_text_font(objects.meshtastic_label, &family_font_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(objects.meshtastic_label, 34, -1);

#ifdef ARCH_PORTDUINO
    // the X11 mouse cursor sits in the screenshots; the T-Deck has none
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && indev->cursor)
            lv_obj_add_flag(indev->cursor, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    restyleMui();
}

/**
 * Re-applied every second while MUI is in front: it recolours and creates objects at runtime.
 */
void FamilyScreen::restyleMui(void)
{
    squareTree(objects.main_screen, root);
    if (objects.lock_screen)
        squareTree(objects.lock_screen, nullptr);

    lv_obj_t *bars[] = {objects.top_panel,
                        objects.top_nodes_panel,
                        objects.top_groups_panel,
                        objects.top_messages_panel,
                        objects.top_group_chat_panel,
                        objects.top_chats_panel,
                        objects.top_map_panel,
                        objects.top_settings_panel,
                        objects.top_setup_panel,
                        objects.top_advanced_settings_panel,
                        objects.top_node_options_panel,
                        objects.top_node_search_panel,
                        objects.top_neighbors_panel,
                        objects.top_lora_tx_panel,
                        objects.top_mesh_detector_panel,
                        objects.top_signal_scanner_panel,
                        objects.top_trace_route_panel,
                        objects.top_statistics_panel,
                        objects.top_packet_log_panel};
    for (lv_obj_t *bar : bars)
        invertBar(bar);
    invertBar(objects.battery_panel); // the top right corner, so the bar runs to the edge

    // dialogs: a heavy white frame on black, like the family blocks
    lv_obj_t *dialogs[] = {objects.initial_setup_panel,         objects.reboot_panel,
                           objects.alert_panel,                 objects.msg_popup_panel,
                           objects.settings_username_panel,     objects.settings_channel_panel,
                           objects.settings_region_panel,       objects.settings_modem_preset_panel,
                           objects.settings_device_role_panel,  objects.settings_wifi_panel,
                           objects.settings_brightness_panel,   objects.settings_theme_panel,
                           objects.settings_screen_timeout_panel, objects.settings_screen_lock_panel,
                           objects.settings_input_control_panel, objects.settings_alert_buzzer_panel,
                           objects.settings_language_panel,     objects.settings_timezone,
                           objects.settings_backup_restore_panel, objects.settings_reset_panel,
                           objects.settings_modify_channel_panel, objects.settings_reboot_panel};
    for (lv_obj_t *d : dialogs) {
        if (!d)
            continue;
        setBg(d, lv_color_black());
        if (lv_obj_get_style_border_width(d, LV_PART_MAIN) != 3 ||
            !lv_color_eq(lv_obj_get_style_border_color(d, LV_PART_MAIN), lv_color_white())) {
            lv_obj_set_style_border_width(d, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(d, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(d, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    // the navigation column: black, icons white; MUI marks the active button with its frame
    setBg(objects.button_panel, lv_color_black());
    lv_obj_t *nav[] = {objects.home_button,     objects.nodes_button, objects.groups_button,
                       objects.messages_button, objects.map_button,   objects.settings_button};
    for (lv_obj_t *b : nav)
        setBg(b, lv_color_black());

    // node icons sit on a per-node colour; a black tile with a white frame keeps them readable
    for (auto &it : view->nodes) {
        lv_obj_t *panel = it.second;
        if (!panel || lv_obj_get_child_count(panel) == 0)
            continue;
        lv_obj_t *img = lv_obj_get_child(panel, 0);
        setBg(img, lv_color_black());
        setRecolor(img, lv_color_white());
        if (lv_obj_get_style_border_width(img, LV_PART_MAIN) != 2) {
            lv_obj_set_style_border_width(img, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(img, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

#endif
