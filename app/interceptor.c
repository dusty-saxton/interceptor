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
#include "driver/bk4819.h"
#include <string.h>
#include <stdlib.h>

// Shared with ui/interceptor.c for drawing the current highlight/edit state
uint8_t gInterceptorHighlight = 0;      // index within the current page (0..14)
int8_t  gInterceptorNameEditIndex = -1; // -1 = not editing a name
char    gInterceptorNameBuf[7] = {0};

// True while typing a 3-digit memory channel number to add to the grid.
// Shared (not static) so ui/interceptor.c can show live digit echo.
bool gInterceptorEnteringChannel = false;
uint16_t gInterceptorPreviewChannel = 0; // scroll-preview channel while adding via UP/DOWN instead of typing
bool     gInterceptorScrollPreviewActive = false; // true only once UP/DOWN has actually been pressed - default view is blank, ready to type

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

    // Force narrowband (12.5kHz) - confirmed via FCC.gov: since Jan 1 2013
    // this is a real legal requirement for Part 90 licensees in these exact
    // bands (150-174 MHz, 421-470 MHz), not just a preference. Wideband
    // deviation on a narrowband channel is out of compliance and would
    // cause audible over-deviation/splatter into adjacent channels.
    sInterceptorTxVfo.CHANNEL_BANDWIDTH = BK4819_FILTER_BW_NARROW;

    // re-derive the band for this specific frequency, then let the real,
    // verified firmware function compute the correct power calibration for
    // that band - never left stale from whatever the previous VFO had
    sInterceptorTxVfo.Band         = FREQUENCY_GetBand(gScanList[idx].Frequency);
    sInterceptorTxVfo.OUTPUT_POWER = OUTPUT_POWER_HIGH; // changed from Low - Low was too weak

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

// Shared by long-press UP/DOWN and the side buttons.
static void Change_Grid_Page(int8_t direction)
{
    uint8_t totalPages = INTERCEPTOR_GetReachablePageCount();
    if (totalPages <= 1) return;

    int8_t next = (int8_t)gCurrentGridPage + direction;
    if (next < 0) next = totalPages - 1;
    if (next >= totalPages) next = 0;
    gCurrentGridPage = (uint8_t)next;
    gInterceptorHighlight = 0;
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gUpdateDisplay = true;
}

// Moves the highlight to the next/previous slot. Rolls across page
// boundaries into the next/previous reachable page (rather than just
// wrapping within the same page), so a simple UP/DOWN gives one continuous
// scroll across all your saved channels and the next open page - no
// separate long-press gesture needed to actually get there.
static void Move_Highlight(int8_t direction)
{
    int8_t next = (int8_t)gInterceptorHighlight + direction;

    if (next >= GRID_PAGE_SIZE) {
        uint8_t totalPages = INTERCEPTOR_GetReachablePageCount();
        if (gCurrentGridPage + 1 < totalPages) {
            gCurrentGridPage++;
            next = 0;
        } else {
            next = 0; // no further page - wrap within this one
        }
    } else if (next < 0) {
        if (gCurrentGridPage > 0) {
            gCurrentGridPage--;
            next = GRID_PAGE_SIZE - 1;
        } else {
            next = GRID_PAGE_SIZE - 1; // already on page 1 - wrap within this one
        }
    }

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

// Finds the next valid channel in the given direction, wrapping around
// 0-199. Bounded so it can't spin forever if somehow nothing is valid -
// in that case just returns the starting value unchanged.
static uint16_t Find_Next_Valid_Channel(uint16_t start, int8_t direction)
{
    uint16_t idx = start;
    for (uint16_t tries = 0; tries < 200; tries++) {
        int16_t next = (int16_t)idx + direction;
        if (next < 0) next = 199;
        if (next >= 200) next = 0;
        idx = (uint16_t)next;
        if (RADIO_CheckValidChannel(idx, false, 0)) return idx;
    }
    return start;
}

static void Begin_Channel_Entry(void)
{
    gInterceptorEnteringChannel = true;
    gInputBoxIndex = 0;
    gInterceptorScrollPreviewActive = false; // default view is blank, ready to type immediately
    // Pre-compute the first valid channel so it's ready the instant UP/DOWN
    // is actually pressed, without needing to search at that moment.
    gInterceptorPreviewChannel = Find_Next_Valid_Channel(199, 1);
    gUpdateDisplay = true;
}

// Reads the 3 typed digits as a 1-based channel number (001-200, matching
// how this firmware's own MR channel entry works), validates it, and if
// valid, pulls that channel's frequency and saved name straight from
// memory into the current grid slot. If no digits were typed at all,
// uses whichever channel was scrolled to with UP/DOWN instead.
static void Confirm_Channel_Entry(void)
{
    gInterceptorEnteringChannel = false;
    uint16_t channel;

    if (gInterceptorScrollPreviewActive && gInputBoxIndex == 0) {
        // Scroll-preview method - already validated by construction, but
        // double-check anyway before trusting it.
        channel = gInterceptorPreviewChannel;
        if (!RADIO_CheckValidChannel(channel, false, 0)) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            gUpdateDisplay = true;
            return;
        }
    } else if (gInputBoxIndex == 3) {
        const char *typed = INPUTBOX_GetAscii();
        channel = (uint16_t)((typed[0] - '0') * 100 + (typed[1] - '0') * 10 + (typed[2] - '0')) - 1;
        gInputBoxIndex = 0;

        if (!RADIO_CheckValidChannel(channel, false, 0)) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            gUpdateDisplay = true;
            return; // not a channel that's actually in use
        }
    } else {
        gInputBoxIndex = 0;
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        gUpdateDisplay = true;
        return; // partial digits typed, neither complete nor empty
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

        // Opens the band-selection screen to reconfigure, instead of
        // directly toggling - F+5 again from there confirms and starts.
        gRequestDisplayScreen = DISPLAY_BAND_SELECT;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        gUpdateDisplay = true;
        return;
    }

    // Long-press UP/DOWN changes pages. This was previously completely
    // missing - short-press UP/DOWN was already used for moving the cursor
    // within a page, but nothing at all navigated between pages, so any
    // slot past the first 9 was genuinely unreachable.
    if ((Key == KEY_UP || Key == KEY_DOWN) && bKeyHeld && gInterceptorNameEditIndex < 0) {
        if (!bKeyPressed) {
            Change_Grid_Page(Key == KEY_UP ? -1 : 1);
        }
        return;
    }

    // STAR: short press deletes the selected slot with no blacklist entry
    // at all ("get rid of this"); long press deletes AND blacklists it
    // ("this is noise, never show it again"). No confirmation on the
    // short-press delete - a stray tap on STAR while navigating will
    // delete instantly, by deliberate choice.
    if (Key == KEY_STAR) {
        if (!bKeyPressed) {
            uint16_t idx = CurrentSlotIndex();
            if (bKeyHeld) {
                INTERCEPTOR_DeleteAndBlacklist(idx);
            } else {
                INTERCEPTOR_DeleteOnly(idx);
            }
            gUpdateDisplay = true;
        }
        return;
    }

    // EXIT: short press leaves the grid screen; long press clears the
    // entire blacklist (an escape hatch, since there's otherwise no way to
    // see or undo what's been blacklisted). Naming and channel-entry mode
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

    // Naming a slot: UP/DOWN cycles letters, with auto-repeat while held -
    // matches this firmware's own real character-cycling convention in
    // app/menu.c, which responds to bKeyPressed regardless of held state.
    // Positioned before the "clean press only" guard below, which
    // previously blocked this entirely, requiring one click per letter
    // with no way to hold-and-scroll.
    if (gInterceptorNameEditIndex >= 0 && (Key == KEY_UP || Key == KEY_DOWN) && bKeyPressed) {
        Cycle_Name_Character(Key == KEY_UP ? 1 : -1);
        return;
    }

    if (bKeyHeld) return; // this screen only reacts to clean presses
    if (!bKeyPressed) return; // act on press, not release, for this screen

    // --- Typing a channel number for a manual add ---
    if (gInterceptorEnteringChannel) {
        if (Key == KEY_UP || Key == KEY_DOWN) {
            // Scroll-preview method - abandons any partially-typed digits,
            // since the two input methods don't mix.
            gInputBoxIndex = 0;
            gInterceptorScrollPreviewActive = true;
            gInterceptorPreviewChannel = Find_Next_Valid_Channel(gInterceptorPreviewChannel, Key == KEY_UP ? -1 : 1);
            gUpdateDisplay = true;
            return;
        }
        if (Key <= KEY_9) {
            gInterceptorScrollPreviewActive = false; // typing a digit reverts back to digit-entry view
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
