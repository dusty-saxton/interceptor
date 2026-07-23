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
extern uint8_t  gInterceptorMeterPercent;
extern bool     gInterceptorTxOverrideActive;
extern bool     gInterceptorBandSweepActive;

// gFrameBuffer is FRAME_LINES (7) pages tall, each page = 8 pixels, indexed
// directly (confirmed against the real UI_PrintString implementation in
// ui/helper.c - NOT a pixel Y coordinate). Layout for the 3x3 grid:
//   page 0        - status bar (small font, 1 page)
//   pages 1-2     - grid row 0 (big font, 2 pages)
//   pages 3-4     - grid row 1 (big font, 2 pages)
//   pages 5-6     - grid row 2 (big font, 2 pages)
// This uses all 7 pages exactly, with room for the big, bold font.
#define STATUS_LINE      0
#define GRID_FIRST_LINE  1
#define GRID_ROWS        3
#define GRID_COLS        3

// Inverts a proportional left-to-right slice of a 2-page-tall cell, based
// on gInterceptorMeterPercent (0-100) - a live signal-strength sweep rather
// than an instant full-cell flip.
static void UI_DrawMeterSweep(uint8_t page, uint8_t x1, uint8_t x2)
{
    uint8_t width    = (x2 >= x1) ? (x2 - x1 + 1) : 0;
    uint8_t fillCols = (uint8_t)(((uint16_t)width * gInterceptorMeterPercent) / 100);
    uint8_t xFill     = x1 + fillCols;
    if (xFill > x2) xFill = x2;

    for (uint8_t col = x1; col <= xFill && col < LCD_WIDTH; col++) {
        if (page < FRAME_LINES)     gFrameBuffer[page][col]     ^= 0xFF;
        if (page + 1 < FRAME_LINES) gFrameBuffer[page + 1][col] ^= 0xFF;
    }
}

// Thin border outline around a 2-page-tall cell - used for the navigation
// cursor, so it never collides with UI_InvertCellBlock (two XORs on the
// same pixels would cancel out and hide both states at once).
static void UI_DrawSelectionBox(uint8_t page, uint8_t x1, uint8_t x2)
{
    if (page < FRAME_LINES)
        for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
            gFrameBuffer[page][col] ^= 0x01; // top edge
    if (page + 1 < FRAME_LINES)
        for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
            gFrameBuffer[page + 1][col] ^= 0x80; // bottom edge
    if (x1 < LCD_WIDTH) {
        if (page < FRAME_LINES)     gFrameBuffer[page][x1]     ^= 0xFF;
        if (page + 1 < FRAME_LINES) gFrameBuffer[page + 1][x1] ^= 0xFF;
    }
    if (x2 < LCD_WIDTH) {
        if (page < FRAME_LINES)     gFrameBuffer[page][x2]     ^= 0xFF;
        if (page + 1 < FRAME_LINES) gFrameBuffer[page + 1][x2] ^= 0xFF;
    }
}

void UI_DisplayInterceptorGridPage(void)
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

    // --- Status bar: small font so the grid rows below have room to be big ---
    // NOTE: this line has ~125px of real usable width at 7px/char - max 17
    // characters is the hard safe limit (confirmed the hard way earlier: going
    // over this causes a position calculation to wrap/corrupt memory, not just
    // look wrong). The page indicator is deliberately just the current page
    // number (no "total pages", no slash) to leave room for the full mode word.
    char status_str[24];
    uint8_t totalPages = INTERCEPTOR_GetUsedPageCount();

    if (gInterceptorBandSweepActive) {
        sprintf(status_str, "SCAN ON P%u/%u", gCurrentGridPage + 1, totalPages);
    } else {
        sprintf(status_str, gSniffingEnabled ? "INT ON P%u/%u" : "INT OFF P%u/%u",
                gCurrentGridPage + 1, totalPages);
    }
    UI_PrintStringSmallNormal(status_str, 2, 127, STATUS_LINE);

    if (gSniffingEnabled || gInterceptorBandSweepActive) {
        for (uint8_t col = 0; col < LCD_WIDTH; col++)
            gFrameBuffer[STATUS_LINE][col] ^= 0xFF;
    }

    // --- 9-cell grid: 3 rows x 3 columns, 2 pages tall per row (big font) ---
    uint16_t offset = gCurrentGridPage * GRID_PAGE_SIZE;

    for (uint8_t i = 0; i < GRID_PAGE_SIZE; i++) {
        uint16_t idx = offset + i;
        if (idx >= GRID_TOTAL_SLOTS) break;

        uint8_t col  = i % GRID_COLS;
        uint8_t row  = i / GRID_COLS;
        uint8_t page = GRID_FIRST_LINE + (row * 2);
        uint8_t x    = (col == 0) ? 0 : ((col == 1) ? 43 : 86);
        uint8_t xEnd = x + 41;

        char box_out[16];

        if (idx == offset + gInterceptorHighlight && gInterceptorEnteringChannel) {
            const char *typed = INPUTBOX_GetAscii();
            char echo[4] = "___";
            for (uint8_t d = 0; d < gInputBoxIndex && d < 3; d++)
                echo[d] = typed[d];
            sprintf(box_out, "%s", echo);
            UI_PrintString(box_out, x, xEnd, page, 7);
            UI_DrawSelectionBox(page, x, xEnd);
            continue;
        }

        if (idx == offset + gInterceptorHighlight && gInterceptorNameEditIndex >= 0) {
            strncpy(box_out, gInterceptorNameBuf, 6);
            box_out[6] = '\0';
            UI_PrintString(box_out, x, xEnd, page, 7);
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

            UI_PrintString(box_out, x, xEnd, page, 7);

            if ((gInterceptorActiveFrequency != 0 && gScanList[idx].Frequency == gInterceptorActiveFrequency)
                || (gInterceptorTxOverrideActive && i == gInterceptorHighlight)) {
                UI_DrawMeterSweep(page, x, xEnd);
            }

            if (gScanList[idx].IsLocked) {
                // small permanent mark for manually-added/locked channels,
                // so they're visually distinct from auto-detected ones -
                // placed away from the border's exact edge pixels so it
                // doesn't get erased by the selection cursor's XOR
                uint8_t markCenter = x + (xEnd - x) / 2;
                if (page + 1 < FRAME_LINES) {
                    for (uint8_t col = markCenter - 1; col <= markCenter + 1 && col < LCD_WIDTH; col++)
                        gFrameBuffer[page + 1][col] ^= 0x20;
                }
            }
        }

        if (i == gInterceptorHighlight) {
            UI_DrawSelectionBox(page, x, xEnd);
        }
    }

    ST7565_BlitFullScreen();
}
