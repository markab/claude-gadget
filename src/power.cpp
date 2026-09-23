#define XPOWERS_CHIP_AXP2101
#include "power.h"
#include "board.h"
#include <Wire.h>
#include <XPowersLib.h>

// All PMU access happens from the Arduino loop task, which also owns touch,
// so the shared I2C bus needs no locking.

static XPowersPMU pmu;
static bool pmuOk = false;
static BatteryInfo cached = {};
static uint32_t lastRead = 0;
static uint32_t lastIrqPoll = 0;

bool power_init() {
    pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
    if (!pmuOk) {
        Serial.println("[pmu] AXP2101 not found");
        return false;
    }

    // PWR button: press 512 ms to power on from off. Firmware powers off on a
    // 2 s hold (long-press IRQ). The PMU forces power-off at 6 s as a failsafe
    // if the firmware has hung.
    pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_512MS);
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
    pmu.setIrqLevelTime(XPOWERS_AXP2101_IRQ_TIME_2S);

    pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    pmu.enableTemperatureMeasure();
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();

    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ |
                  XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ |
                  XPOWERS_AXP2101_BAT_CHG_DONE_IRQ);
    return true;
}

uint8_t power_poll_events() {
    if (!pmuOk) return PWR_EVT_NONE;
    // The PMU IRQ line isn't routed to a GPIO we use, so poll the status registers.
    uint32_t now = millis();
    if (now - lastIrqPoll < 50) return PWR_EVT_NONE;
    lastIrqPoll = now;

    pmu.getIrqStatus();
    uint8_t ev = PWR_EVT_NONE;
    if (pmu.isPekeyShortPressIrq()) ev |= PWR_EVT_SHORT_PRESS;
    if (pmu.isPekeyLongPressIrq())  ev |= PWR_EVT_LONG_PRESS;
    if (pmu.isVbusInsertIrq())      ev |= PWR_EVT_VBUS_IN;
    if (pmu.isVbusRemoveIrq())      ev |= PWR_EVT_VBUS_OUT;
    if (pmu.isBatChargeDoneIrq())   ev |= PWR_EVT_CHG_DONE;
    if (ev) {
        pmu.clearIrqStatus();
        lastRead = 0;  // force a fresh battery read on the next call
    }
    return ev;
}

static const char *chargeStateName(uint8_t s) {
    switch (s) {
        case XPOWERS_AXP2101_CHG_TRI_STATE:  return "Trickle";
        case XPOWERS_AXP2101_CHG_PRE_STATE:  return "Pre-charge";
        case XPOWERS_AXP2101_CHG_CC_STATE:   return "Fast (CC)";
        case XPOWERS_AXP2101_CHG_CV_STATE:   return "Topping (CV)";
        case XPOWERS_AXP2101_CHG_DONE_STATE: return "Full";
        default:                             return "Not charging";
    }
}

BatteryInfo power_battery() {
    if (!pmuOk) return cached;
    uint32_t now = millis();
    if (lastRead && now - lastRead < 2000) return cached;
    lastRead = now;

    BatteryInfo b;
    b.present = pmu.isBatteryConnect();
    b.vbus = pmu.isVbusIn();
    b.charging = pmu.isCharging();
    uint8_t st = pmu.getChargerStatus();
    b.full = st == XPOWERS_AXP2101_CHG_DONE_STATE;
    b.chargeState = b.present ? chargeStateName(st) : "No battery";
    b.percent = b.present ? pmu.getBatteryPercent() : -1;
    b.battMv = b.present ? pmu.getBattVoltage() : 0;
    b.vbusMv = b.vbus ? pmu.getVbusVoltage() : 0;
    b.sysMv = pmu.getSystemVoltage();
    b.pmuTempC = pmu.getTemperature();
    cached = b;
    return cached;
}

void power_off() {
    Serial.println("[pmu] shutdown");
    Serial.flush();
    if (pmuOk) pmu.shutdown();
    // Only reached if the PMU is missing.
    esp_deep_sleep_start();
}
