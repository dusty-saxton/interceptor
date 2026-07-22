#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "audio.h"
#include "functions.h"
#include "dcs.h"
#include <string.h>

InterceptorChannel_t gScanList[GRID_TOTAL_SLOTS] = {0};
uint32_t gLockoutList[MAX_LOCKOUTS] = {0};
uint8_t  gLockoutCount = 0;
uint8_t  gUserSelectedChannelIndex = 0;
uint8_t  gCurrentGridPage = 0;
bool     gSniffingEnabled = false;
bool     gInterceptorViewActive = false;
uint32_t gInterceptorActiveFrequency = 0; // 0 = nothing currently receiving audio

// RSSI threshold used as a base activity gate for the grid-check cycle
// (checking already-saved channels). NOTE: 70 is a starting guess - adjust
// based on how it behaves on your actual radio.
#define ACTIVITY_RSSI_THRESHOLD  70

// Noise/glitch thresholds grounded in this firmware's own built-in squelch
// calibration defaults (see radio.c: SquelchOpenNoiseThresh ~65-90,
// SquelchOpenGlitchThresh ~100, both "lower = cleaner signal").
#define ACTIVITY_NOISE_MAX   90
#define ACTIVITY_GLITCH_MAX  100

static bool Is_Frequency_Active(void) {
    if (BK4819_GetRSSI() < ACTIVITY_RSSI_THRESHOLD) return false;
    if (BK4819_GetExNoiceIndicator() > ACTIVITY_NOISE_MAX) return false;
    if (BK4819_GetGlitchIndicator() > ACTIVITY_GLITCH_MAX) return false;
    return true;
}

uint8_t INTERCEPTOR_GetUsedPageCount(void) {
    for (int16_t i = GRID_TOTAL_SLOTS - 1; i >= 0; i--) {
        if (gScanList[i].Frequency != 0)
            return (uint8_t)(i / GRID_PAGE_SIZE) + 1;
    }
    return 1; // always show at least page 1, even if empty
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

void INTERCEPTOR_LogNewCapture(uint32_t freq, uint8_t codeType, uint8_t code) {
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++)
        if (gScanList[i].Frequency == freq) return; // already have it

    uint16_t target = 0xFFFF;
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
        if (gScanList[i].Frequency == 0) { target = i; break; }
    }
    if (target == 0xFFFF) {
        uint8_t lowest_hits = 255;
        for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
            if (!gScanList[i].IsLocked && gScanList[i].HitCount < lowest_hits) {
                lowest_hits = gScanList[i].HitCount;
                target = i;
            }
        }
        if (target == 0xFFFF) return; // every slot is locked, nothing to evict
    }

    memset(&gScanList[target], 0, sizeof(InterceptorChannel_t));
    gScanList[target].Frequency = freq;
    gScanList[target].CodeType  = codeType;
    gScanList[target].Code      = code;
    AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);
}

void INTERCEPTOR_DeleteAndBlacklist(uint16_t slotIndex) {
    if (slotIndex >= GRID_TOTAL_SLOTS) return;
    if (gScanList[slotIndex].Frequency == 0) return; // nothing there

    if (!gScanList[slotIndex].IsLocked) {
        // only auto-detected slots get blacklisted - a manually-added crew
        // channel being removed just means "not monitoring it this event",
        // not "this frequency is noise"
        if (gLockoutCount < MAX_LOCKOUTS) {
            gLockoutList[gLockoutCount++] = gScanList[slotIndex].Frequency;
        } else {
            // list is full - overwrite the oldest entry rather than
            // silently refusing to blacklist anything further
            memmove(&gLockoutList[0], &gLockoutList[1], (MAX_LOCKOUTS - 1) * sizeof(uint32_t));
            gLockoutList[MAX_LOCKOUTS - 1] = gScanList[slotIndex].Frequency;
        }
    }

    memset(&gScanList[slotIndex], 0, sizeof(InterceptorChannel_t));
    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
}

// ---------------------------------------------------------------------
// Carrier-pause scan behavior: when a channel goes active, stay on it and
// keep listening. When it goes quiet, don't resume scanning immediately -
// wait a few seconds for a possible reply first, same as this firmware's
// real carrier-pause scan mode. The wait is timed against a genuine 10ms
// system tick (see INTERCEPTOR_TimeSlice10ms, called from
// APP_TimeSlice10ms) rather than raw loop iterations, since loop speed
// varies with whatever else the radio is doing.
// ---------------------------------------------------------------------

#define REPLY_WAIT_10MS_TICKS  450  // ~4.5 seconds

static uint16_t sReplyWaitCountdown = 0;
static bool     sWaitingForReply = false;

// Called once per real 10ms system tick from APP_TimeSlice10ms().
void INTERCEPTOR_TimeSlice10ms(void) {
    if (sWaitingForReply && sReplyWaitCountdown > 0) {
        sReplyWaitCountdown--;
        if (sReplyWaitCountdown == 0) {
            sWaitingForReply = false;
            gInterceptorActiveFrequency = 0; // give up waiting, resume scanning
            INTERCEPTOR_SortByPopularity();
        }
    }
}

// Shared by both scan modes: call while dwelling on gInterceptorActiveFrequency.
// Returns true if the caller should keep dwelling (stay tuned, do nothing else
// this tick), false once it's truly time to resume scanning.
static bool Handle_Active_Channel_Dwell(void) {
    if (Is_Frequency_Active()) {
        // still talking (or a reply just started) - cancel any wait-timer
        // and keep listening
        sWaitingForReply = false;
        return true;
    }

    if (!sWaitingForReply) {
        // carrier just dropped - start waiting for a possible reply instead
        // of immediately resuming scan
        sWaitingForReply = true;
        sReplyWaitCountdown = REPLY_WAIT_10MS_TICKS;
        return true;
    }

    // already waiting - INTERCEPTOR_TimeSlice10ms() handles the countdown
    // and will clear gInterceptorActiveFrequency once it hits zero
    return gInterceptorActiveFrequency != 0;
}

// Hunt: non-blocking, one poll per call, mirroring this firmware's own
// built-in scanner (app/scanner.c). Requires the same frequency 3 times in
// a row before accepting it, then polls for a CTCSS/DCS tone the same way.
enum { HUNT_IDLE, HUNT_FREQ, HUNT_CSS };
static uint8_t  sHuntState = HUNT_IDLE;
static uint32_t sHuntFrequency = 0;
static uint8_t  sHuntStableCount = 0;
static uint8_t  sHuntCssAttempts = 0;
static uint8_t  sHuntCssResultType = CODE_TYPE_OFF;
static uint8_t  sHuntCssResultCode = 0;

// How many consecutive-poll attempts to spend looking for a CTCSS/DCS tone
// before giving up and treating the channel as plain/no-tone. Analog
// channels with no sub-tone would otherwise hunt forever here.
#define CSS_MAX_ATTEMPTS  20

static void Hunt_Reset(void) {
    BK4819_DisableFrequencyScan();
    sHuntState = HUNT_IDLE;
    sHuntStableCount = 0;
    sHuntCssAttempts = 0;
    sHuntCssResultType = CODE_TYPE_OFF;
    sHuntCssResultCode = 0;
}

static void Do_Hunt_Cycle(void) {
    if (sHuntState == HUNT_IDLE) {
        BK4819_EnableFrequencyScan();
        sHuntState = HUNT_FREQ;
        sHuntStableCount = 0;
        return; // one poll started, come back next hunt-tick
    }

    if (sHuntState == HUNT_FREQ) {
        uint32_t result;
        if (!BK4819_GetFrequencyScanResult(&result)) {
            return; // nothing yet this tick - try again next hunt-tick
        }

        int32_t delta = (int32_t)result - (int32_t)sHuntFrequency;
        sHuntFrequency = result;
        if (delta < 0) delta = -delta;

        BK4819_DisableFrequencyScan();

        if (delta < 100) {
            sHuntStableCount++;
        } else {
            sHuntStableCount = 0;
        }

        if (sHuntStableCount < 3) {
            BK4819_EnableFrequencyScan(); // keep polling for stability
            return;
        }

        // frequency is stable - move on to checking for a tone
        bool blacklisted = false;
        for (uint8_t i = 0; i < gLockoutCount; i++)
            if (gLockoutList[i] == sHuntFrequency) { blacklisted = true; break; }

        if (blacklisted) {
            Hunt_Reset();
            return;
        }

        BK4819_SetScanFrequency(sHuntFrequency);
        sHuntCssAttempts = 0;
        sHuntState = HUNT_CSS;
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
            // no tone found in a reasonable number of tries - treat as a
            // plain channel with no sub-tone rather than hunting forever
            INTERCEPTOR_LogNewCapture(sHuntFrequency, CODE_TYPE_OFF, 0);
            Hunt_Reset();
            return;
        }

        BK4819_SetScanFrequency(sHuntFrequency); // keep polling for a tone
        return;
    }
}

// One "grid check" cycle: briefly tune to the next slot in sequence and
// see if it's currently active. A static cursor advances each call so
// repeated calls walk through the whole grid over time. On a hit, re-sorts
// by popularity so frequently-active channels bubble toward the top.
static void Do_GridCheck_Cycle(void) {
    static uint16_t next_slot = 0;

    if (gInterceptorActiveFrequency != 0) {
        Handle_Active_Channel_Dwell();
        return;
    }

    for (uint16_t tries = 0; tries < GRID_TOTAL_SLOTS; tries++) {
        uint16_t idx = next_slot;
        next_slot = (next_slot + 1) % GRID_TOTAL_SLOTS;

        if (gScanList[idx].Frequency != 0) {
            BK4819_SetFrequency(gScanList[idx].Frequency);
            SYSTEM_DelayMs(5); // brief settle - PLL needs some lock time after retuning
            if (Is_Frequency_Active()) {
                if (gScanList[idx].HitCount < 255) gScanList[idx].HitCount++;
                gInterceptorActiveFrequency = gScanList[idx].Frequency; // start dwelling
            }
            return;
        }
    }
    // grid is empty - nothing to check this cycle
}

// Fast grid-only scan used when sniffing is OFF: alternates "VFO A" across
// even slots and "VFO B" across odd slots, minimal delay, no hunting for
// new signals. On a hit, re-sorts by popularity same as above.
static void Do_FastGridScan_Cycle(void) {
    static bool use_A = true;
    static int16_t index_A = -2;
    static int16_t index_B = -1;

    if (gInterceptorActiveFrequency != 0) {
        Handle_Active_Channel_Dwell();
        return;
    }

    int16_t *idx = use_A ? &index_A : &index_B;
    use_A = !use_A;

    for (uint16_t tries = 0; tries < GRID_TOTAL_SLOTS; tries++) {
        *idx += 2;
        if (*idx >= GRID_TOTAL_SLOTS) *idx = (idx == &index_A) ? 0 : 1;

        if (gScanList[*idx].Frequency != 0) {
            BK4819_SetFrequency(gScanList[*idx].Frequency);
            SYSTEM_DelayMs(5); // as fast as reasonably possible
            if (Is_Frequency_Active()) {
                if (gScanList[*idx].HitCount < 255) gScanList[*idx].HitCount++;
                gInterceptorActiveFrequency = gScanList[*idx].Frequency;
            }
            return;
        }
    }
}

void INTERCEPTOR_Engine_Tick(void) {
    // Never retune the radio away from an actual live transmission or
    // reception - these are this firmware's real states for "someone is
    // currently talking" (FUNCTION_INCOMING) and "we're currently
    // transmitting" (FUNCTION_TRANSMIT).
    if (gCurrentFunction == FUNCTION_TRANSMIT || gCurrentFunction == FUNCTION_INCOMING)
        return;

    if (gSniffingEnabled) {
        static bool do_hunt_next = true;
        if (do_hunt_next) Do_Hunt_Cycle();
        else               Do_GridCheck_Cycle();
        do_hunt_next = !do_hunt_next;
    } else {
        Hunt_Reset(); // sniffing is off - make sure hunt state doesn't linger
        Do_FastGridScan_Cycle();
    }
}
