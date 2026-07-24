#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/keyboard.h"
#include "ui/inputbox.h"
#include "ui/ui.h"
#include "audio.h"
#include "misc.h"
#include "app/generic.h"
#include <string.h>
#include <stdlib.h>

static void Begin_Manual_Entry(void)
{
    gBandSelectEnteringFreq  = true;
    gBandSelectEnteringWhich = 0;
    gInputBoxIndex = 0;
    gUpdateDisplay = true;
}

static void Cancel_Manual_Entry(void)
{
    gBandSelectEnteringFreq = false;
    gInputBoxIndex = 0;
    gUpdateDisplay = true;
}

// Parses the 6 typed digits (3 whole MHz + 3 thousandths) into 10Hz units,
// the same convention used everywhere else in this feature.
static uint32_t Parse_Typed_Frequency(void)
{
    const char *typed = INPUTBOX_GetAscii();
    uint32_t whole = (uint32_t)((typed[0]-'0')*100 + (typed[1]-'0')*10 + (typed[2]-'0'));
    uint32_t thousandths = (uint32_t)((typed[3]-'0')*100 + (typed[4]-'0')*10 + (typed[5]-'0'));
    return whole * 100000 + thousandths * 100;
}

static void Confirm_Manual_Digit_Entry(void)
{
    if (gInputBoxIndex != 6) return; // need all 6 digits first

    if (gBandSelectEnteringWhich == 0) {
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].StartFreq = Parse_Typed_Frequency();
        gBandSelectEnteringWhich = 1;
        gInputBoxIndex = 0;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        gUpdateDisplay = true;
        return;
    }

    uint32_t endFreq = Parse_Typed_Frequency();
    if (endFreq <= gSweepBands[SWEEP_MANUAL_BAND_INDEX].StartFreq) {
        // invalid range - discard and go back to a clean not-set state
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].StartFreq = 0;
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].EndFreq    = 0;
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].Enabled    = false;
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    } else {
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].EndFreq = endFreq;
        gSweepBands[SWEEP_MANUAL_BAND_INDEX].Enabled = true; // auto-enable once configured
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    }
    gBandSelectEnteringFreq = false;
    gInputBoxIndex = 0;
    gUpdateDisplay = true;
}

void BAND_SELECT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    // F itself always needs explicit routing, same as the grid screen.
    if (Key == KEY_F) {
        GENERIC_Key_F(bKeyPressed, bKeyHeld);
        return;
    }

    // F+5: confirm current selection and actually start sweeping.
    if (gWasFKeyPressed && Key == KEY_5) {
        if (!bKeyPressed) return;
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        bool anyEnabled = false;
        for (uint8_t i = 0; i < SWEEP_BAND_COUNT; i++) {
            if (gSweepBands[i].Enabled && gSweepBands[i].EndFreq > gSweepBands[i].StartFreq) {
                anyEnabled = true;
                break;
            }
        }
        if (!anyEnabled) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL; // nothing selected - can't start
            return;
        }

        gSniffingEnabled            = false;
        gInterceptorBandSweepActive = true;
        gInterceptorViewActive      = true;
        gRequestDisplayScreen       = DISPLAY_INTERCEPTOR;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        return;
    }

    // F+7 still works from here too, same as it does everywhere else -
    // abandons band selection and starts sniffing directly.
    if (gWasFKeyPressed && Key == KEY_7) {
        if (!bKeyPressed) return;
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        gInterceptorBandSweepActive = false;
        gSniffingEnabled = !bKeyHeld;

        gInterceptorViewActive = true;
        gRequestDisplayScreen  = DISPLAY_INTERCEPTOR;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        return;
    }

    if (bKeyHeld) return; // this screen only reacts to clean presses
    if (!bKeyPressed) return;

    // --- Typing a manual frequency ---
    if (gBandSelectEnteringFreq) {
        if (Key <= KEY_9) {
            if (gInputBoxIndex < 6) {
                INPUTBOX_Append(Key);
                gUpdateDisplay = true;
                if (gInputBoxIndex == 6)
                    Confirm_Manual_Digit_Entry();
            }
            return;
        }
        if (Key == KEY_EXIT) {
            Cancel_Manual_Entry();
            return;
        }
        return; // ignore anything else while typing
    }

    switch (Key) {
        case KEY_UP:
            gBandSelectHighlight = (gBandSelectHighlight == 0) ? (BAND_SELECT_TOTAL_ROWS - 1) : (gBandSelectHighlight - 1);
            gUpdateDisplay = true;
            break;

        case KEY_DOWN:
            gBandSelectHighlight = (gBandSelectHighlight + 1) % BAND_SELECT_TOTAL_ROWS;
            gUpdateDisplay = true;
            break;

        case KEY_MENU:
            if (gBandSelectHighlight == BAND_SELECT_NOAA_ROW) {
                gExcludeNoaa = !gExcludeNoaa;
                gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
                gUpdateDisplay = true;
            } else if (gBandSelectHighlight == SWEEP_MANUAL_BAND_INDEX
                && gSweepBands[SWEEP_MANUAL_BAND_INDEX].EndFreq == 0) {
                Begin_Manual_Entry(); // never configured yet - set it up first
            } else {
                gSweepBands[gBandSelectHighlight].Enabled = !gSweepBands[gBandSelectHighlight].Enabled;
                gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
                gUpdateDisplay = true;
            }
            break;

        case KEY_STAR:
            // Only meaningful on the Manual row - lets you change a
            // range you've already set, not just toggle it.
            if (gBandSelectHighlight == SWEEP_MANUAL_BAND_INDEX)
                Begin_Manual_Entry();
            break;

        case KEY_EXIT:
            gRequestDisplayScreen = DISPLAY_MAIN;
            break;

        default:
            break;
    }
}
