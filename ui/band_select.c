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

void UI_DisplayBandSelect(void)
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    UI_PrintStringSmallNormal("Bands: MENU=on/off F5=Go", 0, 127, 0);

    // Keep the highlighted row always visible, scrolling the window as
    // needed rather than trying to show all rows at once.
    uint8_t firstVisible = 0;
    if (gBandSelectHighlight >= BAND_SELECT_VISIBLE_ROWS)
        firstVisible = gBandSelectHighlight - BAND_SELECT_VISIBLE_ROWS + 1;

    for (uint8_t row = 0; row < BAND_SELECT_VISIBLE_ROWS; row++) {
        uint8_t idx = firstVisible + row;
        if (idx >= BAND_SELECT_TOTAL_ROWS) break;

        uint8_t page = 1 + row;
        char line[26];

        if (idx == BAND_SELECT_NOAA_ROW) {
            sprintf(line, "%s Exclude NOAA (162.4-162.55)", gExcludeNoaa ? "[X]" : "[ ]");
        } else if (gBandSelectEnteringFreq && idx == SWEEP_MANUAL_BAND_INDEX) {
            // Live digit echo while typing - 6 digits total, first 3 are
            // whole MHz, last 3 are thousandths (e.g. "146720" = 146.720).
            const char *typed = INPUTBOX_GetAscii();
            char echo[7] = "______";
            for (uint8_t d = 0; d < gInputBoxIndex && d < 6; d++)
                echo[d] = typed[d];
            sprintf(line, "%s %.3s.%.3s MHz",
                    gBandSelectEnteringWhich == 0 ? "Start" : "End  ",
                    echo, echo + 3);
        } else {
            const char *box = gSweepBands[idx].Enabled ? "[X]" : "[ ]";
            if (idx == SWEEP_MANUAL_BAND_INDEX && gSweepBands[idx].EndFreq == 0) {
                sprintf(line, "%s %s (not set)", box, gSweepBands[idx].Name);
            } else {
                sprintf(line, "%s %s %u.%03u-%u.%03u", box, gSweepBands[idx].Name,
                        (unsigned int)(gSweepBands[idx].StartFreq / 100000),
                        (unsigned int)((gSweepBands[idx].StartFreq % 100000) / 100),
                        (unsigned int)(gSweepBands[idx].EndFreq / 100000),
                        (unsigned int)((gSweepBands[idx].EndFreq % 100000) / 100));
            }
        }

        UI_PrintStringSmallNormal(line, 0, 127, page);

        if (idx == gBandSelectHighlight) {
            for (uint8_t col = 0; col < LCD_WIDTH; col++)
                gFrameBuffer[page][col] ^= 0xFF;
        }
    }

    ST7565_BlitFullScreen();
}
