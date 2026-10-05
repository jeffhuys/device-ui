#pragma once

// Dutch labels of the family screen, in one place.
// The family fonts cover 0x20-0x7F, 0xA0-0x17F and "…" (see scripts/gen-family-assets.py in the parent repo).
// Rows, bars and chips are upper case, in the voice of the VOXL logo; message text is left as written.

#if defined(FAMILY_UI) && defined(VIEW_320x240)

// home rows (family_font_28) and the unread badge
#define FAMILY_STR_MESSAGES "BERICHTEN"
#define FAMILY_STR_COUNT "%u"
#define FAMILY_STR_COUNT_LOADING "…"
#define FAMILY_STR_READ "LEZEN"
#define FAMILY_STR_SEND "BERICHT STUREN"

// read (family_font_16 bar and headers, family_font_20 text)
#define FAMILY_STR_READ_TITLE "LEZEN"
#define FAMILY_STR_READ_POSITION "%d / %d"
#define FAMILY_STR_LOADING "Berichten laden…"
#define FAMILY_STR_NO_MESSAGES "Nog geen berichten."
#define FAMILY_STR_ME "IK"
#define FAMILY_STR_PRIVATE "PRIVÉ"
#define FAMILY_STR_SENT "VERSTUURD"
#define FAMILY_STR_NOT_SENT "NIET VERSTUURD"
#define FAMILY_STR_JUST_NOW "ZOJUIST"
#define FAMILY_STR_MINUTES_AGO "%u MIN GELEDEN"
#define FAMILY_STR_HOURS_AGO "%u UUR GELEDEN"
#define FAMILY_STR_DAYS_AGO "%u DAGEN GELEDEN"

// send (family_font_16 bar, family_font_20 text, family_font_14 key hints)
#define FAMILY_STR_SEND_TITLE "BERICHT AAN IEDEREEN"
#define FAMILY_STR_KEY_SEND "ENTER"
#define FAMILY_STR_HINT_SEND "VERSTUREN"
#define FAMILY_STR_KEY_BACK "WIS"
#define FAMILY_STR_HINT_BACK "TERUG"
#define FAMILY_STR_KEY_PRESS "DRUK"

// status line, mirrored from MUI alerts (family_font_14 chip)
#define FAMILY_STR_ALERT_DISCONNECTED "GEEN VERBINDING"
#define FAMILY_STR_ALERT_CONNECTED "VERBONDEN"
#define FAMILY_STR_ALERT_REBOOTING "HERSTARTEN…"
#define FAMILY_STR_ALERT_RESYNC "BIJWERKEN…"
#define FAMILY_STR_ALERT_SHUTDOWN "UITSCHAKELEN…"

// MUI's top bar on the home panel: the VOXL wordmark, then this
#define FAMILY_STR_BRAND_SUFFIX "net"

#endif
