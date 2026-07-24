#ifndef INTERCEPTOR_GRID_H
#define INTERCEPTOR_GRID_H

#include <stdbool.h>
#include <stdint.h>

#define GRID_PAGE_SIZE    9
#define GRID_MAX_PAGES    10                                 // hardware-safe ceiling
#define GRID_TOTAL_SLOTS  (GRID_PAGE_SIZE * GRID_MAX_PAGES)  // 150
#define MAX_LOCKOUTS      20

typedef struct {
    uint32_t Frequency;   // full precision, in the same units as gTxVfo frequencies
    char     Name[7];     // 6 chars + null terminator, empty = show frequency instead
    uint8_t  CodeType;     // CODE_TYPE_OFF / CODE_TYPE_CONTINUOUS_TONE / CODE_TYPE_DIGITAL (see dcs.h)
    uint8_t  Code;         // CTCSS/DCS code index for CodeType, meaningless if CodeType is OFF
    bool     IsLocked;    // manually-added channel: never auto-purged or evicted
    uint8_t  HitCount;    // times detected active while sniffing, used for sorting
    uint8_t  NoiseFlagCount; // consecutive dwells flagged as steady+loud (likely noise) - reset on any normal-variance dwell
} InterceptorChannel_t;

extern InterceptorChannel_t gScanList[GRID_TOTAL_SLOTS];
extern uint32_t gLockoutList[MAX_LOCKOUTS];
extern uint8_t  gLockoutCount;
extern uint8_t  gUserSelectedChannelIndex;
extern uint8_t  gCurrentGridPage;
extern bool     gSniffingEnabled;       // true = hunt+grid-check alternation; false = fast grid-only scan
extern bool     gInterceptorViewActive; // true = grid screen is what's currently shown
extern uint32_t gInterceptorActiveFrequency; // 0 = nothing currently receiving audio
extern uint8_t  gInterceptorMeterPercent;    // 0-100, current sweep meter fill level
extern bool     gInterceptorTxOverrideActive; // true while transmitting on a grid channel via PTT override
extern bool     gInterceptorBandSweepActive;  // true while the F+5 VHF/UHF band sweep is running

// Live "scanning" ticker - shows the frequency currently being evaluated
// by hunt's frequency counter, displayed in the next empty grid cell.
extern bool     gInterceptorHuntTickerActive;
extern uint32_t gInterceptorHuntTickerFreq;

// Brief flash on the cell a new capture just landed in, before it settles
// into its normal display.
extern int8_t   gInterceptorFlashSlot;   // -1 = nothing currently flashing
extern uint8_t  gInterceptorFlashCount;  // remaining flash toggles

// Currently-being-checked cell, for grid-check and fast-scan - inverts
// briefly while a specific saved cell is actively being tested.
extern int16_t  gInterceptorCheckingSlot; // -1 = nothing currently being checked

// --- Band selection for F+5 sweep ---
// Presets grounded in real US allocations, kept within the BK4819's actual
// tunable range (18-630MHz and 760-1300MHz, confirmed in frequencies.c -
// there's a hard gap 630-760MHz that can't be tuned into at all). Slot 8 is
// user-configurable "Manual" - StartFreq/EndFreq both 0 means it hasn't
// been set yet.
#define SWEEP_BAND_COUNT 9
#define SWEEP_MANUAL_BAND_INDEX 8

typedef struct {
    uint32_t StartFreq; // 10Hz units, same convention as everything else here
    uint32_t EndFreq;
    char     Name[12];
    bool     Enabled;   // currently checked for sweeping
} SweepBand_t;

extern SweepBand_t gSweepBands[SWEEP_BAND_COUNT];

// Navigation/state for the band-selection screen itself.
extern uint8_t gBandSelectHighlight;     // which row is highlighted
extern bool    gBandSelectEnteringFreq;  // true while typing the Manual band's frequencies
extern uint8_t gBandSelectEnteringWhich; // 0 = typing start freq, 1 = typing end freq

// Exclude NOAA weather channels (162.400-162.550 MHz, the 7 real NOAA
// channels) from any sweep that would otherwise cover them - independent
// of which preset band contains that range, so it works whether it's
// inside VHF Land Mobile or a custom Manual range that happens to overlap.
#define NOAA_EXCLUDE_START 16240000
#define NOAA_EXCLUDE_END   16255000
extern bool gExcludeNoaa;

// One extra row beyond the 9 bands, for the NOAA-exclude toggle - shared
// between the display and key-handling files.
#define BAND_SELECT_TOTAL_ROWS (SWEEP_BAND_COUNT + 1)
#define BAND_SELECT_NOAA_ROW SWEEP_BAND_COUNT

#endif
