#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/systick.h"
#include "driver/system.h"
#include "driver/gpio.h"
#include "bsp/dp32g030/gpio.h"
#include "audio.h"
#include "functions.h"
#include "dcs.h"
#include "misc.h"
#include "settings.h"
#include "radio.h"
#include "ui/ui.h"
#include "app/app.h"
#include <string.h>

InterceptorChannel_t gScanList[GRID_TOTAL_SLOTS] = {0};
uint32_t gLockoutList[MAX_LOCKOUTS] = {0};
uint8_t  gLockoutCount = 0;
uint8_t  gUserSelectedChannelIndex = 0;
uint8_t  gCurrentGridPage = 0;
bool     gSniffingEnabled = false;
bool     gInterceptorViewActive = false;
uint32_t gInterceptorActiveFrequency = 0;
uint8_t  gInterceptorMeterPercent = 0;
bool     gInterceptorTxOverrideActive = false;

// Defined in app/interceptor.c - the save-to-memory confirmation shown on
// the status bar. Declared here so the 10ms tick can count it down.
extern int16_t  gInterceptorSavedChannelNotify;
extern uint16_t gInterceptorSaveNotifyCountdown;
extern int8_t   gInterceptorSaveFlashSlot;

static uint16_t sReplyWaitCountdown = 0;
static bool     sWaitingForReply = false;
bool     gInterceptorBandSweepActive = false;

SweepBand_t gSweepBands[SWEEP_BAND_COUNT] = {
    { 5000000,  5400000,  1000, "Ham 6m",      false },
    { 14400000, 14800000, 2500, "Ham 2m",      false },
    { 21900000, 22500000, 2500, "Ham 1.25m",   false },
    { 42000000, 45000000, 2500, "Ham 70cm",    false },
    { 15000000, 17400000, 1250, "VHF LandMob", false },
    { 40000000, 42000000, 1250, "UHF Fed",     false },
    { 45000000, 47000000, 1250, "UHF LandMob", false },
    { 80600000, 82400000, 1250, "800 PS",      false },
    { 0,        0,        1250, "Manual",      false },
};

uint8_t gBandSelectHighlight = 0;
bool    gBandSelectEnteringFreq = false;
uint8_t gBandSelectEnteringWhich = 0;
uint8_t gBandSelectStepOptionIndex = 4;
const uint32_t gStepOptions[STEP_OPTION_COUNT] = { 250, 500, 625, 1000, 1250, 2000, 2500 };
bool    gExcludeNoaa = false;
bool    gInterceptorPendingBlacklistBuzz = false;
bool    gSweepNeedsReinit = true;
bool     gInterceptorHuntTickerActive = false;
uint32_t gInterceptorHuntTickerFreq = 0;
int8_t   gInterceptorFlashSlot = -1;
uint8_t  gInterceptorFlashCount = 0;
int16_t  gInterceptorLastActiveSlot = -1; // passive tick-mark on last-heard cell, never moves the cursor
int16_t  gInterceptorCheckingSlot = -1;

#define CANDIDATE_SETTLE_10MS_TICKS  20 // doubled from 10 - the checking-slot flash was sporadic/imperceptible at the shorter window, this gives a screen redraw a reliable chance to actually catch it

enum { CANDCHECK_IDLE, CANDCHECK_WAITING };

typedef struct {
    uint8_t  state;
    uint16_t settleCountdown;
} CandCheckState_t;

static CandCheckState_t sGridCheckState = { CANDCHECK_IDLE, 0 };
static CandCheckState_t sSweepCheckState = { CANDCHECK_IDLE, 0 };

static void Tune_RxVfo_To(uint32_t freq, uint8_t codeType, uint8_t code) {
    gRxVfo->freq_config_RX.Frequency = freq;
    gRxVfo->freq_config_RX.CodeType = codeType;
    gRxVfo->freq_config_RX.Code     = code;
    gRxVfo->CHANNEL_BANDWIDTH = BK4819_FILTER_BW_NARROW;
    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(true);
}

// Fast, RSSI-only pre-check - mirrors the stock spectrum analyzer's own
// retune (app/spectrum.c's SetF()/GetRssi()), reading RSSI almost
// immediately after retuning rather than waiting through the full,
// deliberate ~100ms settle Check_Candidate_Frequency below uses.
// Only rules out clearly-empty spectrum quickly. Anything strong enough
// still goes through the full settle-and-verify below before being saved.
#define SWEEP_RSSI_FALLBACK_DBM (-100)
static bool Fast_Rssi_Precheck(uint32_t freq) {
    BK4819_SetFrequency(freq);
    BK4819_PickRXFilterPathBasedOnFrequency(freq);
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, reg);
    uint16_t guard = 0;
    while ((BK4819_ReadRegister(0x63) & 0xFF) >= 255 && guard++ < 32) {
        SYSTICK_DelayUs(100);
    }
    return BK4819_GetRSSI_dBm() > SWEEP_RSSI_FALLBACK_DBM;
}

static uint8_t Check_Candidate_Frequency(CandCheckState_t *st, uint32_t freq, uint8_t codeType, uint8_t code) {
    if (st->state == CANDCHECK_IDLE) {
        Tune_RxVfo_To(freq, codeType, code);
        st->state           = CANDCHECK_WAITING;
        st->settleCountdown = CANDIDATE_SETTLE_10MS_TICKS;
        return 0;
    }
    if (st->settleCountdown > 0) return 0;

    st->state = CANDCHECK_IDLE;
    if (gCurrentFunction == FUNCTION_INCOMING || gCurrentFunction == FUNCTION_RECEIVE) {
        return 1;
    }
    return 2;
}

#define AF_LEVEL_MAX  63

static void Update_Meter_Level(void) {
    uint8_t af = BK4819_GetAfTxRx();
    uint8_t afInverted = (af > AF_LEVEL_MAX) ? 0 : (AF_LEVEL_MAX - af);
    uint32_t pct = ((uint32_t)afInverted * 100) / AF_LEVEL_MAX;
    if (pct > 100) pct = 100;
    gInterceptorMeterPercent = (uint8_t)(10 + (pct * 90) / 100);
}

uint8_t INTERCEPTOR_GetUsedPageCount(void) {
    for (int16_t i = GRID_TOTAL_SLOTS - 1; i >= 0; i--) {
        if (gScanList[i].Frequency != 0)
            return (uint8_t)(i / GRID_PAGE_SIZE) + 1;
    }
    return 1;
}

uint8_t INTERCEPTOR_GetReachablePageCount(void) {
    uint8_t used = INTERCEPTOR_GetUsedPageCount();
    uint8_t reachable = used + 1;
    return (reachable > GRID_MAX_PAGES) ? GRID_MAX_PAGES : reachable;
}

void INTERCEPTOR_SortByPopularity(void) {
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS - 1; i++) {
        for (uint16_t j = 0; j < GRID_TOTAL_SLOTS - 1 - i; j++) {
            if (gScanList[j].IsLocked || gScanList[j + 1].IsLocked) continue;
            if (gScanList[j].HitCount < gScanList[j + 1].HitCount) {
                InterceptorChannel_t tmp = gScanList[j];
                gScanList[j] = gScanList[j + 1];
                gScanList[j + 1] = tmp;
            }
        }
    }
}

#define FREQ_DEDUP_TOLERANCE  10000

static uint16_t sLastEvictedSlot = 0xFFFF;

void INTERCEPTOR_LogNewCapture(uint32_t freq, uint8_t codeType, uint8_t code) {
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
        uint32_t existing = gScanList[i].Frequency;
        if (existing == 0) continue;
        uint32_t delta = (existing > freq) ? (existing - freq) : (freq - existing);
        if (delta <= FREQ_DEDUP_TOLERANCE) return;
    }

    uint16_t target = 0xFFFF;
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
        if (gScanList[i].Frequency == 0) { target = i; break; }
    }
    if (target == 0xFFFF) {
        uint8_t lowest_hits = 255;
        for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
            if (gScanList[i].IsLocked) continue;
            if (gScanList[i].HitCount < lowest_hits) {
                lowest_hits = gScanList[i].HitCount;
                target = i;
            }
        }
        if (target == sLastEvictedSlot) {
            for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
                if (gScanList[i].IsLocked || i == sLastEvictedSlot) continue;
                if (gScanList[i].HitCount == lowest_hits) { target = i; break; }
            }
        }
        if (target == 0xFFFF) return;
    }

    sLastEvictedSlot = target;
    memset(&gScanList[target], 0, sizeof(InterceptorChannel_t));
    gScanList[target].Frequency = freq;
    gScanList[target].CodeType  = codeType;
    gScanList[target].Code      = code;
    gInterceptorFlashSlot  = (int8_t)target;
    gInterceptorFlashCount = 6;
    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
    gUpdateDisplay = true;
}

void INTERCEPTOR_DeleteAndBlacklist(uint16_t slotIndex, bool playDefaultBeep) {
    if (slotIndex >= GRID_TOTAL_SLOTS) return;
    if (gScanList[slotIndex].Frequency == 0) return;

    uint32_t freq = gScanList[slotIndex].Frequency;

    if (!gScanList[slotIndex].IsLocked) {
        if (gLockoutCount < MAX_LOCKOUTS) {
            gLockoutList[gLockoutCount++] = freq;
        } else {
            memmove(&gLockoutList[0], &gLockoutList[1], (MAX_LOCKOUTS - 1) * sizeof(uint32_t));
            gLockoutList[MAX_LOCKOUTS - 1] = freq;
        }
    }

    memset(&gScanList[slotIndex], 0, sizeof(InterceptorChannel_t));

    if (gInterceptorActiveFrequency == freq) {
        gInterceptorActiveFrequency = 0;
        sWaitingForReply = false;
    }

    if (playDefaultBeep)
        AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
}

void INTERCEPTOR_DeleteOnly(uint16_t slotIndex) {
    if (slotIndex >= GRID_TOTAL_SLOTS) return;
    if (gScanList[slotIndex].Frequency == 0) return;

    uint32_t freq = gScanList[slotIndex].Frequency;
    memset(&gScanList[slotIndex], 0, sizeof(InterceptorChannel_t));

    if (gInterceptorActiveFrequency == freq) {
        gInterceptorActiveFrequency = 0;
        sWaitingForReply = false;
    }

    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
}

#define REPLY_WAIT_10MS_TICKS  100 // ~1s grace before abandoning a dwell - a brief pause in speech or a momentary squelch flicker shouldn't instantly kick the engine back into scanning
#define METER_REDRAW_10MS_TICKS 10
#define TICKER_REDRAW_10MS_TICKS 15
#define MAX_DWELL_10MS_TICKS 2000
#define NOISE_CHECK_10MS_TICKS 150
#define NOISE_VARIANCE_THRESHOLD 20
#define NOISE_LOUD_THRESHOLD 30
#define NOISE_EARLY_EXIT_10MS_TICKS 50
#define NOISE_FLAGS_BEFORE_BLACKLIST 3
#define NOISE_STEP_DELTA_THRESHOLD 2
#define NOISE_CROSSPASS_TOLERANCE 4
#define NOISE_PAUSE_THRESHOLD 20

// Grid-check runs once per second during sweep - enough to catch any
// conversation on a saved cell, without constantly stealing the radio
// away from sweep mid-step. 100 ticks * 10ms = 1 second.
#define GRID_CHECK_INTERVAL_TICKS 100

// Grid-check scheduling state, shared between INTERCEPTOR_TimeSlice10ms
// (which owns the TIMING - it's a genuine 10ms tick) and
// INTERCEPTOR_Engine_Tick (which owns the WORK - it runs from the main
// loop at a much faster, variable rate).
//
// This split matters: previous versions counted the 1-second interval
// inside Engine_Tick, so "100 ticks" was really 100 main-loop iterations,
// not 1 second - which is why the cadence never behaved as intended no
// matter how the counting logic was rearranged. Real time is counted
// here, in the only place that actually ticks at a known rate.
static uint16_t sGridCheckIntervalCounter = 0; // counts real 10ms ticks since the last pass
static bool     sGridPassDue    = false;       // set by the 10ms tick when a pass is owed
static bool     sGridPassActive = false;       // a pass is currently in progress
static uint16_t sGridPassIdx    = 0;           // forward-only position in the current pass

static uint16_t sMeterRedrawCountdown = 0;
static uint16_t sTickerRedrawCountdown = 0;
static uint16_t sDwellDurationCountdown = 0;

void INTERCEPTOR_TimeSlice10ms(void) {
    // Once-per-second grid-check scheduler. Counted here because this is
    // the only function that runs at a known, fixed rate. Only advances
    // while sweep is actually sweeping - not while dwelling on a signal
    // and not while a pass is already running - so time spent listening
    // doesn't bank up a backlog of owed passes.
    if (gInterceptorBandSweepActive && !sGridPassActive && gInterceptorActiveFrequency == 0) {
        if (++sGridCheckIntervalCounter >= GRID_CHECK_INTERVAL_TICKS) {
            sGridCheckIntervalCounter = 0;
            sGridPassDue = true;
        }
    }

    // Real-time PTT safety net. GPIO_CheckBit on the PTT pin returns TRUE
    // when the button is RELEASED (confirmed against app/app.c's own
    // handling, which comments the same polarity). End_Interceptor_PTT is
    // normally called by the key-release event, but that dispatch doesn't
    // always fire cleanly - and when it doesn't, gInterceptorTxOverrideActive
    // stays stuck true forever, permanently locking the meter into
    // mic/TX mode and showing it on whatever cell is selected. Debounced
    // over 3 ticks (~30ms), matching the stock firmware's own PTT debounce,
    // so a momentary glitch can't false-trigger it mid-transmission.
    if (gInterceptorTxOverrideActive) {
        static uint8_t sPttReleasedTicks = 0;
        if (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT)) {
            if (++sPttReleasedTicks >= 3) {
                sPttReleasedTicks = 0;
                End_Interceptor_PTT();
            }
        } else {
            sPttReleasedTicks = 0; // still genuinely held
        }
    }

    // Countdown for the "SAVED CH###" status-bar confirmation - without
    // this the notification would display once and then never clear.
    if (gInterceptorSaveNotifyCountdown > 0) {
        gInterceptorSaveNotifyCountdown--;
        if (gInterceptorSaveNotifyCountdown == 0) {
            gInterceptorSavedChannelNotify = -1;
            gInterceptorSaveFlashSlot = -1;
            gUpdateDisplay = true;
        }
    }

    if (sGridCheckState.state == CANDCHECK_WAITING && sGridCheckState.settleCountdown > 0)
        sGridCheckState.settleCountdown--;
    if (sSweepCheckState.state == CANDCHECK_WAITING && sSweepCheckState.settleCountdown > 0)
        sSweepCheckState.settleCountdown--;

    if (gInterceptorPendingBlacklistBuzz && gCurrentFunction != FUNCTION_RECEIVE) {
        AUDIO_PlayBeep(BEEP_440HZ_500MS);
        gInterceptorPendingBlacklistBuzz = false;
    }

    {
        static bool sWasDwelling = false;
        static uint8_t sDwellMeterMin = 255;
        static uint8_t sDwellMeterMax = 0;
        static uint8_t sDwellMeterPrev = 0;
        static uint8_t sDwellMaxStepDelta = 0;
        static uint32_t sDwellMeterSum = 0;
        static uint16_t sDwellSampleCount = 0;
        static uint16_t sNoiseCheckCountdown = 0;
        static bool sNoiseCheckDone = false;

        if (gInterceptorActiveFrequency != 0) {
            if (!sWasDwelling) {
                sDwellDurationCountdown = MAX_DWELL_10MS_TICKS;
                sWasDwelling = true;
                sDwellMeterMin = 255;
                sDwellMeterMax = 0;
                sDwellMeterPrev = gInterceptorMeterPercent;
                sDwellMaxStepDelta = 0;
                sDwellMeterSum = 0;
                sDwellSampleCount = 0;
                sNoiseCheckCountdown = NOISE_CHECK_10MS_TICKS;
                sNoiseCheckDone = false;
            } else if (sDwellDurationCountdown > 0) {
                sDwellDurationCountdown--;
                if (sDwellDurationCountdown == 0) {
                    gInterceptorActiveFrequency = 0;
                    sWaitingForReply = false;
                    INTERCEPTOR_SortByPopularity();
                    gUpdateDisplay = true;
                }
            }

            if (gInterceptorMeterPercent < sDwellMeterMin) sDwellMeterMin = gInterceptorMeterPercent;
            if (gInterceptorMeterPercent > sDwellMeterMax) sDwellMeterMax = gInterceptorMeterPercent;
            {
                uint8_t stepDelta = (gInterceptorMeterPercent > sDwellMeterPrev)
                    ? (uint8_t)(gInterceptorMeterPercent - sDwellMeterPrev)
                    : (uint8_t)(sDwellMeterPrev - gInterceptorMeterPercent);
                if (stepDelta > sDwellMaxStepDelta) sDwellMaxStepDelta = stepDelta;
                sDwellMeterPrev = gInterceptorMeterPercent;
            }
            sDwellMeterSum += gInterceptorMeterPercent;
            sDwellSampleCount++;

            if (!sNoiseCheckDone && sNoiseCheckCountdown > 0) {
                sNoiseCheckCountdown--;
                if (sNoiseCheckCountdown == 0) {
                    sNoiseCheckDone = true;
                    bool overallFlat = (sDwellMeterMax - sDwellMeterMin) < NOISE_VARIANCE_THRESHOLD;
                    bool smoothDrift = sDwellMaxStepDelta < NOISE_STEP_DELTA_THRESHOLD;
                    bool loud        = sDwellMeterMax > NOISE_LOUD_THRESHOLD;
                    bool hadPause    = sDwellMeterMin <= NOISE_PAUSE_THRESHOLD;
                    bool flatAndLoud = (overallFlat || smoothDrift) && loud && !hadPause;
                    uint8_t avgLevel = (sDwellSampleCount > 0)
                        ? (uint8_t)(sDwellMeterSum / sDwellSampleCount) : 0;

                    uint16_t dwellSlot = 0xFFFF;
                    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
                        if (gScanList[i].Frequency == gInterceptorActiveFrequency) { dwellSlot = i; break; }
                    }

                    if (flatAndLoud) {
                        if (sDwellDurationCountdown > NOISE_EARLY_EXIT_10MS_TICKS)
                            sDwellDurationCountdown = NOISE_EARLY_EXIT_10MS_TICKS;

                        if (dwellSlot != 0xFFFF && !gScanList[dwellSlot].IsLocked) {
                            uint8_t levelDelta = (avgLevel > gScanList[dwellSlot].NoiseFlagLevel)
                                ? (uint8_t)(avgLevel - gScanList[dwellSlot].NoiseFlagLevel)
                                : (uint8_t)(gScanList[dwellSlot].NoiseFlagLevel - avgLevel);
                            bool consistent = (gScanList[dwellSlot].NoiseFlagCount == 0)
                                || (levelDelta < NOISE_CROSSPASS_TOLERANCE);

                            gScanList[dwellSlot].NoiseFlagCount = consistent
                                ? (uint8_t)(gScanList[dwellSlot].NoiseFlagCount + 1) : 1;
                            gScanList[dwellSlot].NoiseFlagLevel = avgLevel;

                            if (gScanList[dwellSlot].NoiseFlagCount >= NOISE_FLAGS_BEFORE_BLACKLIST) {
                                INTERCEPTOR_DeleteAndBlacklist(dwellSlot, false);
                                gInterceptorPendingBlacklistBuzz = true;
                                gInterceptorActiveFrequency = 0;
                                sWaitingForReply = false;
                                gUpdateDisplay = true;
                            }
                        }
                    } else if (dwellSlot != 0xFFFF) {
                        gScanList[dwellSlot].NoiseFlagCount = 0;
                    }
                }
            }
        } else {
            sWasDwelling = false;
        }
    }

    if (sWaitingForReply && sReplyWaitCountdown > 0) {
        sReplyWaitCountdown--;
        if (sReplyWaitCountdown == 0) {
            sWaitingForReply = false;
            gInterceptorActiveFrequency = 0;
            INTERCEPTOR_SortByPopularity();
            gUpdateDisplay = true;
        }
    }

    if (gInterceptorActiveFrequency != 0 || gInterceptorTxOverrideActive) {
        if (gInterceptorTxOverrideActive) {
            uint16_t amp = BK4819_GetVoiceAmplitudeOut();
            uint32_t boosted = (uint32_t)amp * 64;
            if (boosted > 65535) boosted = 65535;
            uint32_t pct = (boosted * 100) / 65535;
            gInterceptorMeterPercent = (pct > 100) ? 100 : (uint8_t)pct;
        } else {
            Update_Meter_Level();
        }

        if (sMeterRedrawCountdown > 0) {
            sMeterRedrawCountdown--;
        } else {
            sMeterRedrawCountdown = METER_REDRAW_10MS_TICKS;
            if (gInterceptorTxOverrideActive) {
                UI_DisplayInterceptorGridPage();
            } else {
                gUpdateDisplay = true;
            }
        }
    }

    if (gInterceptorHuntTickerActive || gInterceptorFlashCount > 0 || gInterceptorSaveNotifyCountdown > 0) {
        if (sTickerRedrawCountdown > 0) {
            sTickerRedrawCountdown--;
        } else {
            sTickerRedrawCountdown = TICKER_REDRAW_10MS_TICKS;
            if (gInterceptorFlashCount > 0) gInterceptorFlashCount--;
            gUpdateDisplay = true;
        }
    }
}

static bool Handle_Active_Channel_Dwell(void) {
    if (gCurrentFunction == FUNCTION_INCOMING || gCurrentFunction == FUNCTION_RECEIVE) {
        sWaitingForReply = false;
        return true;
    }

    if (!sWaitingForReply) {
        if (REPLY_WAIT_10MS_TICKS == 0) {
            gInterceptorActiveFrequency = 0;
            INTERCEPTOR_SortByPopularity();
            gUpdateDisplay = true;
            return false;
        }
        sWaitingForReply = true;
        sReplyWaitCountdown = REPLY_WAIT_10MS_TICKS;
        return true;
    }

    return gInterceptorActiveFrequency != 0;
}

enum { HUNT_IDLE, HUNT_FREQ, HUNT_CSS };
static uint8_t  sHuntState = HUNT_IDLE;
static uint32_t sHuntFrequency = 0;
static uint8_t  sHuntStableCount = 0;
static uint8_t  sHuntCssAttempts = 0;
static uint8_t  sHuntCssResultType = CODE_TYPE_OFF;
static uint8_t  sHuntCssResultCode = 0;

#define CSS_MAX_ATTEMPTS  20

static void Hunt_Reset(void) {
    BK4819_DisableFrequencyScan();
    sHuntState = HUNT_IDLE;
    sHuntStableCount = 0;
    sHuntCssAttempts = 0;
    sHuntCssResultType = CODE_TYPE_OFF;
    sHuntCssResultCode = 0;
    gInterceptorHuntTickerActive = false;
}

static void Do_Hunt_Cycle(void) {
    if (sHuntState == HUNT_IDLE) {
        BK4819_PickRXFilterPathBasedOnFrequency(gRxVfo->pRX->Frequency);
        BK4819_EnableFrequencyScan();
        sHuntState = HUNT_FREQ;
        sHuntStableCount = 0;
        return;
    }

    if (sHuntState == HUNT_FREQ) {
        uint32_t result;
        if (!BK4819_GetFrequencyScanResult(&result)) return;

        int32_t delta = (int32_t)result - (int32_t)sHuntFrequency;
        sHuntFrequency = result;
        if (delta < 0) delta = -delta;

        BK4819_DisableFrequencyScan();

        if (delta < 100) sHuntStableCount++;
        else sHuntStableCount = 0;

        if (sHuntStableCount < 3) {
            BK4819_EnableFrequencyScan();
            return;
        }

        bool blacklisted = false;
        for (uint8_t i = 0; i < gLockoutCount; i++)
            if (gLockoutList[i] == sHuntFrequency) { blacklisted = true; break; }

        if (blacklisted) { Hunt_Reset(); return; }

        BK4819_SetScanFrequency(sHuntFrequency);
        sHuntCssAttempts = 0;
        sHuntState = HUNT_CSS;
        gInterceptorHuntTickerActive = false;
        gUpdateDisplay = true;
        return;
    }

    if (sHuntState == HUNT_CSS) {
        uint32_t cdcssFreq;
        uint16_t ctcssFreq;
        BK4819_CssScanResult_t result = BK4819_GetCxCSSScanResult(&cdcssFreq, &ctcssFreq);

        if (result == BK4819_CSS_RESULT_CDCSS) {
            uint8_t code = DCS_GetCdcssCode(cdcssFreq);
            if (code != 0xFF) {
                INTERCEPTOR_LogNewCapture(sHuntFrequency, CODE_TYPE_DIGITAL, code);
                Hunt_Reset();
                return;
            }
        } else if (result == BK4819_CSS_RESULT_CTCSS) {
            uint8_t code = DCS_GetCtcssCode((int)ctcssFreq);
            if (code != 0xFF) {
                if (code == sHuntCssResultCode && sHuntCssResultType == CODE_TYPE_CONTINUOUS_TONE) {
                    INTERCEPTOR_LogNewCapture(sHuntFrequency, CODE_TYPE_CONTINUOUS_TONE, code);
                    Hunt_Reset();
                    return;
                }
                sHuntCssResultType = CODE_TYPE_CONTINUOUS_TONE;
                sHuntCssResultCode = code;
            }
        }

        sHuntCssAttempts++;
        if (sHuntCssAttempts >= CSS_MAX_ATTEMPTS) {
            INTERCEPTOR_LogNewCapture(sHuntFrequency, CODE_TYPE_OFF, 0);
            Hunt_Reset();
            return;
        }

        BK4819_SetScanFrequency(sHuntFrequency);
        return;
    }
}

// Returns true the moment a cell finishes being checked (result 1 or 2
// reached) - NOT when a full lap completes. The caller counts these
// completions directly against the real populated-cell count, which is
// simpler and more robust than the previous approach of trying to detect
// wraparound internally (that had a real bug: the search for the next
// populated cell could pass through the lap's starting point without
// ever landing exactly on it, so some cells got checked more than once
// per lap before the wrap was ever detected).
static void Do_GridCheck_Cycle(void) {
    static uint16_t next_slot = 0;
    static uint16_t checking_idx = 0xFFFF;

    if (gInterceptorActiveFrequency != 0) {
        gInterceptorCheckingSlot = -1;
        Handle_Active_Channel_Dwell();
        return;
    }

    if (checking_idx == 0xFFFF) {
        for (uint16_t tries = 0; tries < GRID_TOTAL_SLOTS; tries++) {
            uint16_t idx = next_slot;
            next_slot = (next_slot + 1) % GRID_TOTAL_SLOTS;
            if (gScanList[idx].Frequency != 0 && !gScanList[idx].Muted) {
                checking_idx = idx;
                break;
            }
        }
        if (checking_idx == 0xFFFF) { gInterceptorCheckingSlot = -1; return; }
    }

    gInterceptorCheckingSlot = (int16_t)checking_idx;
    gUpdateDisplay = true;

    uint8_t result = Check_Candidate_Frequency(&sGridCheckState, gScanList[checking_idx].Frequency, gScanList[checking_idx].CodeType, gScanList[checking_idx].Code);
    if (result == 0) return; // still settling

    if (result == 1) {
        if (gScanList[checking_idx].HitCount < 255) gScanList[checking_idx].HitCount++;
        gInterceptorActiveFrequency = gScanList[checking_idx].Frequency;
        gInterceptorLastActiveSlot = (int16_t)checking_idx; // tick-mark moves, cursor doesn't
        // Cursor does NOT snap to the active cell - cursor stays where
        // the user left it so the taskbar always shows their chosen
        // cell's frequency, not whatever the scanner last heard.
        APP_StartListening(FUNCTION_RECEIVE);
        gUpdateDisplay = true;
    }
    gInterceptorCheckingSlot = -1;
    checking_idx = 0xFFFF;
}

// One step of a full grid pass, used by sweep mode. Returns true when the
// pass is finished (every populated cell checked once, a cell locked on,
// or we're dwelling).
//
// The design is deliberately the simplest thing that can be correct:
// sGridPassIdx only ever moves FORWARD, from 0 to GRID_TOTAL_SLOTS. Every
// populated cell is therefore visited exactly once, and "pass complete"
// is simply "index reached the end". Earlier versions tried to detect a
// complete lap via index wraparound or by counting completions against a
// target, and both had subtle bugs that let cells be checked twice or the
// pass never finish at all.
static bool Do_GridCheck_Pass(void) {
    if (gInterceptorActiveFrequency != 0) {
        gInterceptorCheckingSlot = -1;
        Handle_Active_Channel_Dwell();
        return true; // dwelling on a signal - pass is over
    }

    // Skip forward over empty and muted cells.
    while (sGridPassIdx < GRID_TOTAL_SLOTS
           && (gScanList[sGridPassIdx].Frequency == 0 || gScanList[sGridPassIdx].Muted))
        sGridPassIdx++;

    if (sGridPassIdx >= GRID_TOTAL_SLOTS) {
        gInterceptorCheckingSlot = -1;
        return true; // walked the whole grid - pass complete
    }

    gInterceptorCheckingSlot = (int16_t)sGridPassIdx;
    gUpdateDisplay = true;

    uint8_t result = Check_Candidate_Frequency(&sGridCheckState,
        gScanList[sGridPassIdx].Frequency,
        gScanList[sGridPassIdx].CodeType,
        gScanList[sGridPassIdx].Code);

    if (result == 0) return false; // still settling on this cell - stay put

    if (result == 1) {
        if (gScanList[sGridPassIdx].HitCount < 255) gScanList[sGridPassIdx].HitCount++;
        gInterceptorActiveFrequency = gScanList[sGridPassIdx].Frequency;
        gInterceptorLastActiveSlot = (int16_t)sGridPassIdx; // tick-mark moves, cursor doesn't
        APP_StartListening(FUNCTION_RECEIVE);
        gUpdateDisplay = true;
        gInterceptorCheckingSlot = -1;
        return true; // locked on - pass ends here, dwell takes over
    }

    // result == 2: nothing on this cell - advance to the next one.
    sGridPassIdx++;
    return false;
}

static void Do_FastGridScan_Cycle(void) {
    static bool use_A = true;
    static int16_t index_A = -2;
    static int16_t index_B = -1;
    static int16_t checking_idx = -1;

    if (gInterceptorActiveFrequency != 0) {
        gInterceptorCheckingSlot = -1;
        Handle_Active_Channel_Dwell();
        return;
    }

    if (checking_idx == -1) {
        int16_t *idx = use_A ? &index_A : &index_B;
        use_A = !use_A;

        for (uint16_t tries = 0; tries < GRID_TOTAL_SLOTS; tries++) {
            *idx += 2;
            if (*idx >= GRID_TOTAL_SLOTS) *idx = (idx == &index_A) ? 0 : 1;
            if (gScanList[*idx].Frequency != 0) {
                checking_idx = *idx;
                break;
            }
        }
        if (checking_idx == -1) { gInterceptorCheckingSlot = -1; return; }
    }

    gInterceptorCheckingSlot = checking_idx;
    gUpdateDisplay = true;

    uint8_t result = Check_Candidate_Frequency(&sGridCheckState, gScanList[checking_idx].Frequency, gScanList[checking_idx].CodeType, gScanList[checking_idx].Code);
    if (result == 0) return;

    if (result == 1) {
        if (gScanList[checking_idx].HitCount < 255) gScanList[checking_idx].HitCount++;
        gInterceptorActiveFrequency = gScanList[checking_idx].Frequency;
        gInterceptorLastActiveSlot = (int16_t)checking_idx; // tick-mark moves, cursor doesn't
        // Cursor does NOT snap - stays where user left it.
        APP_StartListening(FUNCTION_RECEIVE);
        gUpdateDisplay = true;
    }
    gInterceptorCheckingSlot = -1;
    checking_idx = -1;
}

static uint8_t Find_Next_Enabled_Band(uint8_t afterIdx) {
    for (uint8_t tries = 0; tries < SWEEP_BAND_COUNT; tries++) {
        afterIdx = (afterIdx + 1) % SWEEP_BAND_COUNT;
        if (gSweepBands[afterIdx].Enabled && gSweepBands[afterIdx].EndFreq > gSweepBands[afterIdx].StartFreq)
            return afterIdx;
    }
    return 0xFF;
}

static void Skip_Excluded_Ranges(uint32_t *freq, uint32_t stepSize) {
    if (gExcludeNoaa && *freq >= NOAA_EXCLUDE_START && *freq <= NOAA_EXCLUDE_END) {
        *freq = NOAA_EXCLUDE_END + stepSize;
    }
}

static void Do_BandSweep_Cycle(void) {
    static uint8_t  sSweepBandIndex = SWEEP_BAND_COUNT - 1;
    static uint32_t sSweepFreq = 0;

    if (gInterceptorActiveFrequency != 0) {
        gInterceptorHuntTickerActive = false;
        Handle_Active_Channel_Dwell();
        return;
    }

    if (sSweepFreq == 0 || gSweepNeedsReinit) {
        gSweepNeedsReinit = false;
        uint8_t next = Find_Next_Enabled_Band(sSweepBandIndex);
        if (next == 0xFF) { sSweepFreq = 0; return; }
        sSweepBandIndex = next;
        sSweepFreq = gSweepBands[next].StartFreq;
        Skip_Excluded_Ranges(&sSweepFreq, gSweepBands[next].StepSize);
    }

    gInterceptorHuntTickerActive = true;
    gInterceptorHuntTickerFreq   = sSweepFreq;

    if (sSweepCheckState.state == CANDCHECK_IDLE) {
        BK4819_SetAF(BK4819_AF_MUTE); // the fast retune below doesn't touch the audio path on its own - mute explicitly so a genuinely active frequency briefly touched during the probe can't leak audio through before we decide whether to commit to it
        if (!Fast_Rssi_Precheck(sSweepFreq)) {
            sSweepFreq += gSweepBands[sSweepBandIndex].StepSize;
            Skip_Excluded_Ranges(&sSweepFreq, gSweepBands[sSweepBandIndex].StepSize);
            if (sSweepFreq > gSweepBands[sSweepBandIndex].EndFreq) {
                uint8_t next = Find_Next_Enabled_Band(sSweepBandIndex);
                if (next == 0xFF) { sSweepFreq = 0; return; }
                sSweepBandIndex = next;
                sSweepFreq = gSweepBands[next].StartFreq;
                Skip_Excluded_Ranges(&sSweepFreq, gSweepBands[next].StepSize);
            }
            return;
        }
    }

    uint8_t result = Check_Candidate_Frequency(&sSweepCheckState, sSweepFreq, CODE_TYPE_OFF, 0);
    if (result == 0) return;

    if (result == 1) {
        bool blacklisted = false;
        for (uint8_t i = 0; i < gLockoutCount; i++)
            if (gLockoutList[i] == sSweepFreq) { blacklisted = true; break; }

        if (!blacklisted) {
            gInterceptorHuntTickerActive = false;
            INTERCEPTOR_LogNewCapture(sSweepFreq, CODE_TYPE_OFF, 0);
            gInterceptorActiveFrequency = sSweepFreq;
            for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
                if (gScanList[i].Frequency == sSweepFreq) {
                    gInterceptorLastActiveSlot = (int16_t)i; // tick-mark, cursor stays put
                    break;
                }
            }
            APP_StartListening(FUNCTION_RECEIVE);
            gUpdateDisplay = true;
            return;
        }
    }

    sSweepFreq += gSweepBands[sSweepBandIndex].StepSize;
    Skip_Excluded_Ranges(&sSweepFreq, gSweepBands[sSweepBandIndex].StepSize);

    if (sSweepFreq > gSweepBands[sSweepBandIndex].EndFreq) {
        uint8_t next = Find_Next_Enabled_Band(sSweepBandIndex);
        if (next == 0xFF) { sSweepFreq = 0; return; }
        sSweepBandIndex = next;
        sSweepFreq = gSweepBands[next].StartFreq;
        Skip_Excluded_Ranges(&sSweepFreq, gSweepBands[next].StepSize);
    }
}

void INTERCEPTOR_Engine_Tick(void) {
    {
        static uint8_t sSavedDualWatch  = 0xFF;
        static uint8_t sSavedCrossBand  = 0xFF;
        if (gScreenToDisplay != DISPLAY_MAIN) {
            if (sSavedDualWatch == 0xFF) sSavedDualWatch = gEeprom.DUAL_WATCH;
            if (sSavedCrossBand == 0xFF) sSavedCrossBand = gEeprom.CROSS_BAND_RX_TX;
            gEeprom.DUAL_WATCH       = DUAL_WATCH_OFF;
            gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
        } else {
            if (sSavedDualWatch != 0xFF) {
                gEeprom.DUAL_WATCH = sSavedDualWatch;
                sSavedDualWatch = 0xFF;
            }
            if (sSavedCrossBand != 0xFF) {
                gEeprom.CROSS_BAND_RX_TX = sSavedCrossBand;
                sSavedCrossBand = 0xFF;
            }
            gFlagReconfigureVfos = true;
        }
    }

    if (gScreenToDisplay != DISPLAY_INTERCEPTOR) {
        Hunt_Reset();
        gInterceptorActiveFrequency = 0;
        gInterceptorFlashSlot = -1;
        gInterceptorFlashCount = 0;
        gInterceptorCheckingSlot = -1;
        gInterceptorHuntTickerActive = false;
        // Clear grid-pass scheduling too - a pass interrupted partway
        // through shouldn't resume mid-walk when we come back.
        sGridPassActive = false;
        sGridPassDue    = false;
        sGridPassIdx    = 0;
        sGridCheckIntervalCounter = 0;
        return;
    }

    if (gCurrentFunction == FUNCTION_TRANSMIT) return;

    // Back off when the radio is genuinely receiving something we didn't
    // initiate - without this, the engine can retune away from a signal
    // mid-detection. FUNCTION_INCOMING is also our OWN success signal
    // while mid-check, so this only applies when we're not currently
    // waiting on a candidate check or already dwelling on a confirmed hit.
    if (gCurrentFunction == FUNCTION_INCOMING
        && sGridCheckState.state != CANDCHECK_WAITING
        && sSweepCheckState.state != CANDCHECK_WAITING
        && gInterceptorActiveFrequency == 0
        && !gInterceptorTxOverrideActive)
        return;

    if (gInterceptorBandSweepActive) {
        // Sweep runs continuously until the 10ms scheduler says a grid
        // pass is owed (once per real second). Then one clean pass runs
        // through every populated cell exactly once, and sweep resumes.
        // All the timing lives in INTERCEPTOR_TimeSlice10ms - this
        // function just does whichever work is currently called for,
        // because it runs at a variable main-loop rate and can't measure
        // real time on its own.
        if (sGridPassActive) {
            if (Do_GridCheck_Pass())
                sGridPassActive = false; // pass finished - back to sweeping
        } else if (sGridPassDue
                   && sSweepCheckState.state == CANDCHECK_IDLE
                   && gInterceptorActiveFrequency == 0) {
            // Only start a pass at a clean boundary - never mid-settle on
            // a sweep candidate, and never while dwelling on a signal.
            sGridPassDue    = false;
            sGridPassActive = true;
            sGridPassIdx    = 0; // start of a fresh forward walk
        } else {
            Do_BandSweep_Cycle();
        }
        return;
    }

    if (gSniffingEnabled) {
        static bool huntOwnsTuner = true;
        static uint16_t huntTurnTicks = 0;
        #define HUNT_MAX_TURN_TICKS 150

        if (huntOwnsTuner) {
            Do_Hunt_Cycle();
            huntTurnTicks++;
            if (sHuntState == HUNT_IDLE) {
                huntOwnsTuner = false;
                huntTurnTicks = 0;
            } else if (huntTurnTicks >= HUNT_MAX_TURN_TICKS) {
                Hunt_Reset();
                huntOwnsTuner = false;
                huntTurnTicks = 0;
            }
        } else {
            Do_GridCheck_Cycle();
            if (sGridCheckState.state == CANDCHECK_IDLE && gInterceptorActiveFrequency == 0)
                huntOwnsTuner = true;
        }
    } else {
        Hunt_Reset();
        Do_FastGridScan_Cycle();
    }
}
