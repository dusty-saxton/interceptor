#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/st7565.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include <stdio.h>
#include <string.h>

// current highlight/edit state, shared with app/interceptor.c's key handler
extern uint8_t gInterceptorHighlight;
extern int8_t  gInterceptorNameEditIndex; // -1 = not editing
extern char    gInterceptorNameBuf[7];
extern bool    gInterceptorEnteringChannel;
extern uint32_t gInterceptorActiveFrequency;

// Full block invert - used to show a cell currently receiving live audio.
// Unmistakable at a glance, matching how this codebase already indicates
// the selected menu item elsewhere (see the menu-cursor invert in menu.c).
static void UI_InvertCellBlock(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
    uint8_t page1 = y1 / 8;
    uint8_t page2 = y2 / 8;
    if (page2 >= 8) page2 = 7;

    for (uint8_t page = page1; page <= page2; page++)
        for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
            gFrameBuffer[page][col] ^= 0xFF;
}

// Thin border outline only - used for the navigation cursor, so it never
// collides with UI_InvertCellBlock (two XORs on the same pixels would
// cancel out and hide both states at once).
static void UI_DrawSelectionBox(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
    uint8_t page1 = y1 / 8;
    uint8_t page2 = y2 / 8;
    if (page2 >= 8) page2 = 7;

    // top and bottom rows of the box
    for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++) {
        gFrameBuffer[page1][col] ^= 0x01;
        gFrameBuffer[page2][col] ^= 0x80;
    }
    // left and right columns of the box
    if (x1 < LCD_WIDTH)
        for (uint8_t page = page1; page <= page2; page++)
            gFrameBuffer[page][x1] ^= 0xFF;
    if (x2 < LCD_WIDTH)
        for (uint8_t page = page1; page <= page2; page++)
            gFrameBuffer[page][x2] ^= 0xFF;
}

void UI_DisplayInterceptorGridPage(void)
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    // --- Status bar: sniffing state + page indicator ---
    char status_str[24];
    uint8_t total_pages = INTERCEPTOR_GetUsedPageCount();

    sprintf(status_str, gSniffingEnabled ? "INTERCEPT ON  P:%u/%u" : "INTERCEPT OFF P:%u/%u",
            gCurrentGridPage + 1, total_pages);
    UI_PrintString(status_str, 2, 127, 0, 4);

    // Invert the status bar's pixel row when sniffing is active - a quick
    // at-a-glance highlighted banner.
    if (gSniffingEnabled) {
        for (uint8_t col = 0; col < LCD_WIDTH; col++)
            gFrameBuffer[0][col] ^= 0xFF;
    }

    // --- 15-cell grid: 5 rows x 3 columns ---
    uint16_t offset = gCurrentGridPage * GRID_PAGE_SIZE;

    for (uint8_t i = 0; i < GRID_PAGE_SIZE; i++) {
        uint16_t idx = offset + i;
        if (idx >= GRID_TOTAL_SLOTS) break;

        uint8_t col = i % 3;
        uint8_t row = i / 3;
        uint8_t x = (col == 0) ? 4 : ((col == 1) ? 48 : 92);
        uint8_t y = 10 + (row * 11);

        char box_out[16];

        if (idx == offset + gInterceptorHighlight && gInterceptorEnteringChannel) {
            // live digit echo while typing a channel number - pads with
            // underscores so the cursor position is visible
            const char *typed = INPUTBOX_GetAscii();
            char echo[4] = "___";
            for (uint8_t d = 0; d < gInputBoxIndex && d < 3; d++)
                echo[d] = typed[d];
            sprintf(box_out, "Ch%s", echo);
            UI_PrintString(box_out, x, x + 36, y, 6);
            UI_DrawSelectionBox(x - 2, y - 1, x + 38, y + 8);
            continue;
        }

        if (idx == offset + gInterceptorHighlight && gInterceptorNameEditIndex >= 0) {
            // currently being renamed - show the in-progress buffer with a
            // cursor marker under the active character
            strncpy(box_out, gInterceptorNameBuf, 6);
            box_out[6] = '\0';
            UI_PrintString(box_out, x, x + 36, y, 6);
            UI_DrawSelectionBox(x - 2, y - 1, x + 38, y + 8);
            continue;
        }

        if (gScanList[idx].Frequency != 0) {
            if (gScanList[idx].Name[0] != '\0') {
                strncpy(box_out, gScanList[idx].Name, 6);
                box_out[6] = '\0';
            } else {
                uint32_t raw_f = gScanList[idx].Frequency;
                // Display-only truncation to 2 decimal places (whole
                // stored Frequency is untouched and used for actual tuning).
                sprintf(box_out, "%3u.%02u",
                        (unsigned int)(raw_f / 100000),
                        (unsigned int)((raw_f % 100000) / 1000));
            }

            UI_PrintString(box_out, x, x + 36, y, 6);

            if (gInterceptorActiveFrequency != 0 && gScanList[idx].Frequency == gInterceptorActiveFrequency) {
                // this slot is currently receiving live audio
                UI_InvertCellBlock(x - 2, y - 1, x + 38, y + 8);
            }
        }

        if (i == gInterceptorHighlight) {
            UI_DrawSelectionBox(x - 2, y - 1, x + 38, y + 8);
        }
    }

    ST7565_BlitFullScreen();
}
