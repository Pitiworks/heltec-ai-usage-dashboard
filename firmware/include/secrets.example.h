#pragma once

// ---------------------------------------------------------------------
// Template for local WiFi and dashboard-server credentials.
//
// Copy this file to "secrets.h" and fill in real values. "secrets.h"
// itself is git-ignored and must never be committed.
// ---------------------------------------------------------------------

#define WIFI_SSID       "CHANGE_ME_WIFI_SSID"
#define WIFI_PASSWORD   "CHANGE_ME_WIFI_PASSWORD"

// LAN IP or hostname of the Ubuntu usage-dashboard server
#define DASHBOARD_HOST  "CHANGE_ME_DASHBOARD_IP"   // e.g. "192.168.1.50"
#define DASHBOARD_PORT  8791
