#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/st7565.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "dcs.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

// current highlight/edit state, shared with app/interceptor.c's key handler
extern uint8_t gInterceptorHighlight;
extern int8_t  gInterceptorNameEditIndex; // -1 = not editing
extern char    gInterceptorNameBuf[7];
extern bool    gInterceptorEnteringChannel;
extern uint16_t gInterceptorPreviewChannel;
extern bool     gInterceptorScrollPreviewActive;
extern uint32_t gInterceptorActiveFrequency;
extern uint8_t  gInterceptorMeterPercent;
extern bool     gInterceptorTxOverrideActive;
extern bool     gInterceptorBandSweepActive;
extern bool     gInterceptorHuntTickerActive;
extern uint32_t gInterceptorHuntTickerFreq;
extern int8_t   gInterceptorFlashSlot;
extern uint8_t  gInterceptorFlashCount;
extern int16_t  gInterceptorCheckingSlot;

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
// Shifts already-rendered content up within its own page by right-shifting
// each column's byte value (bit0 = top pixel, bit7 = bottom pixel per this
// display's convention, confirmed via UI_DrawSelectionBox's own top/bottom
// edge bits) - used to lift cell text off the exact bottom edge by a
// couple pixels, since UI_PrintStringSmallBold has no sub-page positioning
// of its own.
// UI_PrintStringSmallBold centers text using unsigned arithmetic that
// UNDERFLOWS (confirmed in ui/helper.c) if the string is wider than the
// available (x, xEnd) space - it does not clamp or truncate on its own.
// Several of our own strings (CTCSS tone display especially, e.g.
// "254.1Hz" at 7 characters) can exceed a single grid cell's ~41px width
// at this font's 7px/char spacing (max ~5 safe characters) - this was a
// real, confirmed memory-corruption risk, not a theoretical one. Truncates
// defensively in place before calling through.
static void Safe_PrintStringSmallBold(char *str, uint8_t x, uint8_t xEnd, uint8_t page)
{
    uint8_t avail = (xEnd >= x) ? (xEnd - x) : 0;
    uint8_t maxChars = avail / 7; // 6px glyph + 1px spacing
    if (strlen(str) > maxChars)
        str[maxChars] = '\0';
    UI_PrintStringSmallBold(str, x, xEnd, page);
}

static void Shift_Text_Up(uint8_t page, uint8_t x1, uint8_t x2, uint8_t pixels)
{
    if (page >= FRAME_LINES) return;
    uint8_t mask = (uint8_t)((1u << pixels) - 1); // low `pixels` bits = top `pixels` rows of this page
    for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++) {
        uint8_t spillover = gFrameBuffer[page][col] & mask;
        gFrameBuffer[page][col] >>= pixels;
        // Without this, those spillover rows are just lost - which is
        // exactly why the text looked chopped off at the top before.
        // Writing them into the bottom `pixels` rows of the page above
        // preserves the full glyph instead of clipping it.
        if (page > 0)
            gFrameBuffer[page - 1][col] |= (uint8_t)(spillover << (8 - pixels));
    }
}

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
            gFrameBuffer[page][col] ^= 0x03; // top edge, 2px thick
    if (page + 1 < FRAME_LINES)
        for (uint8_t col = x1; col <= x2 && col < LCD_WIDTH; col++)
            gFrameBuffer[page + 1][col] ^= 0xC0; // bottom edge, 2px thick
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
    uint8_t totalPages = INTERCEPTOR_GetReachablePageCount();

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

    // Next empty slot overall (not just this page) - used to show the live
    // hunt ticker in whichever cell a new capture would actually land in.
    uint16_t nextEmptySlot = 0xFFFF;
    for (uint16_t i = 0; i < GRID_TOTAL_SLOTS; i++) {
        if (gScanList[i].Frequency == 0) { nextEmptySlot = i; break; }
    }

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
            if (gInterceptorScrollPreviewActive && gInputBoxIndex == 0) {
                // Scroll-preview method - show the real channel's saved
                // name (or frequency, if it has none) as a live preview.
                char previewName[7] = {0};
                SETTINGS_FetchChannelName(previewName, (int)gInterceptorPreviewChannel);
                if (previewName[0] != '\0') {
                    strncpy(box_out, previewName, 6);
                    box_out[6] = '\0';
                } else {
                    uint32_t raw_f = SETTINGS_FetchChannelFrequency((int)gInterceptorPreviewChannel);
                    sprintf(box_out, "%3u.%02u",
                            (unsigned int)(raw_f / 100000),
                            (unsigned int)((raw_f % 100000) / 1000));
                }
            } else {
                const char *typed = INPUTBOX_GetAscii();
                char echo[4] = "___";
                for (uint8_t d = 0; d < gInputBoxIndex && d < 3; d++)
                    echo[d] = typed[d];
                sprintf(box_out, "%s", echo);
            }
            Safe_PrintStringSmallBold(box_out, x, xEnd, page + 1);
            Shift_Text_Up(page + 1, x, xEnd, 2); // lift off the exact bottom edge
            UI_DrawSelectionBox(page, x, xEnd);
            continue;
        }

        if (idx == offset + gInterceptorHighlight && gInterceptorNameEditIndex >= 0) {
            strncpy(box_out, gInterceptorNameBuf, 6);
            box_out[6] = '\0';
            Safe_PrintStringSmallBold(box_out, x, xEnd, page + 1);
            Shift_Text_Up(page + 1, x, xEnd, 2); // lift off the exact bottom edge
            UI_DrawSelectionBox(page, x, xEnd);
            continue;
        }

        if (gScanList[idx].Frequency == 0 && idx == nextEmptySlot && gInterceptorHuntTickerActive) {
            // Live scanning ticker - shows the frequency hunt is currently
            // evaluating, right in the cell a new capture would land in.
            uint32_t raw_f = gInterceptorHuntTickerFreq;
            sprintf(box_out, "%3u.%02u",
                    (unsigned int)(raw_f / 100000),
                    (unsigned int)((raw_f % 100000) / 1000));
            Safe_PrintStringSmallBold(box_out, x, xEnd, page + 1);
            Shift_Text_Up(page + 1, x, xEnd, 2); // lift off the exact bottom edge
            if (i == gInterceptorHighlight) {
                UI_DrawSelectionBox(page, x, xEnd);
            }
            continue;
        }

        if (gScanList[idx].Frequency != 0) {
            if (idx == (uint16_t)gInterceptorFlashSlot && gInterceptorFlashCount > 3
                && gScanList[idx].CodeType != CODE_TYPE_OFF) {
                // Briefly show the discovered tone instead of the
                // frequency/name, for the first half of the flash sequence
                // only - settles to the normal display for the rest.
                if (gScanList[idx].CodeType == CODE_TYPE_CONTINUOUS_TONE) {
                    uint16_t tone = CTCSS_Options[gScanList[idx].Code];
                    sprintf(box_out, "%u.%uHz", tone / 10, tone % 10);
                } else {
                    // DCS - normal vs inverted isn't tracked separately by
                    // our capture, so this shows the bare code without a
                    // N/I suffix rather than risk showing the wrong one.
                    sprintf(box_out, "D%03o", DCS_Options[gScanList[idx].Code]);
                }
            } else if (gScanList[idx].Name[0] != '\0') {
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

            Safe_PrintStringSmallBold(box_out, x, xEnd, page + 1);
            Shift_Text_Up(page + 1, x, xEnd, 2); // lift off the exact bottom edge

            if ((gInterceptorActiveFrequency != 0 && gScanList[idx].Frequency == gInterceptorActiveFrequency)
                || (gInterceptorTxOverrideActive && i == gInterceptorHighlight)) {
                UI_DrawMeterSweep(page, x, xEnd);
            }

            if (gScanList[idx].IsLocked) {
                // shorter underline (middle portion, not edge-to-edge) for
                // manually-added/locked channels, so they're visually
                // distinct from auto-detected ones - uses bit 0x40 (not the
                // border's own 0x80 bottom-edge bit) so it doesn't collide
                // with the selection cursor's XOR
                if (page + 1 < FRAME_LINES) {
                    uint8_t width  = (xEnd >= x) ? (xEnd - x + 1) : 0;
                    uint8_t inset  = width / 5; // trim ~20% off each side
                    uint8_t startX = x + inset;
                    uint8_t endX   = xEnd - inset;
                    for (uint8_t col = startX; col <= endX && col < LCD_WIDTH; col++)
                        gFrameBuffer[page + 1][col] ^= 0x40;
                }
            }

            if (idx == (uint16_t)gInterceptorFlashSlot && gInterceptorFlashCount > 0
                && (gInterceptorFlashCount % 2) == 0) {
                // brief full-cell invert flash on a just-saved capture,
                // alternating on/off each redraw for a blinking effect
                for (uint8_t col = x; col <= xEnd && col < LCD_WIDTH; col++) {
                    if (page < FRAME_LINES)     gFrameBuffer[page][col]     ^= 0xFF;
                    if (page + 1 < FRAME_LINES) gFrameBuffer[page + 1][col] ^= 0xFF;
                }
            }

            if (gInterceptorCheckingSlot >= 0 && idx == (uint16_t)gInterceptorCheckingSlot) {
                // this saved cell is currently being tested by grid-check
                // or fast-scan - full invert for the brief settle period
                for (uint8_t col = x; col <= xEnd && col < LCD_WIDTH; col++) {
                    if (page < FRAME_LINES)     gFrameBuffer[page][col]     ^= 0xFF;
                    if (page + 1 < FRAME_LINES) gFrameBuffer[page + 1][col] ^= 0xFF;
                }
            }
        }

        if (i == gInterceptorHighlight) {
            UI_DrawSelectionBox(page, x, xEnd);
        }
    }

    ST7565_BlitFullScreen();
}
