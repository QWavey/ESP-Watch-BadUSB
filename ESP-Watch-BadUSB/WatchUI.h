#ifndef WATCH_UI_H
#define WATCH_UI_H

// WatchUI — the AMOLED front panel for the Watch-BadUSB port. Replaces the
// hardware LED status of the Key with an on-screen LED dot + status + AP
// creds + STOP button + settings modal.

#include <Arduino.h>
#include <stdint.h>

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
