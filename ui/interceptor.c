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

// gFrameBuffer is FRAME_LINES (7) pages tall, each page = 8 pixels, indexed
// directly (NOT a pixel Y coordinate - confirmed against the real
// UI_PrintString implementation in ui/helper.c). The status bar uses the
// big font (2 pages tall: 0 and 1); the 5 grid rows use the small font
// (1 page each), occupying pages 2 through 6 - exactly filling the screen.
#define STATUS_LINE      0
#define GRID_FIRST_LINE  2

// Inverts one page's worth of pixels across a column range - used to show
// a cell currently receiving live audio. Operates on a single page only,
// since every grid cell here is exactly one page tall.
static void UI_InvertCellBlock(uint8_t page, uint8_t x1, uint8_t x2)
{
    if (page >= FRAME_LINES) return;
    for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
        gFrameBuffer[page][col] ^= 0xFF;
}

// Thin border outline (top/bottom rows + left/right columns of the cell)
// - used for the navigation cursor, so it never collides with
// UI_InvertCellBlock (two XORs on the same pixels would cancel out and
// hide both states at once).
static void UI_DrawSelectionBox(uint8_t page, uint8_t x1, uint8_t x2)
{
    if (page >= FRAME_LINES) return;
    for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
        gFrameBuffer[page][col] ^= 0x81; // top pixel + bottom pixel of this page's 8-pixel column
    if (x1 < LCD_WIDTH) gFrameBuffer[page][x1] ^= 0xFF;
    if (x2 < LCD_WIDTH) gFrameBuffer[page][x2] ^= 0xFF;
}

void UI_DisplayInterceptorGridPage(void)
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    // --- Status bar: sniffing state + page indicator ---
    char status_str[24];
    uint8_t total_pages = INTERCEPTOR_GetUsedPageCount();

    sprintf(status_str, gSniffingEnabled ? "INTERCEPT ON  P:%u/%u" : "INTERCEPT OFF P:%u/%u",
            gCurrentGridPage + 1, total_pages);
    UI_PrintString(status_str, 2, 127, STATUS_LINE, 4);

    // Invert both status bar pages when sniffing is active - a quick
    // at-a-glance highlighted banner.
    if (gSniffingEnabled) {
        for (uint8_t col = 0; col < LCD_WIDTH; col++) {
            gFrameBuffer[STATUS_LINE][col]     ^= 0xFF;
            gFrameBuffer[STATUS_LINE + 1][col] ^= 0xFF;
        }
    }

    // --- 15-cell grid: 5 rows x 3 columns, one page tall per row ---
    uint16_t offset = gCurrentGridPage * GRID_PAGE_SIZE;

    for (uint8_t i = 0; i < GRID_PAGE_SIZE; i++) {
        uint16_t idx = offset + i;
        if (idx >= GRID_TOTAL_SLOTS) break;

        uint8_t col  = i % 3;
        uint8_t row  = i / 3;
        uint8_t page = GRID_FIRST_LINE + row;
        uint8_t x    = (col == 0) ? 2 : ((col == 1) ? 45 : 88);
        uint8_t xEnd = x + 40;

        char box_out[16];

        if (idx == offset + gInterceptorHighlight && gInterceptorEnteringChannel) {
            const char *typed = INPUTBOX_GetAscii();
            char echo[4] = "___";
            for (uint8_t d = 0; d < gInputBoxIndex && d < 3; d++)
                echo[d] = typed[d];
            sprintf(box_out, "C%s", echo);
            UI_PrintStringSmallNormal(box_out, x, xEnd, page);
            UI_DrawSelectionBox(page, x, xEnd);
            continue;
        }

        if (idx == offset + gInterceptorHighlight && gInterceptorNameEditIndex >= 0) {
            strncpy(box_out, gInterceptorNameBuf, 6);
            box_out[6] = '\0';
            UI_PrintStringSmallNormal(box_out, x, xEnd, page);
            UI_DrawSelectionBox(page, x, xEnd);
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

            UI_PrintStringSmallNormal(box_out, x, xEnd, page);

            if (gInterceptorActiveFrequency != 0 && gScanList[idx].Frequency == gInterceptorActiveFrequency) {
                UI_InvertCellBlock(page, x, xEnd);
            }
        }

        if (i == gInterceptorHighlight) {
            UI_DrawSelectionBox(page, x, xEnd);
        }
    }

    ST7565_BlitFullScreen();
}
