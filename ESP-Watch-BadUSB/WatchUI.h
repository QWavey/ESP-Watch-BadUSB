#ifndef WATCH_UI_H
#define WATCH_UI_H

// WatchUI — the AMOLED front panel for the Watch-BadUSB port. Replaces the
// hardware LED status of the Key with an on-screen LED dot + status + AP
// creds + STOP button + settings modal.

#include <Arduino.h>
#include <stdint.h>
#include <vector>

void watchUiBegin();
void watchUiTick();

void watchUiSetLed(uint32_t rgb, uint16_t blinkMs = 0);
void watchUiFlash(const char* text);
void watchUiSetStatus(const char* text);
void watchUiSetScript(const char* name);
void watchUiSetClients(int n);
void watchUiSetAP(const char* ssid, const char* password, const char* ip);
void watchUiSetProgress(int percent);

bool watchUiConsumeStopPressed();

// ---- Files-tab bridge ----
// The Files tab lists /scripts on the SD card. Tapping the green ▶ button on
// a row queues a run action; tapping the red 🗑 queues a delete. The main
// loop consumes these via watchUiConsumePendingScriptAction() so LVGL never
// blocks on SD I/O.
struct WatchUiPendingScriptAction {
    bool has_run;      char run_name[64];
    bool has_delete;   char delete_name[64];
};
WatchUiPendingScriptAction watchUiConsumePendingScriptAction();

// Feed the list to the UI. Pass an ordered vector of script basenames.
void watchUiSetFileList(const std::vector<String>& names);

// Show a transient banner across the whole home tab: SD inserted / removed,
// power-off countdown, etc. Pass nullptr to hide.
void watchUiSetBanner(const char* text, uint32_t rgb = 0x2A2A30);

// ---- settings-tab bridges ----
struct WatchUiPendingSettings {
    bool has_wifi;          bool wifi_on;
    bool has_bt;            bool bt_on;
    bool has_btdisc;        bool btdisc_on;
    bool has_led;           bool led_on;
    bool has_silent;        bool silent_on;
    bool has_logging;       bool logging_on;
    bool has_com;           bool com_on;
    bool want_reboot;
    bool want_factory_reset;
};

WatchUiPendingSettings watchUiConsumePendingSettings();
void watchUiRefreshSettings(bool wifi, bool bt, bool btdisc,
                            bool led,  bool silent, bool logging, bool com);

typedef void (*WatchUiWalkthroughDone)();
void watchUiShowWalkthrough(const char* ssid, const char* psk, const char* ip,
                            WatchUiWalkthroughDone onDoneCb);
bool watchUiWalkthroughActive();

#endif // WATCH_UI_H
