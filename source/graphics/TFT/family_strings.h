#pragma once

// Dutch labels of the family screen, in one place.
// Strings drawn in family_font_28 must stay inside 0x20-0x7F and 0xA0-0x17F (no "…").

#if defined(FAMILY_UI) && defined(VIEW_320x240)

// home rows (family_font_28)
#define FAMILY_STR_MESSAGES "Berichten (%u)"
#define FAMILY_STR_MESSAGES_LOADING "Berichten..."
#define FAMILY_STR_READ "Lezen"
#define FAMILY_STR_SEND "Bericht sturen"

// read (montserrat 16/20)
#define FAMILY_STR_READ_HINT "Rol om te lezen. Druk om terug te gaan."
#define FAMILY_STR_LOADING "Berichten laden…"
#define FAMILY_STR_NO_MESSAGES "Nog geen berichten."
#define FAMILY_STR_ME "Ik"
#define FAMILY_STR_PRIVATE "privé"
#define FAMILY_STR_SENT "Verstuurd"
#define FAMILY_STR_NOT_SENT "Niet verstuurd"
#define FAMILY_STR_JUST_NOW "zojuist"
#define FAMILY_STR_MINUTES_AGO "%u min geleden"
#define FAMILY_STR_HOURS_AGO "%u uur geleden"
#define FAMILY_STR_DAYS_AGO "%u dagen geleden"

// send (montserrat 16/20)
#define FAMILY_STR_SEND_TITLE "Bericht aan iedereen"
#define FAMILY_STR_SEND_HINT "Enter of druk: versturen. Wis: terug."

// status line, mirrored from MUI alerts (montserrat 16)
#define FAMILY_STR_ALERT_DISCONNECTED "Geen verbinding"
#define FAMILY_STR_ALERT_CONNECTED "Verbonden"
#define FAMILY_STR_ALERT_REBOOTING "Herstarten…"
#define FAMILY_STR_ALERT_RESYNC "Bijwerken…"
#define FAMILY_STR_ALERT_SHUTDOWN "Uitschakelen…"

#endif
