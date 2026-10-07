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
#define FAMILY_STR_NOT_SENT "NIET BEVESTIGD" // no node was heard passing it on; it may still have arrived
#define FAMILY_STR_JUST_NOW "ZOJUIST"
#define FAMILY_STR_MINUTES_AGO "%u MIN GELEDEN"
#define FAMILY_STR_HOURS_AGO "%u UUR GELEDEN"
#define FAMILY_STR_DAYS_AGO "%u DAGEN GELEDEN"

// send (family_font_16 bar, family_font_20 text, family_font_14 key hints)
// first-start welcome (FamilyScreen.cpp, "welcome")
#define FAMILY_STR_WELCOME_POSITION "%d / %d"
#define FAMILY_STR_WELCOME_NEXT "VERDER"
#define FAMILY_STR_WELCOME_SKIP "OVERSLAAN"
#define FAMILY_STR_WELCOME_HELLO "ZEG HALLO"
#define FAMILY_STR_WELCOME_T1 "WELKOM"
#define FAMILY_STR_WELCOME_B1 "Dit apparaat heet:" // one line in the column beside the model
#define FAMILY_STR_WELCOME_A1 "Zo zien de anderen jouw berichten."
#define FAMILY_STR_WELCOME_T2 "LEZEN"
#define FAMILY_STR_WELCOME_B2 "Op het beginscherm zie je of er nieuwe berichten zijn. Tik op LEZEN, veeg om te bladeren, en tik op TERUG om terug te gaan."
#define FAMILY_STR_WELCOME_T3 "STUREN"
#define FAMILY_STR_WELCOME_B3 "Tik op BERICHT STUREN, typ je bericht en tik op VERSTUREN. Het gaat naar de hele familie."
#define FAMILY_STR_WELCOME_T4 "SLAPEN"
#define FAMILY_STR_WELCOME_B4_MIN "Na %u %s gaat het scherm uit. Druk op de trackbal, het balletje onder het scherm, om het weer aan te zetten. Een nieuw bericht zet het vanzelf aan."
#define FAMILY_STR_WELCOME_B4 "Gaat het scherm uit, druk dan op de trackbal, het balletje onder het scherm. Een nieuw bericht zet het vanzelf aan."
#define FAMILY_STR_MINUTE "minuut"
#define FAMILY_STR_MINUTES "minuten"
#define FAMILY_STR_WELCOME_T5 "ZEG HALLO"
#define FAMILY_STR_WELCOME_B5 "Dit gaat naar de hele familie:" // one line: the card below needs the room
#define FAMILY_STR_WELCOME_MESSAGE "Hallo vanaf %s! Aangesloten op VOXLnet."
#define FAMILY_STR_WELCOME_REPLAY "VOXL welcome tour"
#define FAMILY_STR_SEND_TITLE "AAN IEDEREEN" // after TERUG; the home row already says BERICHT STUREN
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

// boot animation, the status block under the radio pulse (family_font_16)
#define FAMILY_STR_BOOT_CONNECTING "VERBINDEN"
#define FAMILY_STR_BOOT_LOADING "BERICHTEN LADEN"
#define FAMILY_STR_BOOT_READY "KLAAR"

// MUI's top bar on the home panel: the VOXL wordmark, then this
#define FAMILY_STR_BRAND_SUFFIX "net"

#endif
