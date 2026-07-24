#ifndef INTERCEPTOR_H
#define INTERCEPTOR_H

#include "interceptor_grid.h"
#include "driver/keyboard.h"

// Call once per main loop iteration - runs whichever scan mode is active.
void INTERCEPTOR_Engine_Tick(void);

// Call once per real 10ms system tick (from APP_TimeSlice10ms) - times the
// carrier-pause reply-wait window accurately regardless of main loop speed.
void INTERCEPTOR_TimeSlice10ms(void);

// Adds a frequency (and its detected CTCSS/DCS tone, if any) to the grid.
// Skips duplicates. If the grid is completely full, evicts the
// lowest-HitCount unlocked slot. Does NOT mark the slot locked - callers
// that want a protected/manual entry should set IsLocked themselves after.
void INTERCEPTOR_LogNewCapture(uint32_t freq, uint8_t codeType, uint8_t code);

// Removes a slot from the grid and adds its frequency to the permanent
// blacklist, so future hunting will never re-save it (useful for noise/
// digital-junk signals that keep getting falsely detected as activity).
void INTERCEPTOR_DeleteAndBlacklist(uint16_t slotIndex);

// Just clears the slot, no blacklist entry - for a quick "get rid of this"
// versus "this is noise, never show it again".
void INTERCEPTOR_DeleteOnly(uint16_t slotIndex);

// Re-sorts the grid by HitCount, most-detected first. Locked slots never move.
void INTERCEPTOR_SortByPopularity(void);

// Returns how many pages currently have at least one populated slot
// (always at least 1).
uint8_t INTERCEPTOR_GetUsedPageCount(void);
uint8_t INTERCEPTOR_GetReachablePageCount(void);

// The grid screen's display function (ui/interceptor.c) and key handler
// (app/interceptor.c) - registered in ui/ui.c and app/app.c respectively.
void UI_DisplayInterceptorGridPage(void);
void INTERCEPTOR_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
void UI_DisplayBandSelect(void);
void BAND_SELECT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

#endif
