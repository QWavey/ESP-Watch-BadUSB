#include "WatchUI.h"
#include "pin_config.h"

// Screen + touch classes come from the QWavey/ESP32-S3-Waveshare-OLED-Watch
// library. They must be installed alongside their transitive deps
// (Arduino_GFX, Arduino_DriveBus, LVGL 9, XPowersLib). See the README.
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <lvgl.h>
#include <XPowersLib.h>
#include <string>

// Global instances the extern declarations in those headers point at.
ScreenClass  Screen;
TouchClass   Touch;
static XPowersPMU s_pmu;
static bool       s_pmuReady = false;

// ---- Home-tab LVGL objects the setters mutate ------------------------------
static lv_obj_t* s_tabHome    = nullptr;
static lv_obj_t* s_tabFiles   = nullptr;
static lv_obj_t* s_tabSet     = nullptr;
static lv_obj_t* s_statusLbl  = nullptr;
static lv_obj_t* s_scriptLbl  = nullptr;
static lv_obj_t* s_progress   = nullptr;
static lv_obj_t* s_apSsidLbl  = nullptr;
static lv_obj_t* s_apPassLbl  = nullptr;
static lv_obj_t* s_ipLbl      = nullptr;
static lv_obj_t* s_clientsLbl = nullptr;
static lv_obj_t* s_battLbl    = nullptr;
static lv_obj_t* s_flashLbl   = nullptr;
static lv_obj_t* s_bannerLbl  = nullptr;
static lv_obj_t* s_stopBtn    = nullptr;
static lv_obj_t* s_filesList  = nullptr;   // scroll container inside the Files tab
static lv_obj_t* s_filesEmpty = nullptr;   // "no scripts yet" placeholder

// ---- Settings-tab switch handles (indexed by SETTING_*) -------------------
enum SettingIdx {
    SET_WIFI = 0, SET_BT, SET_BTDISC, SET_LED,
    SET_SILENT, SET_LOGGING, SET_COM, SET_AUTOSTART,
    SET_COUNT
};
static lv_obj_t* s_settingSw[SET_COUNT] = { nullptr };

// ---- state -----------------------------------------------------------------
static uint32_t      s_ledColor       = 0x00FF00;   // green idle default
static uint16_t      s_blinkMs        = 0;
static unsigned long s_lastBlinkTick  = 0;
static bool          s_blinkOn        = true;
static bool          s_stopEdge       = false;
static unsigned long s_flashUntil     = 0;
static unsigned long s_lastLvglTick   = 0;
static unsigned long s_lastBattPoll   = 0;
static unsigned long s_lastPMUProbe   = 0;
static WatchUiPendingSettings s_pending = {};
static WatchUiPendingScriptAction s_pendingAction = {};
static WatchUiPendingExtras s_pendingExtras = {};
static String s_currentAutostart;   // "" = none; used for star highlight
static uint32_t s_bannerUntil = 0;

// Clock overlay + power-hold + screen-sleep state
static lv_obj_t* s_clockRoot   = nullptr;
static lv_obj_t* s_clockTime   = nullptr;   // large HH:MM
static lv_obj_t* s_clockHint   = nullptr;   // "swipe down to reveal"
static lv_obj_t* s_tabView     = nullptr;   // captured for swipe-up handler
static bool      s_powerHold   = false;
static bool      s_screenAsleep = false;
extern class ScreenClass Screen;            // forward-declared instance in header

// ---- design tokens ---------------------------------------------------------
// Restrained palette — one neutral ground, one accent, one danger. Nothing
// glows. Nothing gradients. Same colour discipline the hub SVGs use.
static const uint32_t C_BG        = 0x0A0A0B;
static const uint32_t C_SURFACE   = 0x141416;
static const uint32_t C_LINE      = 0x2A2A30;
static const uint32_t C_TEXT      = 0xE8E8EA;
static const uint32_t C_MUTED     = 0x8A8A90;
static const uint32_t C_ACCENT    = 0x3AA7F5;   // subdued blue, not neon
static const uint32_t C_OK        = 0x30D158;
static const uint32_t C_DANGER    = 0xC8442D;
static const uint32_t C_LED_OFF   = 0x101010;

static const int LCD_W = LCD_WIDTH;
static const int LCD_H = LCD_HEIGHT;

static lv_color_t lvhex(uint32_t rgb) { return lv_color_hex(rgb & 0xFFFFFF); }

// ---- helpers ---------------------------------------------------------------
static void stopBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) s_stopEdge = true;
}

// R1-B: XPowersPMU is a typedef for XPowersAXP2101 under XPOWERS_CHIP_AXP2101.
// Its 4-arg begin() is the only one that compiles; the 2-arg overload does
// not exist. Wire is already up (TouchClass::on ran Wire.begin(IIC_SDA,
// IIC_SCL)) so passing the pins again is idempotent.
// R4-A: use a hasTried flag so the first call actually attempts init instead
// of short-circuiting because millis()-0 < 2000.
static void tryInitPMU() {
    if (s_pmuReady) return;
    static bool hasTried = false;
    if (hasTried && millis() - s_lastPMUProbe < 2000) return;
    hasTried = true;
    s_lastPMUProbe = millis();
    if (s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        s_pmuReady = true;
        s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        s_pmu.clearIrqStatus();
        // Watch port: keep the power-key IRQs latched so we can poll the
        // status register in watchUiTick and drive a hold-to-power-off
        // countdown. We NEVER wire the AXP2101's INT pin to a GPIO here —
        // polling the latch is enough at 100 ms cadence.
        s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
                        XPOWERS_AXP2101_PKEY_POSITIVE_IRQ |
                        XPOWERS_AXP2101_PKEY_LONG_IRQ);
        s_pmu.enableBattDetection();
        s_pmu.enableBattVoltageMeasure();
        s_pmu.enableSystemVoltageMeasure();
        s_pmu.enableVbusVoltageMeasure();
        s_pmu.enableTemperatureMeasure();
    }
}

// One consistent row: [icon] [label ..... grow .....] [switch]. Used for
// every switch on the Settings tab so the tab reads as one thing.
static lv_obj_t* makeSwitchRow(lv_obj_t* parent, const char* icon,
                               const char* label, bool initial,
                               lv_event_cb_t cb, int idx) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W, 60);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 30);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_color(sw, lvhex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    // Lag fix: LVGL animates the knob slide over 300 ms by default. On a
    // 410×502 QSPI AMOLED the partial-redraw of that animation is what
    // makes a tap feel like it takes half a second to register. Zero it.
    lv_obj_set_style_anim_duration(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(sw, 0, LV_PART_KNOB);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    // Lag fix: strip the row's press-highlight state so tapping anywhere
    // else on the row doesn't queue a full-row redraw for the darken pass.
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);
    if (idx >= 0 && idx < SET_COUNT) s_settingSw[idx] = sw;
    return row;
}

// Same row shell but the trailing element is a button — used for Reboot /
// Factory Reset. Danger flag turns the button red.
static lv_obj_t* makeActionRow(lv_obj_t* parent, const char* icon,
                               const char* label, const char* btnText,
                               bool danger, lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W, 70);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(danger ? C_DANGER : C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 30);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_set_size(btn, 130, 46);
    lv_obj_set_style_bg_color(btn, lvhex(danger ? C_DANGER : C_ACCENT), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bt = lv_label_create(btn);
    lv_label_set_text(bt, btnText);
    lv_obj_set_style_text_color(bt, lv_color_white(), 0);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_18, 0);
    lv_obj_center(bt);
    return row;
}

// ---- settings switch event handler ----------------------------------------
// Runs on the LVGL task. We NEVER touch WiFi/BT/Preferences from here — that
// would race with the main firmware task. Just record the intent; loop()
// picks it up via watchUiConsumePendingSettings().
static void settingSwitchCb(lv_event_t* e) {
    lv_obj_t* sw   = (lv_obj_t*)lv_event_get_target(e);
    int idx        = (int)(intptr_t)lv_event_get_user_data(e);
    bool on        = lv_obj_has_state(sw, LV_STATE_CHECKED);
    switch (idx) {
        case SET_WIFI:      s_pending.has_wifi    = true; s_pending.wifi_on    = on; break;
        case SET_BT:        s_pending.has_bt      = true; s_pending.bt_on      = on; break;
        case SET_BTDISC:    s_pending.has_btdisc  = true; s_pending.btdisc_on  = on; break;
        case SET_LED:       s_pending.has_led     = true; s_pending.led_on     = on; break;
        case SET_SILENT:    s_pending.has_silent  = true; s_pending.silent_on  = on; break;
        case SET_LOGGING:   s_pending.has_logging = true; s_pending.logging_on = on; break;
        case SET_COM:       s_pending.has_com     = true; s_pending.com_on     = on; break;
        case SET_AUTOSTART: s_pendingExtras.has_autostart = true;
                            s_pendingExtras.autostart_on  = on; break;
    }
}
static void rebootBtnCb(lv_event_t*)         { s_pending.want_reboot        = true; }
static void factoryResetBtnCb(lv_event_t*)   { s_pending.want_factory_reset = true; }
static void resetStdBtnCb(lv_event_t*)       { s_pendingExtras.want_reset_std = true; }
static void brickBtnCb(lv_event_t*)          { s_pendingExtras.want_brick     = true; }

// ---- tab builders ---------------------------------------------------------
static void buildHomeTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    // Status + battery. The animated LED "circle that turns colors while a
    // script runs" is gone — status label carries the same information as
    // text, without a repainting circle on every LVGL tick.
    s_statusLbl = lv_label_create(tab);
    lv_label_set_text(s_statusLbl, "Booting...");
    lv_obj_set_style_text_color(s_statusLbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_statusLbl, &lv_font_montserrat_22, 0);
    lv_obj_align(s_statusLbl, LV_ALIGN_TOP_LEFT, 0, 6);

    s_battLbl = lv_label_create(tab);
    lv_label_set_text(s_battLbl, LV_SYMBOL_BATTERY_FULL " -- %");
    lv_obj_set_style_text_color(s_battLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_battLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_battLbl, LV_ALIGN_TOP_RIGHT, 0, 8);

    // Script + progress
    s_scriptLbl = lv_label_create(tab);
    lv_label_set_text(s_scriptLbl, LV_SYMBOL_FILE "  no script");
    lv_obj_set_style_text_color(s_scriptLbl, lvhex(C_OK), 0);
    lv_obj_set_style_text_font(s_scriptLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_scriptLbl, LCD_W - 24);
    lv_label_set_long_mode(s_scriptLbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_scriptLbl, LV_ALIGN_TOP_LEFT, 0, 82);

    s_progress = lv_bar_create(tab);
    lv_obj_set_size(s_progress, LCD_W - 24, 8);
    lv_obj_align(s_progress, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_bar_set_range(s_progress, 0, 100);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_color(s_progress, lvhex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress, 4, 0);
    lv_obj_set_style_radius(s_progress, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);

    // AP block — a subtle card so the creds read as a group
    lv_obj_t* apCard = lv_obj_create(tab);
    lv_obj_remove_style_all(apCard);
    lv_obj_set_size(apCard, LCD_W - 24, 118);
    lv_obj_align(apCard, LV_ALIGN_TOP_LEFT, 0, 134);
    lv_obj_set_style_bg_color(apCard, lvhex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(apCard, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(apCard, 12, 0);
    lv_obj_set_style_pad_all(apCard, 12, 0);
    lv_obj_clear_flag(apCard, LV_OBJ_FLAG_SCROLLABLE);

    s_apSsidLbl = lv_label_create(apCard);
    lv_label_set_text(s_apSsidLbl, LV_SYMBOL_WIFI "  AP: (starting)");
    lv_obj_set_style_text_color(s_apSsidLbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_apSsidLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_apSsidLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_apPassLbl = lv_label_create(apCard);
    lv_label_set_text(s_apPassLbl, "PSK: ...");
    lv_obj_set_style_text_color(s_apPassLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_apPassLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_apPassLbl, LV_ALIGN_TOP_LEFT, 0, 24);

    s_ipLbl = lv_label_create(apCard);
    lv_label_set_text(s_ipLbl, "http://192.168.4.1");
    lv_obj_set_style_text_color(s_ipLbl, lvhex(C_ACCENT), 0);
    lv_obj_set_style_text_font(s_ipLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(s_ipLbl, LV_ALIGN_TOP_LEFT, 0, 50);

    s_clientsLbl = lv_label_create(apCard);
    lv_label_set_text(s_clientsLbl, LV_SYMBOL_USB "  0 clients");
    lv_obj_set_style_text_color(s_clientsLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_clientsLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_clientsLbl, LV_ALIGN_TOP_LEFT, 0, 80);

    // Banner moved out of buildHomeTab — it now lives on lv_layer_top()
    // (created in watchUiBegin) so the power-off countdown / SD hot-plug
    // toast floats above Home, Files, Settings and the clock overlay.

    // Toast overlay (hidden until watchUiFlash). Bug #22: sit ABOVE the STOP
    // button (74 tall, bottom -10) with generous space above so the toast
    // doesn't get clipped by tall multi-line messages either.
    s_flashLbl = lv_label_create(tab);
    lv_obj_set_style_text_color(s_flashLbl, lvhex(0xFFB020), 0);
    lv_obj_set_style_text_font(s_flashLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_flashLbl, lvhex(0x201510), 0);
    lv_obj_set_style_bg_opa(s_flashLbl, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_flashLbl, 10, 0);
    lv_obj_set_style_radius(s_flashLbl, 8, 0);
    lv_obj_set_width(s_flashLbl, LCD_W - 40);
    lv_label_set_long_mode(s_flashLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_flashLbl, "");
    lv_obj_align(s_flashLbl, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_add_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);

    // STOP button — full width along the bottom
    s_stopBtn = lv_btn_create(tab);
    lv_obj_set_size(s_stopBtn, LCD_W - 24, 74);
    lv_obj_align(s_stopBtn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_stopBtn, lvhex(C_DANGER), 0);
    lv_obj_set_style_radius(s_stopBtn, 12, 0);
    lv_obj_set_style_shadow_width(s_stopBtn, 0, 0);
    lv_obj_add_event_cb(s_stopBtn, stopBtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* stopLbl = lv_label_create(s_stopBtn);
    lv_label_set_text(stopLbl, LV_SYMBOL_STOP "  STOP SCRIPT");
    lv_obj_set_style_text_color(stopLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stopLbl, &lv_font_montserrat_22, 0);
    lv_obj_center(stopLbl);
}

// ---- Files tab -------------------------------------------------------------
// Rows are [ name (grows) | ▶ green | 🗑 red ]. Callbacks record the intent
// on s_pendingAction; the main loop consumes it and runs the actual SD /
// interpreter work off the LVGL task.
static void filesRunCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_run = true;
    strncpy(s_pendingAction.run_name, name, sizeof(s_pendingAction.run_name) - 1);
    s_pendingAction.run_name[sizeof(s_pendingAction.run_name) - 1] = '\0';
}
static void filesDelCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_delete = true;
    strncpy(s_pendingAction.delete_name, name, sizeof(s_pendingAction.delete_name) - 1);
    s_pendingAction.delete_name[sizeof(s_pendingAction.delete_name) - 1] = '\0';
}
static void filesStarCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_toggle_autostart = true;
    // Pressing the star on the currently-armed script clears autostart;
    // pressing on any other row makes THAT the new autostart target.
    if (s_currentAutostart == String(name)) {
        s_pendingAction.autostart_name[0] = '\0';
    } else {
        strncpy(s_pendingAction.autostart_name, name,
                sizeof(s_pendingAction.autostart_name) - 1);
        s_pendingAction.autostart_name[sizeof(s_pendingAction.autostart_name) - 1] = '\0';
    }
}

static void buildFilesTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    s_filesList = lv_obj_create(tab);
    lv_obj_remove_style_all(s_filesList);
    lv_obj_set_size(s_filesList, LCD_W, LV_PCT(100));
    lv_obj_set_style_pad_all(s_filesList, 0, 0);
    lv_obj_set_style_bg_color(s_filesList, lvhex(C_BG), 0);
    lv_obj_set_flex_flow(s_filesList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_filesList, LV_FLEX_ALIGN_START,
                                       LV_FLEX_ALIGN_START,
                                       LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_filesList, LV_DIR_VER);
    lv_obj_set_style_pad_gap(s_filesList, 0, 0);
    lv_obj_set_scrollbar_mode(s_filesList, LV_SCROLLBAR_MODE_OFF);

    s_filesEmpty = lv_label_create(tab);
    lv_label_set_text(s_filesEmpty,
        LV_SYMBOL_FILE "  No scripts yet\n"
        "Drop .txt files in /scripts on the SD card.");
    lv_obj_set_style_text_color(s_filesEmpty, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_filesEmpty, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_filesEmpty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_filesEmpty, LCD_W - 40);
    lv_label_set_long_mode(s_filesEmpty, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_filesEmpty, LV_ALIGN_CENTER, 0, 0);
}

// Store row-name strings so callbacks can point at stable memory across
// rebuilds. LVGL user_data is a raw pointer, so we can't hand it a String
// stack copy — this vector outlives each row until the next rebuild frees it.
static std::vector<std::string> s_fileRowNames;

// ---- settings tab ---------------------------------------------------------
static void buildSettingsTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    // Left-align rows on the cross axis; centering + LV_PCT(100) rows lets
    // LVGL round widths up by 1 px per row on some builds and the whole
    // container ends up wider than the viewport — visible as the tab
    // "getting wide" the moment it's selected. Also lock scroll to vertical
    // only so touch-drag on a row can't nudge the tab sideways.
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_style_pad_gap(tab, 0, 0);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);   // /improve: clean surface

    // Section: Radios
    makeSwitchRow(tab, LV_SYMBOL_WIFI,      "WiFi AP",         true, settingSwitchCb, SET_WIFI);
    makeSwitchRow(tab, LV_SYMBOL_BLUETOOTH, "Bluetooth",       false, settingSwitchCb, SET_BT);
    makeSwitchRow(tab, LV_SYMBOL_EYE_OPEN,  "BT discovery",    false, settingSwitchCb, SET_BTDISC);

    // Section: Behaviour
    // Watch port: NO status LED on this board (no hardware LED, no RGB
    // pixel). The "Status LED" row is a stale carry-over from the Key.
    // Row omitted; LED_PIN in Config.h is only kept for the LEDManager
    // shim compile-compat. Firmware reports hidden_settings=["led"] via
    // /api/stats so the web UI can hide its toggle too.
    // makeSwitchRow(tab, LV_SYMBOL_POWER, "Status LED", true, settingSwitchCb, SET_LED);
    // Silent startup off by default — a fresh flash should show the wearer
    // that USB HID is up, not hide it. Users can still opt in.
    makeSwitchRow(tab, LV_SYMBOL_EYE_CLOSE, "Silent startup",  false, settingSwitchCb, SET_SILENT);
    makeSwitchRow(tab, LV_SYMBOL_LIST,      "Log to SD",       false, settingSwitchCb, SET_LOGGING);
    makeSwitchRow(tab, LV_SYMBOL_USB,       "COM shell (CDC)", false, settingSwitchCb, SET_COM);
    // Autostart: gates whether the persisted boot_script runs at boot.
    // Independent from Files-tab star, which sets *which* script is queued.
    makeSwitchRow(tab, LV_SYMBOL_PLAY,       "Autostart at boot", false, settingSwitchCb, SET_AUTOSTART);

    // Section: Actions
    makeActionRow(tab, LV_SYMBOL_REFRESH, "Reboot",            "REBOOT", false, rebootBtnCb);
    makeActionRow(tab, LV_SYMBOL_LOOP,    "Reset to standard", "RESET",  false, resetStdBtnCb);
    makeActionRow(tab, LV_SYMBOL_TRASH,   "Factory reset",     "WIPE",   true,  factoryResetBtnCb);
    makeActionRow(tab, LV_SYMBOL_WARNING, "Brick firmware",    "BRICK",  true,  brickBtnCb);

    // Footer version stamp
    lv_obj_t* v = lv_label_create(tab);
    lv_label_set_text(v, "ESP-Watch-BadUSB   ·   watch-web-1");
    lv_obj_set_style_text_color(v, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(v, 14, 0);
}

// ---- public API ------------------------------------------------------------
void watchUiBegin() {
    static bool inited = false;
    if (inited) return;   // R15: guard against a second call re-adding widgets
    inited = true;
    Serial.println("[WatchUI] Bringing up display + touch");
    Screen.on();
    Touch.on();
    tryInitPMU();

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // Tabview: two tabs, big enough tap targets for a wrist screen.
    lv_obj_t* tv = lv_tabview_create(scr);
    s_tabView = tv;
    lv_obj_set_size(tv, LCD_W, LCD_H);
    lv_tabview_set_tab_bar_size(tv, 56);
    lv_obj_set_style_bg_color(tv, lvhex(C_BG), 0);

    // Style the tab bar to match the palette (no gradient, subtle line).
    // Fix "stretched settings tab": LVGL's default tab-btn checked style
    // gives the active tab a wider background pill than the inactive one.
    // Style them identically (only text colour + a bottom underline change
    // on select) and the two pills stop dancing sideways.
    lv_obj_t* tabBar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tabBar, lvhex(C_SURFACE), 0);
    lv_obj_set_style_border_side(tabBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(tabBar, 1, 0);
    lv_obj_set_style_border_color(tabBar, lvhex(C_LINE), 0);
    lv_obj_set_style_text_color(tabBar, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_color(tabBar, lvhex(C_TEXT),  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tabBar, &lv_font_montserrat_18, 0);
    // Kill the tab button's own background chip so the active tab doesn't
    // look wider than the inactive one; add a subtle bottom underline on
    // the selected tab instead.
    lv_obj_set_style_bg_opa(tabBar, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(tabBar, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side (tabBar, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tabBar, 3, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tabBar, lvhex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    // "Tab stretched like italic font" — after v1 (transform_scale only) still
    // reproduced on the Files tab, the real culprit is the default theme
    // widening the checked tab-item via BIGGER text_letter_space + a slightly
    // heavier text_font. Force both to the same fixed values across every
    // state on every part of the tab-button, and lock its horizontal padding
    // so the button's own layout can't change size on select.
    {
      const auto pin = [&](lv_state_t st) {
        lv_style_selector_t sel = LV_PART_ITEMS | st;
        lv_obj_set_style_transform_scale_x(tabBar, 256, sel);
        lv_obj_set_style_transform_scale_y(tabBar, 256, sel);
        lv_obj_set_style_text_letter_space(tabBar, 0, sel);
        lv_obj_set_style_text_font(tabBar, &lv_font_montserrat_18, sel);
        lv_obj_set_style_pad_hor(tabBar, 12, sel);
        lv_obj_set_style_pad_ver(tabBar, 0, sel);
        lv_obj_set_style_anim_duration(tabBar, 0, sel);
      };
      pin((lv_state_t)LV_STATE_DEFAULT);
      pin((lv_state_t)LV_STATE_CHECKED);
      pin((lv_state_t)LV_STATE_PRESSED);
      pin((lv_state_t)(LV_STATE_CHECKED | LV_STATE_PRESSED));
      pin((lv_state_t)LV_STATE_FOCUS_KEY);
      pin((lv_state_t)LV_STATE_FOCUSED);
    }
    // Disable tab-switch slide animation — feels laggy on QSPI AMOLED.
    lv_obj_set_style_anim_duration(tv, 0, 0);

    // Swipe-up on the tabview brings the clock back. LVGL emits a
    // LV_EVENT_GESTURE with LV_DIR_TOP when the user drags upward.
    lv_obj_add_event_cb(tv, [](lv_event_t* e){
        if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if (d == LV_DIR_TOP) watchUiShowClock();
    }, LV_EVENT_GESTURE, nullptr);

    s_tabHome  = lv_tabview_add_tab(tv, LV_SYMBOL_HOME     "  Home");
    s_tabFiles = lv_tabview_add_tab(tv, LV_SYMBOL_FILE     "  Files");
    s_tabSet   = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS "  Settings");
    buildHomeTab(s_tabHome);
    buildFilesTab(s_tabFiles);
    buildSettingsTab(s_tabSet);

    // Banner on lv_layer_top() — floats above every tab and the clock. See
    // watchUiSetBanner() which move-foregrounds it on each show so the
    // clock overlay never occludes the countdown / hot-plug toast.
    s_bannerLbl = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(s_bannerLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_bannerLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(s_bannerLbl, 0, 0);
    lv_obj_set_style_bg_color(s_bannerLbl, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_opa(s_bannerLbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_bannerLbl, 10, 0);
    lv_obj_set_style_radius(s_bannerLbl, 8, 0);
    lv_obj_set_width(s_bannerLbl, LCD_W - 40);
    lv_obj_set_style_text_align(s_bannerLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_bannerLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_bannerLbl, "");
    lv_obj_align(s_bannerLbl, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);

    // Swipe-UP anywhere on the screen brings the clock back. The tabview's
    // own gesture handler is one place; attaching a screen-level fallback
    // too catches gestures inside scrollable tab-content that would
    // otherwise be consumed by the scroll.
    lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t* e){
        if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if (d == LV_DIR_TOP && !watchUiClockVisible()) watchUiShowClock();
    }, LV_EVENT_GESTURE, nullptr);

    // Clock face — shown at boot; swipe down to reveal the tabview.
    watchUiShowClock();

    Serial.println("[WatchUI] Ready");
}

// ---- Clock overlay ---------------------------------------------------------
static void clockGestureCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_BOTTOM) watchUiHideClock();   // "swipe down to reveal"
    // Tapping the clock also dismisses — most users try that first.
}
static void clockClickCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    watchUiHideClock();
}

void watchUiShowClock() {
    if (s_clockRoot) {
        lv_obj_clear_flag(s_clockRoot, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    s_clockRoot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_clockRoot);
    lv_obj_set_size(s_clockRoot, LCD_W, LCD_H);
    lv_obj_set_pos(s_clockRoot, 0, 0);
    lv_obj_set_style_bg_color(s_clockRoot, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_clockRoot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_clockRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_clockRoot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_clockRoot, clockGestureCb, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(s_clockRoot, clockClickCb,   LV_EVENT_CLICKED, nullptr);

    s_clockTime = lv_label_create(s_clockRoot);
    lv_label_set_text(s_clockTime, "00:00");
    lv_obj_set_style_text_color(s_clockTime, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clockTime, &lv_font_montserrat_48, 0);
    // "Clock stretches as time goes up" fix: LV_SIZE_CONTENT (the default)
    // makes the label re-flow to fit each new HH:MM string. Every rewrite
    // triggers a layout pass that briefly enlarges/shrinks the widget's
    // bounding box — visible as horizontal stretch. Pin an explicit width
    // that fits the widest possible HH:MM ("00:00"-"23:59") and center
    // text INSIDE that fixed frame. Height also fixed so vertical layout
    // doesn't dance either.
    lv_obj_set_size(s_clockTime, LCD_W - 40, 80);
    lv_obj_set_style_text_align(s_clockTime, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale_x(s_clockTime, 256, 0);
    lv_obj_set_style_transform_scale_y(s_clockTime, 256, 0);
    lv_obj_set_style_text_letter_space(s_clockTime, 0, 0);
    lv_obj_center(s_clockTime);
    // Hint text removed per user request — clock face stays clean.
    s_clockHint = nullptr;
}

void watchUiHideClock() {
    if (!s_clockRoot) return;
    lv_obj_del(s_clockRoot);
    s_clockRoot = nullptr;
    s_clockTime = nullptr;
    s_clockHint = nullptr;
}

bool watchUiClockVisible() {
    return s_clockRoot != nullptr;
}

void watchUiSetClockSeconds(uint32_t seconds) {
    if (!s_clockTime) return;
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds / 60) % 60;
    static char last[8] = "";
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h, (unsigned)m);
    if (strcmp(last, buf) == 0) return;
    strncpy(last, buf, sizeof(last));
    lv_label_set_text(s_clockTime, buf);
}

// ---- Power-off long-press countdown ---------------------------------------
void watchUiBeginPowerHold() {
    s_powerHold = true;
    watchUiSetBanner(LV_SYMBOL_POWER "  Hold to power off (4)", 0xC8442D);
}
void watchUiSetPowerHoldRemaining(int seconds) {
    if (!s_powerHold) return;
    if (seconds <= 0) {
        watchUiSetBanner(LV_SYMBOL_POWER "  Powering off...", 0xC8442D);
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_POWER "  Hold %d s to power off", seconds);
    watchUiSetBanner(buf, 0xC8442D);
}
void watchUiEndPowerHold() {
    if (!s_powerHold) return;
    s_powerHold = false;
    watchUiSetBanner(nullptr);
}

// ---- Screen sleep ---------------------------------------------------------
// AMOLED: painting black = pixels dark = ~0 power. Screen.off() blanks the
// framebuffer. We also freeze LVGL's redraw so wake latency is a single flush.
void watchUiScreenSleep() {
    if (s_screenAsleep) return;
    s_screenAsleep = true;
    Screen.off();
}
void watchUiScreenWake() {
    if (!s_screenAsleep) return;
    s_screenAsleep = false;
    // Force a full repaint so the wake tap doesn't stare at a black screen.
    lv_obj_invalidate(lv_scr_act());
    lv_obj_t* top = lv_layer_top();
    if (top) lv_obj_invalidate(top);
    Screen.on();
}
bool watchUiScreenAsleep() { return s_screenAsleep; }

void watchUiTick() {
    unsigned long now = millis();
    if (now - s_lastLvglTick >= 5) {
        lv_tick_inc(now - s_lastLvglTick);
        s_lastLvglTick = now;
    }
    // Watch port perf: rate-limit lv_task_handler to 10 ms. loop() runs at
    // ~1 kHz; every LVGL scheduler pass does invalidate/scan/animation work
    // regardless of dirty state. 10 ms is finer than LVGL's own 33 ms
    // refresh — no visual difference, ~10× less CPU wasted here, which
    // gives touch/server/USB more headroom. The old forced full-screen
    // invalidate every 300 ms is gone — it caused a repaint storm and was
    // only added as a diagnostic that never proved anything.
    // Watch port perf: rate-limit lv_task_handler to 5 ms so touch tracking
    // stays sharp — LVGL's own scheduler still respects LV_DEF_REFR_PERIOD.
    // The pass early-outs cheaply when nothing is dirty (~50 µs), so 5 ms
    // is nearly free compared to the old 10 ms cap.
    static unsigned long s_lastHandler = 0;
    if (now - s_lastHandler < 5) return;
    s_lastHandler = now;
    lv_task_handler();

    // LED dot removed from the home tab — the status label carries the same
    // information as text without a repainting circle every tick. Keep the
    // colour state around so callers (LEDManager watchUiSetLed) still compile.
    (void)s_blinkMs;
    (void)s_lastBlinkTick;
    (void)s_blinkOn;

    // Banner timeout — the power-hold banner suppresses auto-hide so the
    // countdown stays visible while the wearer keeps the key down.
    if (s_bannerLbl && !(lv_obj_has_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN))) {
        if (!s_powerHold && s_bannerUntil && now > s_bannerUntil) {
            lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
            s_bannerUntil = 0;
        }
    }

    // Power-key hold poll — AXP2101 latches negative (press) and positive
    // (release) IRQs; we watch the state and drive the countdown banner.
    // AXP2101 default long-press timeout for power-off is ~4 s.
    if (s_pmuReady) {
        static bool     pkeyDown       = false;
        static uint32_t pkeyDownStart  = 0;
        static unsigned long lastPoll  = 0;
        if (now - lastPoll >= 100) {
            lastPoll = now;
            s_pmu.getIrqStatus();   // refresh cached status
            if (s_pmu.isPekeyNegativeIrq()) {
                pkeyDown = true;
                pkeyDownStart = now;
                watchUiBeginPowerHold();
                s_pmu.clearIrqStatus();
            }
            if (s_pmu.isPekeyPositiveIrq()) {
                pkeyDown = false;
                watchUiEndPowerHold();
                s_pmu.clearIrqStatus();
            }
            if (pkeyDown) {
                const uint32_t HOLD_MS = 4000;
                uint32_t elapsed = now - pkeyDownStart;
                int remaining = elapsed >= HOLD_MS ? 0
                                : (int)((HOLD_MS - elapsed + 999) / 1000);
                watchUiSetPowerHoldRemaining(remaining);
                // AXP2101 handles the actual shutdown at its long-press
                // threshold — we just paint the countdown until it fires.
            }
        }
    }

    // Toast timeout
    if (s_flashLbl && !(lv_obj_has_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN))) {
        if (s_flashUntil && now > s_flashUntil) {
            lv_obj_add_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);
            s_flashUntil = 0;
        }
    }

    // Battery poll (~0.5 Hz). /improve-performance: only rewrite the label
    // when a *change* actually happened. On the AMOLED path, every label
    // set marks the widget dirty which forces a partial redraw; when nothing
    // changed we were spending CPU on identical rewrites 2 times/second.
    if (now - s_lastBattPoll >= 2000) {
        s_lastBattPoll = now;
        if (!s_pmuReady) tryInitPMU();
        if (s_pmuReady && s_battLbl) {
            int   pct = s_pmu.isBatteryConnect() ? s_pmu.getBatteryPercent() : -1;
            int   mv  = (int)s_pmu.getBattVoltage();
            // Quantise mV to 20 mV to dampen jitter — else the tenths digit
            // dances every poll on a stable battery.
            mv = (mv / 20) * 20;
            static int lastPct = -999;
            static int lastMv  = -999;
            if (pct != lastPct || mv != lastMv) {
                lastPct = pct;
                lastMv  = mv;
                const char* icon =
                    pct < 0            ? LV_SYMBOL_USB :
                    pct >= 80          ? LV_SYMBOL_BATTERY_FULL :
                    pct >= 60          ? LV_SYMBOL_BATTERY_3    :
                    pct >= 40          ? LV_SYMBOL_BATTERY_2    :
                    pct >= 20          ? LV_SYMBOL_BATTERY_1    :
                                         LV_SYMBOL_BATTERY_EMPTY;
                char buf[48];
                if (pct >= 0) snprintf(buf, sizeof(buf), "%s  %d%%   %d.%02dV",
                                       icon, pct, mv / 1000, (mv % 1000) / 10);
                else          snprintf(buf, sizeof(buf), "%s  %s  %d.%02dV",
                                       icon,
                                       s_pmu.isVbusIn() ? "USB" : "no batt",
                                       mv / 1000, (mv % 1000) / 10);
                lv_label_set_text(s_battLbl, buf);
            }
        }
    }
}

void watchUiSetLed(uint32_t rgb, uint16_t blinkMs) {
    s_ledColor = rgb & 0xFFFFFF;
    s_blinkMs  = blinkMs;
}

void watchUiFlash(const char* text) {
    if (!s_flashLbl || !text) return;
    lv_label_set_text(s_flashLbl, text);
    lv_obj_clear_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);
    s_flashUntil = millis() + 5000;
}

void watchUiSetStatus(const char* text) {
    if (!s_statusLbl || !text) return;
    // /improve-performance: don't rewrite the label when the text didn't
    // change — the .ino loop calls this every 250 ms with the same string
    // 99% of the time.
    static char last[48] = "";
    if (strncmp(last, text, sizeof(last) - 1) == 0) return;
    strncpy(last, text, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
    lv_label_set_text(s_statusLbl, text);
}

void watchUiSetScript(const char* name) {
    if (!s_scriptLbl) return;
    // /improve-performance: same change-detect as setStatus. Called from the
    // 4 Hz loop tick, the script name only changes when execution transitions.
    static char last[64] = "";
    const char* payload = (name && *name) ? name : "";
    if (strncmp(last, payload, sizeof(last) - 1) == 0) return;
    strncpy(last, payload, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
    if (!*payload) lv_label_set_text(s_scriptLbl, LV_SYMBOL_FILE "  no script");
    else           lv_label_set_text_fmt(s_scriptLbl,
                       LV_SYMBOL_FILE "  %s", payload);
}

void watchUiSetClients(int n) {
    if (!s_clientsLbl) return;
    // /improve-performance: only rewrite when the count actually changed.
    // loop() calls this 4x/second — otherwise we mark the label dirty and
    // re-render an identical string.
    static int lastN = -1;
    if (n == lastN) return;
    lastN = n;
    // Bug #12: LV_SYMBOL_WIFI matches the AP+PSK+IP block below better than USB.
    lv_label_set_text_fmt(s_clientsLbl,
        LV_SYMBOL_WIFI "  %d client%s", n, n == 1 ? "" : "s");
}

void watchUiSetAP(const char* ssid, const char* password, const char* ip) {
    if (s_apSsidLbl && ssid)     lv_label_set_text_fmt(s_apSsidLbl,
                                     LV_SYMBOL_WIFI "  AP: %s", ssid);
    if (s_apPassLbl && password) lv_label_set_text_fmt(s_apPassLbl,
                                     "PSK: %s", password);
    if (s_ipLbl && ip)           lv_label_set_text_fmt(s_ipLbl,
                                     "http://%s", ip);
}

void watchUiSetProgress(int percent) {
    if (!s_progress) return;
    if (percent < 0 || percent > 100) {
        lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_progress, percent, LV_ANIM_OFF);
}

bool watchUiConsumeStopPressed() {
    bool v = s_stopEdge;
    s_stopEdge = false;
    return v;
}

WatchUiPendingSettings watchUiConsumePendingSettings() {
    WatchUiPendingSettings p = s_pending;
    s_pending = WatchUiPendingSettings{};
    return p;
}

WatchUiPendingScriptAction watchUiConsumePendingScriptAction() {
    WatchUiPendingScriptAction p = s_pendingAction;
    s_pendingAction = WatchUiPendingScriptAction{};
    return p;
}

void watchUiSetBanner(const char* text, uint32_t rgb) {
    if (!s_bannerLbl) return;
    if (!text || !*text) {
        lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
        s_bannerUntil = 0;
        return;
    }
    lv_label_set_text(s_bannerLbl, text);
    lv_obj_set_style_bg_color(s_bannerLbl, lvhex(rgb), 0);
    lv_obj_clear_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
    // Banner and clock both live on lv_layer_top(). Force the banner to the
    // top of the sibling order so the power-off countdown stays visible
    // even while the clock overlay is up.
    lv_obj_move_foreground(s_bannerLbl);
    // 4-second banner; call watchUiSetBanner(nullptr) to clear early.
    s_bannerUntil = millis() + 4000;
}

void watchUiSetFileList(const std::vector<String>& names,
                        const String& autostartName) {
    if (!s_filesList || !s_filesEmpty) return;
    s_currentAutostart = autostartName;
    // Wipe existing rows. LVGL frees the child widgets; the user_data we set
    // on each button points at std::string entries in s_fileRowNames, so we
    // clear the LVGL tree BEFORE mutating the vector so no callback fires
    // against a freed pointer.
    lv_obj_clean(s_filesList);
    s_fileRowNames.clear();
    s_fileRowNames.reserve(names.size());

    if (names.empty()) {
        lv_obj_clear_flag(s_filesEmpty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_filesEmpty, LV_OBJ_FLAG_HIDDEN);

    for (const String& n : names) {
        s_fileRowNames.emplace_back(n.c_str());
        const char* nameCstr = s_fileRowNames.back().c_str();

        lv_obj_t* row = lv_obj_create(s_filesList);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LCD_W, 68);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                   LV_FLEX_ALIGN_CENTER,
                                   LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, nameCstr);
        lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        // Same anti-stretch discipline as the clock label — pin transform
        // scale + letter spacing on the row label so a theme-driven
        // transform can't rubber-band it wider than the file name needs.
        lv_obj_set_style_transform_scale_x(lbl, 256, 0);
        lv_obj_set_style_transform_scale_y(lbl, 256, 0);
        lv_obj_set_style_text_letter_space(lbl, 0, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_pad_right(lbl, 8, 0);

        bool isAutostart = (autostartName.length() > 0 &&
                            autostartName == n);

        // ⏱ Autostart — filled yellow when set, muted grey when not.
        lv_obj_t* starBtn = lv_btn_create(row);
        lv_obj_set_size(starBtn, 52, 52);
        lv_obj_set_style_bg_color(starBtn,
            lvhex(isAutostart ? 0xE5A93A : 0x2A2A30), 0);
        lv_obj_set_style_radius(starBtn, 8, 0);
        lv_obj_set_style_shadow_width(starBtn, 0, 0);
        lv_obj_add_event_cb(starBtn, filesStarCb, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* starLbl = lv_label_create(starBtn);
        // No star glyph in the built-in montserrat FA subset; use upload
        // arrow ("pin") with a bright-vs-dim colour tell.
        lv_label_set_text(starLbl, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_text_color(starLbl,
            isAutostart ? lv_color_white() : lvhex(C_MUTED), 0);
        lv_obj_set_style_text_font(starLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(starLbl);

        // ▶ Run — green square
        lv_obj_t* runBtn = lv_btn_create(row);
        lv_obj_set_size(runBtn, 52, 52);
        lv_obj_set_style_bg_color(runBtn, lvhex(C_OK), 0);
        lv_obj_set_style_radius(runBtn, 8, 0);
        lv_obj_set_style_shadow_width(runBtn, 0, 0);
        lv_obj_set_style_margin_left(runBtn, 6, 0);
        lv_obj_add_event_cb(runBtn, filesRunCb, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* runLbl = lv_label_create(runBtn);
        lv_label_set_text(runLbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(runLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(runLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(runLbl);

        // 🗑 Delete — red square, rightmost
        lv_obj_t* delBtn = lv_btn_create(row);
        lv_obj_set_size(delBtn, 52, 52);
        lv_obj_set_style_bg_color(delBtn, lvhex(C_DANGER), 0);
        lv_obj_set_style_radius(delBtn, 8, 0);
        lv_obj_set_style_shadow_width(delBtn, 0, 0);
        lv_obj_set_style_margin_left(delBtn, 6, 0);
        lv_obj_add_event_cb(delBtn, filesDelCb, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* delLbl = lv_label_create(delBtn);
        lv_label_set_text(delLbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(delLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(delLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(delLbl);
    }
}

// Sync switches without firing our event handler (would re-queue a pending
// change and loop forever). LVGL clears the checked state via lv_obj_add/
// clear_state directly — LV_EVENT_VALUE_CHANGED only fires on interactive
// toggles, so this is safe.
static void syncSw(int idx, bool on) {
    if (idx < 0 || idx >= SET_COUNT) return;
    if (!s_settingSw[idx]) return;
    bool was = lv_obj_has_state(s_settingSw[idx], LV_STATE_CHECKED);
    if (was == on) return;
    if (on) lv_obj_add_state (s_settingSw[idx], LV_STATE_CHECKED);
    else    lv_obj_clear_state(s_settingSw[idx], LV_STATE_CHECKED);
}

void watchUiRefreshSettings(bool wifi, bool bt, bool btdisc,
                            bool led,  bool silent, bool logging, bool com) {
    syncSw(SET_WIFI,    wifi);
    syncSw(SET_BT,      bt);
    syncSw(SET_BTDISC,  btdisc);
    syncSw(SET_LED,     led);
    syncSw(SET_SILENT,  silent);
    syncSw(SET_LOGGING, logging);
    syncSw(SET_COM,     com);
}

void watchUiSetAutostartToggle(bool on) {
    syncSw(SET_AUTOSTART, on);
}

WatchUiPendingExtras watchUiConsumePendingExtras() {
    WatchUiPendingExtras p = s_pendingExtras;
    s_pendingExtras = WatchUiPendingExtras{};
    return p;
}

// ---- First-boot walkthrough overlay ----------------------------------------
// A three-slide modal that covers the tabview. Same restrained palette as the
// Settings tab — no gradient, no glow. Slides:
//   0. Welcome — what this device is
//   1. Dashboard — SSID / PSK / URL, tap to switch tabs
//   2. STOP — how the button works, safety cue
// Progress dots at the bottom-centre, Skip button top-right, primary CTA
// full-width on the bottom.
static lv_obj_t* s_wtRoot        = nullptr;
static lv_obj_t* s_wtHeadline    = nullptr;
static lv_obj_t* s_wtBody        = nullptr;
static lv_obj_t* s_wtDots[3]     = { nullptr, nullptr, nullptr };
static lv_obj_t* s_wtNextBtn     = nullptr;
static lv_obj_t* s_wtNextLbl     = nullptr;
static int       s_wtSlide       = 0;
static char      s_wtSsid[48]    = "";
static char      s_wtPsk[48]     = "";
static char      s_wtIp[24]      = "";
static WatchUiWalkthroughDone s_wtDoneCb = nullptr;

static void wtSetSlide(int idx);
static void wtRender();

static void wtDismiss() {
    if (!s_wtRoot) return;
    lv_obj_del(s_wtRoot);
    s_wtRoot = nullptr;
    if (s_wtDoneCb) { s_wtDoneCb(); s_wtDoneCb = nullptr; }
}

static void wtNextCb(lv_event_t*) {
    if (s_wtSlide >= 2) wtDismiss();
    else                wtSetSlide(s_wtSlide + 1);
}
static void wtSkipCb(lv_event_t*) { wtDismiss(); }

static void wtSetSlide(int idx) {
    s_wtSlide = idx;
    for (int i = 0; i < 3; ++i) {
        if (!s_wtDots[i]) continue;
        lv_obj_set_style_bg_color(s_wtDots[i],
            lvhex(i == idx ? C_ACCENT : C_LINE), 0);
    }
    wtRender();
}

static void wtRender() {
    if (!s_wtHeadline || !s_wtBody || !s_wtNextLbl) return;
    switch (s_wtSlide) {
        case 0:
            lv_label_set_text(s_wtHeadline, "Welcome");
            lv_label_set_text_fmt(s_wtBody,
                "This is your ESP-Watch-BadUSB.\n\n"
                "USB HID + web-driven BadUSB in a wrist form-factor. "
                "Tap through in a second to see how it's laid out.");
            lv_label_set_text(s_wtNextLbl, "Next  " LV_SYMBOL_RIGHT);
            break;
        case 1:
            lv_label_set_text(s_wtHeadline, "Dashboard");
            lv_label_set_text_fmt(s_wtBody,
                "Join the watch's WiFi and open the dashboard.\n\n"
                LV_SYMBOL_WIFI "  %s\n"
                "PSK: %s\n\n"
                "%s",
                s_wtSsid, s_wtPsk, s_wtIp);
            lv_label_set_text(s_wtNextLbl, "Next  " LV_SYMBOL_RIGHT);
            break;
        case 2:
            lv_label_set_text(s_wtHeadline, "STOP button");
            lv_label_set_text_fmt(s_wtBody,
                "The red button on the Home tab kills any running "
                "DuckyScript instantly. Same effect as a short-press on "
                "the BOOT button.\n\n"
                "The " LV_SYMBOL_SETTINGS " Settings tab toggles WiFi, "
                "Bluetooth, LED, silent boot and more.");
            lv_label_set_text(s_wtNextLbl, LV_SYMBOL_OK "  Got it");
            break;
    }
}

bool watchUiWalkthroughActive() { return s_wtRoot != nullptr; }

void watchUiShowWalkthrough(const char* ssid, const char* psk, const char* ip,
                            WatchUiWalkthroughDone onDoneCb) {
    if (s_wtRoot) return;   // already shown
    s_wtDoneCb = onDoneCb;
    strncpy(s_wtSsid, ssid ? ssid : "(none)",     sizeof(s_wtSsid) - 1);
    strncpy(s_wtPsk,  psk  ? psk  : "(open)",     sizeof(s_wtPsk)  - 1);
    strncpy(s_wtIp,   ip   ? ip   : "http://192.168.4.1", sizeof(s_wtIp) - 1);
    s_wtSsid[sizeof(s_wtSsid) - 1] = '\0';
    s_wtPsk [sizeof(s_wtPsk)  - 1] = '\0';
    s_wtIp  [sizeof(s_wtIp)   - 1] = '\0';

    // Full-screen modal. Sits on top layer so it hides the tabview cleanly.
    s_wtRoot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wtRoot);
    lv_obj_set_size(s_wtRoot, LCD_W, LCD_H);
    lv_obj_set_pos(s_wtRoot, 0, 0);
    lv_obj_set_style_bg_color(s_wtRoot, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_wtRoot, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_wtRoot, 20, 0);
    lv_obj_clear_flag(s_wtRoot, LV_OBJ_FLAG_SCROLLABLE);

    // Top-right Skip button.
    lv_obj_t* skip = lv_btn_create(s_wtRoot);
    lv_obj_set_size(skip, 90, 40);
    lv_obj_align(skip, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(skip, lvhex(C_SURFACE), 0);
    lv_obj_set_style_radius(skip, 8, 0);
    lv_obj_set_style_shadow_width(skip, 0, 0);
    lv_obj_add_event_cb(skip, wtSkipCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* sk = lv_label_create(skip);
    lv_label_set_text(sk, "Skip");
    lv_obj_set_style_text_color(sk, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(sk, &lv_font_montserrat_16, 0);
    lv_obj_center(sk);

    // Headline
    s_wtHeadline = lv_label_create(s_wtRoot);
    lv_obj_set_style_text_color(s_wtHeadline, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_wtHeadline, &lv_font_montserrat_32, 0);
    lv_obj_align(s_wtHeadline, LV_ALIGN_TOP_LEFT, 0, 70);

    // Body
    s_wtBody = lv_label_create(s_wtRoot);
    lv_obj_set_style_text_color(s_wtBody, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_wtBody, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_wtBody, LCD_W - 40);
    lv_label_set_long_mode(s_wtBody, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_wtBody, LV_ALIGN_TOP_LEFT, 0, 130);

    // Progress dots (three)
    lv_obj_t* dotRow = lv_obj_create(s_wtRoot);
    lv_obj_remove_style_all(dotRow);
    lv_obj_set_size(dotRow, LV_SIZE_CONTENT, 20);
    lv_obj_align(dotRow, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_set_flex_flow(dotRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(dotRow, 10, 0);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* d = lv_obj_create(dotRow);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lvhex(C_LINE), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        s_wtDots[i] = d;
    }

    // Primary CTA — Next / Got it
    s_wtNextBtn = lv_btn_create(s_wtRoot);
    lv_obj_set_size(s_wtNextBtn, LCD_W - 40, 68);
    lv_obj_align(s_wtNextBtn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_wtNextBtn, lvhex(C_ACCENT), 0);
    lv_obj_set_style_radius(s_wtNextBtn, 12, 0);
    lv_obj_set_style_shadow_width(s_wtNextBtn, 0, 0);
    lv_obj_add_event_cb(s_wtNextBtn, wtNextCb, LV_EVENT_CLICKED, nullptr);
    s_wtNextLbl = lv_label_create(s_wtNextBtn);
    lv_obj_set_style_text_color(s_wtNextLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wtNextLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_wtNextLbl);

    wtSetSlide(0);
}
