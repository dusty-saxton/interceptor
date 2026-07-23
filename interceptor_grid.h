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

#endif
