#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/bk4819.h"
#include "driver/system.h"
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
uint32_t gInterceptorActiveFrequency = 0; // 0 = nothing currently receiving audio
uint8_t  gInterceptorMeterPercent = 0;    // 0-100, how full the sweep meter should be
bool     gInterceptorTxOverrideActive = false; // true while transmitting on a grid channel via PTT override

// Moved up here (was declared later, right next to Handle_Active_Channel_Dwell)
// so INTERCEPTOR_DeleteAndBlacklist/DeleteOnly below can also reset this -
// without that, deleting the cell currently being actively dwelled on left
// the engine permanently stuck thinking something was still active, since
// gInterceptorActiveFrequency was never cleared and this reply-wait state
// wasn't either.
static uint16_t sReplyWaitCountdown = 0;
static bool     sWaitingForReply = false;
bool     gInterceptorBandSweepActive = false;
bool     gInterceptorHuntTickerActive = false;
uint32_t gInterceptorHuntTickerFreq = 0;
int8_t   gInterceptorFlashSlot = -1;
uint8_t  gInterceptorFlashCount = 0;
int16_t  gInterceptorCheckingSlot = -1; // -1 = nothing currently being checked

// ---------------------------------------------------------------------
// Checking a candidate frequency: this now properly repoints the REAL
// current RX VFO (gRxVfo), exactly the way this firmware's own built-in
// scanner does it (see app/chFrScanner.c NextFreqChannel), instead of
// calling BK4819_SetFrequency() directly. That earlier approach left the
// rest of the firmware still believing whatever gRxVfo already was (e.g.
// VFO A) was what's being listened to, since gRxVfo itself never changed -
// so real audio and real squelch detection kept reflecting gRxVfo's real
// channel, never whatever frequency we were actually trying to check.
// Detection is now the real gCurrentFunction == FUNCTION_INCOMING signal
// (the actual hardware squelch decision), not a guessed RSSI/noise/glitch
// threshold.
// ---------------------------------------------------------------------

// How long to wait after retuning before trusting FUNCTION_INCOMING as a
// real result - matches this firmware's own real scanner pacing
// (scan_pause_delay_in_6_10ms = 100ms), not an arbitrary guess.
#define CANDIDATE_SETTLE_10MS_TICKS  10

enum { CANDCHECK_IDLE, CANDCHECK_WAITING };

// Bundles one independent settle-timer instance. Band sweep and grid-check
// now run interleaved with each other (see INTERCEPTOR_Engine_Tick), and
// previously shared one single state machine - meaning one process's
// still-settling wait could get silently hijacked mid-sequence by the
// other's call, resulting in a result being credited to whichever
// candidate the *other* process had actually left the radio tuned to.
// Each caller now gets its own instance so they can never collide.
typedef struct {
    uint8_t  state;
    uint16_t settleCountdown;
} CandCheckState_t;

static CandCheckState_t sGridCheckState = { CANDCHECK_IDLE, 0 }; // shared by grid-check + fast-scan (never concurrent with each other)
static CandCheckState_t sSweepCheckState = { CANDCHECK_IDLE, 0 }; // band sweep's own, independent

static void Tune_RxVfo_To(uint32_t freq, uint8_t codeType, uint8_t code) {
    gRxVfo->freq_config_RX.Frequency = freq;
    // This was the actual bug behind audio cutting out after ~1 second
    // unless VFO A already happened to be on the same channel: we were
    // only ever changing the frequency, never the CTCSS/DCS tone-matching
    // fields, so the real squelch decoder kept checking against whatever
    // tone was left over from VFO A's PREVIOUS channel - completely
    // unrelated to whatever grid cell we were actually checking. Initial
    // carrier-open audio would play briefly, then the real tone-mismatch
    // logic would correctly (from its own perspective) re-mute it.
    gRxVfo->freq_config_RX.CodeType = codeType;
    gRxVfo->freq_config_RX.Code     = code;
    // Force narrow bandwidth specifically for our own checks - this wasn't
    // being touched before, so every check inherited whatever bandwidth
    // gRxVfo already had (often WIDE). A wide receive filter means tuning
    // to any one of several closely-spaced saved channels can still pick
    // up the same real signal, which is why different cells were lighting
    // up inconsistently for what should be one consistent transmission.
    gRxVfo->CHANNEL_BANDWIDTH = BK4819_FILTER_BW_NARROW;
    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(true);
}

// Call with the candidate frequency you want checked. Returns:
//   0 = still settling, caller should do nothing else this tick
//   1 = confirmed active (real squelch opened - FUNCTION_INCOMING)
//   2 = confirmed not active, ready to move to the next candidate
// Non-blocking - one step per call, same as everything else here.
static uint8_t Check_Candidate_Frequency(CandCheckState_t *st, uint32_t freq, uint8_t codeType, uint8_t code) {
    if (st->state == CANDCHECK_IDLE) {
        Tune_RxVfo_To(freq, codeType, code);
        st->state           = CANDCHECK_WAITING;
        st->settleCountdown = CANDIDATE_SETTLE_10MS_TICKS;
        return 0;
    }
    if (st->settleCountdown > 0) return 0; // still counting down (see INTERCEPTOR_TimeSlice10ms)

    st->state = CANDCHECK_IDLE;
    if (gCurrentFunction == FUNCTION_INCOMING) {
        // Detecting FUNCTION_INCOMING alone isn't enough - this firmware's
        // own real scanner (CHFRSCANNER_ContinueScanning) explicitly calls
        // APP_StartListening() at this exact point to actually transition
        // into listening, which is what unmutes audio. Without this call,
        // detection works but nothing is ever actually heard - confirmed
        // missing piece.
        APP_StartListening(FUNCTION_RECEIVE);
        return 1;
    }
    return 2;
}

// RSSI-based meter fill, purely cosmetic (how strong does the confirmed-
// active signal look) - this no longer decides whether something counts
// as active at all, that's FUNCTION_INCOMING's job now.
// NOTE: not hardware-calibrated, a starting guess like everything else here.
#define METER_RSSI_MIN   70
#define METER_RSSI_FULL  280

static void Update_Meter_Level(void) {
    uint16_t rssi = BK4819_GetRSSI();
    if (rssi <= METER_RSSI_MIN) {
        gInterceptorMeterPercent = 0;
        return;
    }
    uint32_t span = METER_RSSI_FULL - METER_RSSI_MIN;
    uint32_t over = rssi - METER_RSSI_MIN;
    // Real-world testing showed this never moved below ~30% or above
    // ~50% - meaning actual signals only ever occupy a narrow middle
    // slice of the assumed RSSI range. Stretching by 3x here spreads that
    // same real range across roughly 0-100% instead, same approach as the
    // TX meter's gain fix - adjust further if it's still not dramatic
    // enough, or if it now pins to 100% too easily.
    uint32_t pct  = (over * 100 * 3) / span;
    gInterceptorMeterPercent = (pct > 100) ? 100 : (uint8_t)pct;
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

// How close two frequencies need to be to count as "the same channel" for
// duplicate detection. Measurement jitter can round the same real signal
// to a slightly different value from one detection to the next - without
// this tolerance, that jitter looks like a brand-new frequency every time.
// 10000 = 100kHz in this firmware's 10Hz-per-count units - a starting
// guess, not hardware-calibrated.
#define FREQ_DEDUP_TOLERANCE  10000

static uint16_t sLastEvictedSlot = 0xFFFF;

void INTERCEPTOR_LogNewCapture(uint32_t freq, uint8_t codeType, uint8_t code) {
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
        uint32_t existing = gScanList[i].Frequency;
        if (existing == 0) continue;
        uint32_t delta = (existing > freq) ? (existing - freq) : (freq - existing);
        if (delta <= FREQ_DEDUP_TOLERANCE) return; // close enough to count as already-have-it
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
        // If the lowest-hit-count slot is the exact same one evicted last
        // time, and something else ties with it, prefer the other one -
        // otherwise a run of near-simultaneous new detections can keep
        // thrashing the same slot over and over instead of spreading out.
        if (target == sLastEvictedSlot) {
            for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
                if (gScanList[i].IsLocked || i == sLastEvictedSlot) continue;
                if (gScanList[i].HitCount == lowest_hits) { target = i; break; }
            }
        }
        if (target == 0xFFFF) return; // every slot is locked, nothing to evict
    }

    sLastEvictedSlot = target;
    memset(&gScanList[target], 0, sizeof(InterceptorChannel_t));
    gScanList[target].Frequency = freq;
    gScanList[target].CodeType  = codeType;
    gScanList[target].Code      = code;
    gInterceptorFlashSlot  = (int8_t)target;
    gInterceptorFlashCount = 6; // a few blink cycles before settling to normal display
    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP); // always plays - not gated by the global beep setting
    gUpdateDisplay = true; // new capture won't show up on screen otherwise
}

void INTERCEPTOR_DeleteAndBlacklist(uint16_t slotIndex) {
    if (slotIndex >= GRID_TOTAL_SLOTS) return;
    if (gScanList[slotIndex].Frequency == 0) return; // nothing there

    uint32_t freq = gScanList[slotIndex].Frequency;

    if (!gScanList[slotIndex].IsLocked) {
        // only auto-detected slots get blacklisted - a manually-added crew
        // channel being removed just means "not monitoring it this event",
        // not "this frequency is noise"
        if (gLockoutCount < MAX_LOCKOUTS) {
            gLockoutList[gLockoutCount++] = freq;
        } else {
            // list is full - overwrite the oldest entry rather than
            // silently refusing to blacklist anything further
            memmove(&gLockoutList[0], &gLockoutList[1], (MAX_LOCKOUTS - 1) * sizeof(uint32_t));
            gLockoutList[MAX_LOCKOUTS - 1] = freq;
        }
    }

    memset(&gScanList[slotIndex], 0, sizeof(InterceptorChannel_t));

    // If this was the cell currently being actively dwelled on (e.g. a
    // persistent noise source that never lets up), clear the dwell state
    // too - otherwise the engine gets stuck forever thinking something is
    // still active on a frequency that no longer exists in the grid, and
    // scanning never resumes no matter what mode you toggle afterward.
    if (gInterceptorActiveFrequency == freq) {
        gInterceptorActiveFrequency = 0;
        sWaitingForReply = false;
    }

    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
}

// Just clears the slot - no blacklist entry at all, even for auto-detected
// channels. Distinct from INTERCEPTOR_DeleteAndBlacklist: this is for "get
// rid of this quickly," that one is for "this is noise, never show it again."
void INTERCEPTOR_DeleteOnly(uint16_t slotIndex) {
    if (slotIndex >= GRID_TOTAL_SLOTS) return;
    if (gScanList[slotIndex].Frequency == 0) return; // nothing there

    uint32_t freq = gScanList[slotIndex].Frequency;
    memset(&gScanList[slotIndex], 0, sizeof(InterceptorChannel_t));

    if (gInterceptorActiveFrequency == freq) {
        gInterceptorActiveFrequency = 0;
        sWaitingForReply = false;
    }

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
#define METER_REDRAW_10MS_TICKS 10  // ~100ms between meter redraws
#define TICKER_REDRAW_10MS_TICKS 15 // ~150ms between ticker/flash redraws

static uint16_t sMeterRedrawCountdown = 0;
static uint16_t sTickerRedrawCountdown = 0;

// Called once per real 10ms system tick from APP_TimeSlice10ms().
void INTERCEPTOR_TimeSlice10ms(void) {
    if (sGridCheckState.state == CANDCHECK_WAITING && sGridCheckState.settleCountdown > 0)
        sGridCheckState.settleCountdown--;
    if (sSweepCheckState.state == CANDCHECK_WAITING && sSweepCheckState.settleCountdown > 0)
        sSweepCheckState.settleCountdown--;

    if (sWaitingForReply && sReplyWaitCountdown > 0) {
        sReplyWaitCountdown--;
        if (sReplyWaitCountdown == 0) {
            sWaitingForReply = false;
            gInterceptorActiveFrequency = 0; // give up waiting, resume scanning
            INTERCEPTOR_SortByPopularity();
            gUpdateDisplay = true; // clear the invert highlight from screen
        }
    }

    // While a channel is actively being listened to, redraw the meter at a
    // throttled rate (not every single main-loop tick, which would hammer
    // the screen far more than needed) so the sweep visibly animates with
    // the live signal level.
    if (gInterceptorActiveFrequency != 0 || gInterceptorTxOverrideActive) {
        if (gInterceptorTxOverrideActive) {
            // TX: sample live mic/modulation level instead of RX RSSI.
            // Simple linear scaling - this firmware's own real mic-bar
            // feature (ui/main.c) uses a non-linear sqrt curve for better
            // low-volume sensitivity, but that helper isn't exposed
            // anywhere we can reuse it from, so this is a simpler
            // approximation using the same underlying real function.
            uint16_t amp = BK4819_GetVoiceAmplitudeOut();
            // The real mic-bar feature (ui/main.c) multiplies this reading
            // by 8, which is where this started - but that wasn't enough
            // for normal speech on this radio in practice, so this has been
            // bumped up further based on actual hardware testing (8 -> 32
            // -> 64). Adjust further if still not sensitive enough.
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
                // The normal gUpdateDisplay -> GUI_DisplayScreen() path
                // doesn't seem to actually refresh the screen during an
                // active transmission - this firmware's own real mic-bar
                // feature has the same problem and works around it by
                // calling its draw function and blitting directly instead
                // of relying on the flag. Mirroring that here.
                UI_DisplayInterceptorGridPage();
            } else {
                gUpdateDisplay = true;
            }
        }
    }

    // Live hunt ticker and new-capture flash - both need periodic redraws
    // to actually animate, same throttled-real-tick approach as the meter
    // above, kept separate since neither of these cares about TX/RX level.
    if (gInterceptorHuntTickerActive || gInterceptorFlashCount > 0) {
        if (sTickerRedrawCountdown > 0) {
            sTickerRedrawCountdown--;
        } else {
            sTickerRedrawCountdown = TICKER_REDRAW_10MS_TICKS;
            if (gInterceptorFlashCount > 0) gInterceptorFlashCount--;
            gUpdateDisplay = true;
        }
    }
}

// Shared by both scan modes: call while dwelling on gInterceptorActiveFrequency.
// Returns true if the caller should keep dwelling (stay tuned, do nothing else
// this tick), false once it's truly time to resume scanning. Uses the real
// gCurrentFunction state - gRxVfo is already pointed at this frequency from
// when it was first confirmed active, so real reception continues on its own;
// this is just bookkeeping for when to give up and move on.
static bool Handle_Active_Channel_Dwell(void) {
    if (gCurrentFunction == FUNCTION_INCOMING || gCurrentFunction == FUNCTION_RECEIVE) {
        sWaitingForReply = false;
        return true;
    }

    if (!sWaitingForReply) {
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
// This part still uses the BK4819 frequency-counter directly (a genuinely
// different hardware feature - measuring an unknown nearby signal's exact
// frequency - not "is gRxVfo's frequency active", so it isn't affected by
// the gRxVfo-conflict this rewrite fixes elsewhere).
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
        // This firmware's own real scanner always calls this before
        // enabling the frequency counter - it selects the correct RF
        // front-end filter path for whatever frequency range is currently
        // in use. We were missing this entirely, which is the confirmed
        // cause of harmonic/image frequency misdetection (e.g. capturing
        // a transmitted frequency as roughly double its real value).
        BK4819_PickRXFilterPathBasedOnFrequency(gRxVfo->pRX->Frequency);
        BK4819_EnableFrequencyScan();
        sHuntState = HUNT_FREQ;
        sHuntStableCount = 0;
        return;
    }

    if (sHuntState == HUNT_FREQ) {
        uint32_t result;
        if (!BK4819_GetFrequencyScanResult(&result)) {
            return;
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
            BK4819_EnableFrequencyScan();
            return;
        }

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
        gInterceptorHuntTickerActive = false; // stopped on a stable candidate - not "scrolling" anymore
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

// One "grid check" cycle: properly repoints gRxVfo to the next saved slot
// in sequence (see Check_Candidate_Frequency) and waits for the real
// squelch decision, rather than directly polling BK4819 registers while
// gRxVfo still points elsewhere. A static cursor advances each call so
// repeated calls walk through the whole grid over time. On a hit, re-sorts
// by popularity so frequently-active channels bubble toward the top.
static void Do_GridCheck_Cycle(void) {
    static uint16_t next_slot = 0;
    static uint16_t checking_idx = 0xFFFF;

    if (gInterceptorActiveFrequency != 0) {
        gInterceptorCheckingSlot = -1;
        Handle_Active_Channel_Dwell();
        return;
    }

    if (checking_idx == 0xFFFF) {
        // pick the next populated slot to check
        for (uint16_t tries = 0; tries < GRID_TOTAL_SLOTS; tries++) {
            uint16_t idx = next_slot;
            next_slot = (next_slot + 1) % GRID_TOTAL_SLOTS;
            if (gScanList[idx].Frequency != 0) {
                checking_idx = idx;
                break;
            }
        }
        if (checking_idx == 0xFFFF) { gInterceptorCheckingSlot = -1; return; } // grid is empty
    }

    gInterceptorCheckingSlot = (int16_t)checking_idx;
    gUpdateDisplay = true;

    uint8_t result = Check_Candidate_Frequency(&sGridCheckState, gScanList[checking_idx].Frequency, gScanList[checking_idx].CodeType, gScanList[checking_idx].Code);
    if (result == 0) return; // still settling

    if (result == 1) {
        if (gScanList[checking_idx].HitCount < 255) gScanList[checking_idx].HitCount++;
        gInterceptorActiveFrequency = gScanList[checking_idx].Frequency;
        gUpdateDisplay = true;
    }
    gInterceptorCheckingSlot = -1;
    checking_idx = 0xFFFF; // done with this one either way, pick a new one next time
}

// Fast grid-only scan used when sniffing is OFF: alternates "VFO A" across
// even slots and "VFO B" across odd slots, checking one at a time using
// the same real gRxVfo-based method as grid-check above.
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
        if (checking_idx == -1) { gInterceptorCheckingSlot = -1; return; } // grid is empty
    }

    gInterceptorCheckingSlot = checking_idx;
    gUpdateDisplay = true;

    uint8_t result = Check_Candidate_Frequency(&sGridCheckState, gScanList[checking_idx].Frequency, gScanList[checking_idx].CodeType, gScanList[checking_idx].Code);
    if (result == 0) return;

    if (result == 1) {
        if (gScanList[checking_idx].HitCount < 255) gScanList[checking_idx].HitCount++;
        gInterceptorActiveFrequency = gScanList[checking_idx].Frequency;
        gUpdateDisplay = true;
    }
    gInterceptorCheckingSlot = -1;
    checking_idx = -1;
}

// ---------------------------------------------------------------------
// Band sweep: steps across the real commercial VHF and UHF land-mobile
// bands (confirmed via FCC.gov: 150-174 MHz VHF, 450-470 MHz UHF - not the
// ham bands), continuously alternating between the two. Uses the same real
// gRxVfo-based check as grid-check/fast-scan now, so it correctly dwells
// and lets you actually hear what it finds instead of silently logging and
// moving on.
// ---------------------------------------------------------------------

#define SWEEP_VHF_START  15000000  // 150.00000 MHz
#define SWEEP_VHF_END    17400000  // 174.00000 MHz
#define SWEEP_UHF_START  45000000  // 450.00000 MHz
#define SWEEP_UHF_END    47000000  // 470.00000 MHz
// 12.5 kHz, not 25 kHz - confirmed via FCC.gov: since Jan 1 2013, Part 90
// licensees in these exact bands (150-174 MHz, 421-470 MHz) are required
// to operate at 12.5kHz efficiency. A 25kHz step would silently skip
// roughly half of all real, legally-operating channels - not slower,
// genuinely never tested at all. This doubles total sweep time versus the
// old 25kHz step, but that's the real tradeoff for actually covering the
// channels that exist.
#define SWEEP_STEP_SIZE  1250

static void Do_BandSweep_Cycle(void) {
    static uint32_t sSweepFreq = SWEEP_VHF_START;
    static bool     sSweepInVhf = true;

    if (gInterceptorActiveFrequency != 0) {
        gInterceptorHuntTickerActive = false;
        Handle_Active_Channel_Dwell();
        return;
    }

    gInterceptorHuntTickerActive = true;
    gInterceptorHuntTickerFreq   = sSweepFreq;

    uint8_t result = Check_Candidate_Frequency(&sSweepCheckState, sSweepFreq, CODE_TYPE_OFF, 0);
    if (result == 0) return; // still settling

    if (result == 1) {
        bool blacklisted = false;
        for (uint8_t i = 0; i < gLockoutCount; i++)
            if (gLockoutList[i] == sSweepFreq) { blacklisted = true; break; }

        if (!blacklisted) {
            gInterceptorHuntTickerActive = false;
            INTERCEPTOR_LogNewCapture(sSweepFreq, CODE_TYPE_OFF, 0); // no tone info at this sweep speed
            gInterceptorActiveFrequency = sSweepFreq;
            gUpdateDisplay = true;
            return; // stay here - don't advance yet, we're dwelling now
        }
    }

    sSweepFreq += SWEEP_STEP_SIZE;

    if (sSweepInVhf && sSweepFreq > SWEEP_VHF_END) {
        sSweepInVhf = false;
        sSweepFreq  = SWEEP_UHF_START;
    } else if (!sSweepInVhf && sSweepFreq > SWEEP_UHF_END) {
        sSweepInVhf = true;
        sSweepFreq  = SWEEP_VHF_START;
    }
}

void INTERCEPTOR_Engine_Tick(void) {
    // This firmware's real Dual Watch feature retunes the radio on its own
    // independent timer, completely regardless of what screen is showing
    // (confirmed in app/app.c - none of its conditions check gScreenToDisplay
    // at all). Save/restore runs first, unconditionally, every tick - not
    // gated behind the grid-screen check below - so restoration on
    // returning to the main screen always happens even on the very tick
    // we're about to stop scanning.
    //
    // Cross-Band RX/TX is handled the same way, following this firmware's
    // own real precedent - the built-in scanner (CHFRSCANNER_Start/Stop)
    // does exactly this same save-disable-restore around its own scanning.
    {
        static uint8_t sSavedDualWatch  = 0xFF; // 0xFF = nothing currently saved
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
            // We directly overwrite gRxVfo->CHANNEL_BANDWIDTH during our own
            // checks (see Tune_RxVfo_To) - force the same real, full
            // reconfigure this firmware already uses elsewhere (see
            // End_Interceptor_PTT) so bandwidth and everything else gets
            // properly re-derived from the actual stored channel settings,
            // not just left overwritten from our last check.
            gFlagReconfigureVfos = true;
        }
    }

    // Grid mode IS the scanning - it should only actually run while the
    // grid screen is genuinely showing. Previously the engine kept
    // scanning in the background regardless of which screen was displayed,
    // which meant leaving the grid screen didn't actually stop it, still
    // risking exactly the kind of conflict the Dual Watch fix above was
    // meant to prevent. Also gives the status bar's "S" indicator (see
    // ui/status.c) something exact and honest to key off of.
    if (gScreenToDisplay != DISPLAY_INTERCEPTOR) {
        Hunt_Reset();
        gInterceptorActiveFrequency = 0;
        gInterceptorFlashSlot = -1;
        gInterceptorFlashCount = 0;
        gInterceptorCheckingSlot = -1;
        gInterceptorHuntTickerActive = false;
        return;
    }

    // Never interrupt an actual live transmission.
    if (gCurrentFunction == FUNCTION_TRANSMIT)
        return;

    // FUNCTION_INCOMING is now also OUR OWN success signal while we're
    // mid-check (see Check_Candidate_Frequency) - only treat it as "leave
    // this alone" when we're NOT the one who caused it, i.e. when we're
    // not currently waiting on a candidate check or already dwelling on a
    // confirmed hit.
    if (gCurrentFunction == FUNCTION_INCOMING
        && sCandCheckState != CANDCHECK_WAITING
        && gInterceptorActiveFrequency == 0
        && !gInterceptorTxOverrideActive)
        return;

    if (gInterceptorBandSweepActive) {
        static bool sweepOwnsTuner = true;

        // Whichever process is currently mid-settle keeps the radio until
        // it actually finishes (accepted or rejected) - both processes
        // retune the SAME physical radio, so swapping every raw tick
        // regardless of settle-in-progress let one yank the hardware away
        // from the other mid-wait, corrupting whichever check was running
        // even with separate software state for each.
        if (sweepOwnsTuner) {
            Do_BandSweep_Cycle();
            if (sSweepCheckState.state == CANDCHECK_IDLE && gInterceptorActiveFrequency == 0)
                sweepOwnsTuner = false;
        } else {
            Do_GridCheck_Cycle();
            if (sGridCheckState.state == CANDCHECK_IDLE && gInterceptorActiveFrequency == 0)
                sweepOwnsTuner = true;
        }
        return; // mutually exclusive with sniffing/fast-scan below
    }

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
