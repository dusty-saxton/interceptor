#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/st7565.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include <stdio.h>
#include <string.h>

// How many rows fit on screen at once - the header takes the first page,
// leaving the rest for a scrolling window of band rows.
#define BAND_SELECT_VISIBLE_ROWS (FRAME_LINES - 1)

// This font is 6px wide + 1px spacing = 7px/char. UI_PrintStringSmallNormal
// centers text using unsigned arithmetic that UNDERFLOWS if the string is
// wider than the available space (confirmed in ui/helper.c) - it does not
// clamp or truncate on its own. 128px / 7px = ~18.3, so every string handed
// to it here MUST stay at or under 18 characters, confirmed with the exact
// math, not just "should be fine." This was a real, serious bug before -
// rows previously reached 31 characters here.
#define BAND_SELECT_MAX_LINE_LEN 18

void UI_DisplayBandSelect(void)
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    // Header shows the highlighted row's frequency range instead of static
    // instructions - keeps every individual row short enough to be safe at
    // this font size, while still surfacing the range somewhere.
    char header[BAND_SELECT_MAX_LINE_LEN + 1];
    if (gBandSelectEnteringFreq) {
        const char *typed = INPUTBOX_GetAscii();
        char echo[7] = "______";
        for (uint8_t d = 0; d < gInputBoxIndex && d < 6; d++)
            echo[d] = typed[d];
        snprintf(header, sizeof(header), "%s %.3s.%.3s",
                 gBandSelectEnteringWhich == 0 ? "Start" : "End  ", echo, echo + 3);
    } else if (gBandSelectHighlight == BAND_SELECT_NOAA_ROW) {
        snprintf(header, sizeof(header), "162.400-162.550");
    } else if (gBandSelectHighlight == SWEEP_MANUAL_BAND_INDEX
               && gSweepBands[SWEEP_MANUAL_BAND_INDEX].EndFreq == 0) {
        snprintf(header, sizeof(header), "Not configured");
    } else {
        snprintf(header, sizeof(header), "%u.%03u-%u.%03u",
                 (unsigned int)(gSweepBands[gBandSelectHighlight].StartFreq / 100000),
                 (unsigned int)((gSweepBands[gBandSelectHighlight].StartFreq % 100000) / 100),
                 (unsigned int)(gSweepBands[gBandSelectHighlight].EndFreq / 100000),
                 (unsigned int)((gSweepBands[gBandSelectHighlight].EndFreq % 100000) / 100));
    }
    UI_PrintStringSmallNormal(header, 0, 0, 0); // End==Start -> left-aligned, no centering

    // Keep the highlighted row always visible, scrolling the window as
    // needed rather than trying to show all rows at once.
    uint8_t firstVisible = 0;
    if (gBandSelectHighlight >= BAND_SELECT_VISIBLE_ROWS)
        firstVisible = gBandSelectHighlight - BAND_SELECT_VISIBLE_ROWS + 1;

    for (uint8_t row = 0; row < BAND_SELECT_VISIBLE_ROWS; row++) {
        uint8_t idx = firstVisible + row;
        if (idx >= BAND_SELECT_TOTAL_ROWS) break;

        uint8_t page = 1 + row;
        char line[BAND_SELECT_MAX_LINE_LEN + 1];

        // Just checkbox + name on every row now - the frequency range
        // lives in the header instead, keeping every row short and safe.
        if (idx == BAND_SELECT_NOAA_ROW) {
            snprintf(line, sizeof(line), "%s Exclude NOAA", gExcludeNoaa ? "[X]" : "[ ]");
        } else {
            snprintf(line, sizeof(line), "%s %s",
                     gSweepBands[idx].Enabled ? "[X]" : "[ ]", gSweepBands[idx].Name);
        }

        UI_PrintStringSmallNormal(line, 0, 0, page); // End==Start -> left-aligned, no centering

        if (idx == gBandSelectHighlight) {
            for (uint8_t col = 0; col < LCD_WIDTH; col++)
                gFrameBuffer[page][col] ^= 0xFF;
        }
    }

    ST7565_BlitFullScreen();
}
