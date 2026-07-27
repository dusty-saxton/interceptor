#ifndef INTERCEPTOR_GRID_H
#define INTERCEPTOR_GRID_H

#include <stdbool.h>
#include <stdint.h>

#define GRID_PAGE_SIZE    9
#define GRID_MAX_PAGES    10
#define GRID_TOTAL_SLOTS  (GRID_PAGE_SIZE * GRID_MAX_PAGES)
#define MAX_LOCKOUTS      20

typedef struct {
    uint32_t Frequency;
    char     Name[7];
    uint8_t  CodeType;
    uint8_t  Code;
    bool     IsLocked;
    uint8_t  HitCount;
    uint8_t  NoiseFlagCount;
    uint8_t  NoiseFlagLevel;
    bool     Muted;
} InterceptorChannel_t;

extern InterceptorChannel_t gScanList[GRID_TOTAL_SLOTS];
extern uint32_t gLockoutList[MAX_LOCKOUTS];
extern uint8_t  gLockoutCount;
extern uint8_t  gUserSelectedChannelIndex;
extern uint8_t  gCurrentGridPage;
extern bool     gSniffingEnabled;
extern bool     gInterceptorViewActive;
extern uint32_t gInterceptorActiveFrequency;
extern uint8_t  gInterceptorMeterPercent;
extern bool     gInterceptorTxOverrideActive;
extern bool     gInterceptorBandSweepActive;
extern bool     gInterceptorHuntTickerActive;
extern uint32_t gInterceptorHuntTickerFreq;
extern int8_t   gInterceptorFlashSlot;
extern uint8_t  gInterceptorFlashCount;
extern int16_t  gInterceptorCheckingSlot;
extern int16_t  gInterceptorLastActiveSlot;

#define SWEEP_BAND_COUNT 9
#define SWEEP_MANUAL_BAND_INDEX 8

typedef struct {
    uint32_t StartFreq;
    uint32_t EndFreq;
    uint32_t StepSize;
    char     Name[12];
    bool     Enabled;
} SweepBand_t;

extern SweepBand_t gSweepBands[SWEEP_BAND_COUNT];
extern uint8_t gBandSelectHighlight;
extern bool    gBandSelectEnteringFreq;
extern uint8_t gBandSelectEnteringWhich;
extern uint8_t gBandSelectStepOptionIndex;

#define STEP_OPTION_COUNT 7
extern const uint32_t gStepOptions[STEP_OPTION_COUNT];

#define NOAA_EXCLUDE_START 16240000
#define NOAA_EXCLUDE_END   16255000
extern bool gExcludeNoaa;
extern bool gSweepNeedsReinit;

#define BAND_SELECT_TOTAL_ROWS (SWEEP_BAND_COUNT + 1)
#define BAND_SELECT_NOAA_ROW SWEEP_BAND_COUNT

#endif
