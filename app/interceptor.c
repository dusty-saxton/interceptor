#include "interceptor_grid.h"
#include "interceptor.h"
#include "driver/keyboard.h"
#include "ui/inputbox.h"
#include "ui/ui.h"
#include "audio.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "frequencies.h"
#include "app/generic.h"
#include <string.h>
#include <stdlib.h>

// Shared with ui/interceptor.c for drawing the current highlight/edit state
uint8_t gInterceptorHighlight = 0;      // index within the current page (0..14)
int8_t  gInterceptorNameEditIndex = -1; // -1 = not editing a name
char    gInterceptorNameBuf[7] = {0};

// True while typing a 3-digit memory channel number to add to the grid.
// Shared (not static) so ui/interceptor.c can show live digit echo.
bool gInterceptorEnteringChannel = false;

static uint16_t CurrentSlotIndex(void) {
    return (gCurrentGridPage * GRID_PAGE_SIZE) + gInterceptorHighlight;
}

// A dedicated VFO_Info_t used only while transmitting on a grid channel.
// Never touches the real, persistent gEeprom.VfoInfo[] array - gTxVfo is
// only temporarily repointed at this during the PTT press, then handed
// back to the real VFO afterward via the same gFlagReconfigureVfos
// mechanism this firmware already uses to restore VFOs elsewhere.
static VFO_Info_t sInterceptorTxVfo;
static VFO_Info_t *sSavedTxVfo = NULL;

static void Begin_Interceptor_PTT(void)
{
    uint16_t idx = CurrentSlotIndex();
    if (gScanList[idx].Frequency == 0) return; // nothing to transmit on

    gInterceptorTxOverrideActive = true;

    // transmitting takes priority over background intercepting - stops it
    // outright rather than just pausing for the TX moment, since resuming
    // is a deliberate action (F+7) rather than automatic
    gSniffingEnabled = false;

    // start from a copy of the real current VFO so squelch/bandwidth/
    // scramble/compander/etc settings are all sane defaults
    sInterceptorTxVfo = *gTxVfo;
    sInterceptorTxVfo.pRX = &sInterceptorTxVfo.freq_config_RX;
    sInterceptorTxVfo.pTX = &sInterceptorTxVfo.freq_config_TX;

    sInterceptorTxVfo.freq_config_TX.Frequency = gScanList[idx].Frequency;
    sInterceptorTxVfo.freq_config_TX.CodeType  = gScanList[idx].CodeType;
    sInterceptorTxVfo.freq_config_TX.Code      = gScanList[idx].Code;

    // grid channels are direct/simplex - no repeater offset
    sInterceptorTxVfo.TX_OFFSET_FREQUENCY           = 0;
    sInterceptorTxVfo.TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;

    // re-derive the band for this specific frequency, then let the real,
    // verified firmware function compute the correct power calibration for
    // that band - never left stale from whatever the previous VFO had
    sInterceptorTxVfo.Band         = FREQUENCY_GetBand(gScanList[idx].Frequency);
    sInterceptorTxVfo.OUTPUT_POWER = OUTPUT_POWER_LOW; // always Low, by design

    RADIO_ConfigureSquelchAndOutputPower(&sInterceptorTxVfo);

    sSavedTxVfo = gTxVfo;
    gTxVfo = &sInterceptorTxVfo;

    GENERIC_Key_PTT(true); // reuse the real, existing PTT-press/TX-safety path
}

static void End_Interceptor_PTT(void)
{
    gInterceptorTxOverrideActive = false;
    GENERIC_Key_PTT(false); // reuse the real, existing PTT-release/end-of-TX path

    if (sSavedTxVfo != NULL) {
        // Don't just restore the saved pointer directly - force the same
        // full reconfigure this firmware already uses elsewhere (see
        // gFlagReconfigureVfos in app/app.c), so gTxVfo/gRxVfo and all
        // hardware registers get properly re-derived from the real,
        // persistent VFO settings rather than trusted to still be correct.
        gFlagReconfigureVfos = true;
        sSavedTxVfo = NULL;
    }
}

// Moves the highlight to the next/previous slot, skipping empty ones,
// bounded so it can never spin forever on an empty page.
// Moves the highlight to the next/previous slot on the current page,
// wrapping around. Deliberately does NOT skip empty slots - you need to
// be able to navigate onto an empty one to add a channel there.
static void Move_Highlight(int8_t direction)
{
    int8_t next = (int8_t)gInterceptorHighlight + direction;
    if (next < 0) next = GRID_PAGE_SIZE - 1;
    if (next >= GRID_PAGE_SIZE) next = 0;
    gInterceptorHighlight = (uint8_t)next;
}

static void Begin_Name_Edit(void)
{
    uint16_t idx = CurrentSlotIndex();
    memset(gInterceptorNameBuf, 0, sizeof(gInterceptorNameBuf));
    if (gScanList[idx].Name[0] != '\0')
        strncpy(gInterceptorNameBuf, gScanList[idx].Name, 6);
    else
        memset(gInterceptorNameBuf, '_', 6); // placeholder, same convention as channel naming

    gInterceptorNameEditIndex = 0;
    gUpdateDisplay = true;
}

static void Confirm_Name_Edit(void)
{
    uint16_t idx = CurrentSlotIndex();
    for (int8_t i = 5; i >= 0; i--) {
        if (gInterceptorNameBuf[i] == '_')
            gInterceptorNameBuf[i] = '\0';
        else
            break;
    }
    memset(gScanList[idx].Name, 0, sizeof(gScanList[idx].Name));
    strncpy(gScanList[idx].Name, gInterceptorNameBuf, 6);

    gInterceptorNameEditIndex = -1;
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gUpdateDisplay = true;
}

// Cycles the character at the current edit cursor, same convention (and
// skip-list) as this firmware's existing channel-name editor.
static void Cycle_Name_Character(int8_t direction)
{
    const char unwanted[] = "$%&!\"':;?^`|{}";
    char c = gInterceptorNameBuf[gInterceptorNameEditIndex] + direction;
    unsigned int i = 0;
    while (i < sizeof(unwanted) - 1 && c >= 32 && c <= 126) {
        if (c == unwanted[i++]) {
            c += direction;
            i = 0;
        }
    }
    gInterceptorNameBuf[gInterceptorNameEditIndex] = (c < 32) ? 126 : (c > 126) ? 32 : c;
    gUpdateDisplay = true;
}

static void Begin_Channel_Entry(void)
{
    gInterceptorEnteringChannel = true;
    gInputBoxIndex = 0;
    gUpdateDisplay = true;
}

// Reads the 3 typed digits as a 1-based channel number (001-200, matching
// how this firmware's own MR channel entry works), validates it, and if
// valid, pulls that channel's frequency and saved name straight from
// memory into the current grid slot.
static void Confirm_Channel_Entry(void)
{
    gInterceptorEnteringChannel = false;

    if (gInputBoxIndex != 3) {
        gInputBoxIndex = 0;
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        gUpdateDisplay = true;
        return; // need exactly 3 digits
    }

    const char *typed = INPUTBOX_GetAscii();
    uint16_t channel = (uint16_t)((typed[0] - '0') * 100 + (typed[1] - '0') * 10 + (typed[2] - '0')) - 1;
    gInputBoxIndex = 0;

    if (!RADIO_CheckValidChannel(channel, false, 0)) {
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        gUpdateDisplay = true;
        return; // not a channel that's actually in use
    }

    uint16_t idx = CurrentSlotIndex();
    memset(&gScanList[idx], 0, sizeof(InterceptorChannel_t));
    gScanList[idx].Frequency = SETTINGS_FetchChannelFrequency((int)channel);
    SETTINGS_FetchChannelName(gScanList[idx].Name, (int)channel);
    gScanList[idx].IsLocked = true; // manually added: protected from auto-eviction/sort

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gUpdateDisplay = true;
}

void INTERCEPTOR_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    // The F key itself has to be explicitly routed to GENERIC_Key_F, same
    // as every other screen does (app/main.c, app/menu.c, app/fm.c all do
    // this individually - nothing calls it automatically). Without this,
    // gWasFKeyPressed never gets armed while viewing this screen, so F+7
    // would never do anything here at all.
    if (Key == KEY_F) {
        GENERIC_Key_F(bKeyPressed, bKeyHeld);
        return;
    }

    // F+7 toggles sniffing, same as it does from the main screen. Needed
    // here too since this screen has its own key handler entirely separate
    // from app/main.c's - pressing F+7 while already viewing the grid would
    // otherwise never reach that logic at all.
    if (gWasFKeyPressed && Key == KEY_7) {
        if (!bKeyPressed) return; // act on press, not release
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        gInterceptorBandSweepActive = false; // mutually exclusive with band sweep

        if (!bKeyHeld) {
            gSniffingEnabled = !gSniffingEnabled;
        } else {
            gSniffingEnabled = false;
        }
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        gUpdateDisplay = true;
        return;
    }

    // F+5 toggles the wide VHF/UHF band sweep, same "handle it on our own
    // screen too" reasoning as F+7 above.
    if (gWasFKeyPressed && Key == KEY_5) {
        if (!bKeyPressed) return; // act on press, not release
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        gSniffingEnabled = false; // mutually exclusive with regular sniffing
        gInterceptorBandSweepActive = !gInterceptorBandSweepActive;

        gInterceptorViewActive = true;
        gRequestDisplayScreen  = DISPLAY_INTERCEPTOR;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        gUpdateDisplay = true;
        return;
    }

    // STAR now requires a genuine long-press-then-release to delete a slot
    // (and blacklist it, if it wasn't a manually-added one) - a single tap
    // used to delete instantly with no confirmation, which was too easy to
    // trigger by accident while just navigating.
    if (Key == KEY_STAR) {
        if (!bKeyPressed && bKeyHeld) {
            uint16_t idx = CurrentSlotIndex();
            INTERCEPTOR_DeleteAndBlacklist(idx);
            gUpdateDisplay = true;
        }
        return;
    }

    // EXIT: short press leaves the grid screen as normal; a long press
    // instead clears the entire blacklist (an escape hatch, since there was
    // previously no way to ever see or undo what's been blacklisted). Only
    // applies during normal navigation - naming and channel-entry mode
    // still handle their own EXIT below, unaffected by this.
    if (Key == KEY_EXIT && !gInterceptorEnteringChannel && gInterceptorNameEditIndex < 0) {
        if (!bKeyPressed) {
            if (bKeyHeld) {
                gLockoutCount = 0;
                memset(gLockoutList, 0, sizeof(gLockoutList));
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                gUpdateDisplay = true;
            } else {
                gInterceptorViewActive = false;
                gRequestDisplayScreen  = DISPLAY_MAIN;
            }
        }
        return;
    }

    // PTT needs both press (start TX) and release (end TX) events, unlike
    // every other key on this screen - handle it before the "press only"
    // guard below swallows the release.
    if (Key == KEY_PTT) {
        if (bKeyPressed) {
            if (!bKeyHeld) Begin_Interceptor_PTT();
        } else {
            End_Interceptor_PTT();
        }
        return;
    }

    if (bKeyHeld) return; // this screen only reacts to clean presses
    if (!bKeyPressed) return; // act on press, not release, for this screen

    // --- Typing a channel number for a manual add ---
    if (gInterceptorEnteringChannel) {
        if (Key <= KEY_9) {
            if (gInputBoxIndex < 3) {
                INPUTBOX_Append(Key);
                gUpdateDisplay = true;
                if (gInputBoxIndex == 3)
                    Confirm_Channel_Entry(); // auto-confirm once 3 digits are in
            }
            return;
        }
        if (Key == KEY_MENU) {
            Confirm_Channel_Entry();
            return;
        }
        if (Key == KEY_EXIT) {
            gInterceptorEnteringChannel = false;
            gInputBoxIndex = 0;
            gUpdateDisplay = true;
            return;
        }
        return; // ignore anything else while typing
    }

    // --- Naming a slot ---
    if (gInterceptorNameEditIndex >= 0) {
        switch (Key) {
            case KEY_UP:
                Cycle_Name_Character(1);
                return;
            case KEY_DOWN:
                Cycle_Name_Character(-1);
                return;
            case KEY_MENU:
                if (gInterceptorNameEditIndex < 5) {
                    gInterceptorNameEditIndex++;
                    gUpdateDisplay = true;
                } else {
                    Confirm_Name_Edit();
                }
                return;
            case KEY_EXIT:
                Confirm_Name_Edit();
                return;
            default:
                return;
        }
    }

    // --- Normal grid navigation ---
    switch (Key) {
        case KEY_UP:
            Move_Highlight(-1);
            gUpdateDisplay = true;
            break;

        case KEY_DOWN:
            Move_Highlight(1);
            gUpdateDisplay = true;
            break;

        case KEY_MENU: {
            uint16_t idx = CurrentSlotIndex();
            if (gScanList[idx].Frequency == 0)
                Begin_Channel_Entry();
            else
                Begin_Name_Edit();
            break;
        }

        default:
            break;
    }
}
