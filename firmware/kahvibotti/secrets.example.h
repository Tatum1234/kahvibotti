// Private settings. Copy this file to secrets.h (same folder), fill it in, and never publish secrets.h
// (it's in .gitignore). If the bot token ever leaks: @BotFather -> /revoke -> paste the new token here and
// flash again.
#pragma once

// The office Wi-Fi (the built-in network; up to 3 more can be added from the admin panel).
#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// From @BotFather (/newbot). Looks like 123456789:AAE...
#define BOT_TOKEN     "123456789:replace-with-your-bot-token"

// The owner's admin panel login (http://emukahvibotti.local) AND the password of the
// "EmuKahviBottiHotspot" hotspot that opens after 5 min without Wi-Fi. 8+ characters.
// This account can't be changed or removed from the panel: it's the fallback if the panel's own admin
// account is forgotten.
#define ADMIN_USER     "admin"
#define ADMIN_PASSWORD "change-me-please"

// Telegram group for milestone and test messages (can be changed later on the Huolto page).
// Group IDs are negative, e.g. -1001234567890.
#define GROUP_CHAT_ID "-1001234567890"
