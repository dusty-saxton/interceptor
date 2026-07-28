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
int16_t gInterceptorSavedChannelNotify = -1; // -1 = no notification pending, else the channel just saved to
uint16_t gInterceptorSaveNotifyCountdown = 0; // real 10ms ticks remaining to show it
int8_t gInterceptorSaveFlashSlot = -1; // which grid cell was just saved - flashes the channel number in it as confirmation
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

// Not static - INTERCEPTOR_Engine_Tick (interceptor.c) calls this directly
// as a safety net, comparing gInterceptorTxOverrideActive against the PTT
// pin's actual real-time state rather than trusting key-event dispatch
// alone to always fire the release cleanly.
void End_Interceptor_PTT(void)
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

// Saves a grid cell out to the first available (unused) real memory
// channel - auto-selects the slot, no manual number entry needed. Uses
// the cell's given name if it has one, or leaves the channel unnamed
// (shows frequency only, same as any other unnamed memory channel) if
// it doesn't. Built on this firmware's own real channel-save mechanism,
// not a custom EEPROM write of our own.
static void Save_Cell_To_Memory(uint16_t slotIdx)
{
    if (gScanList[slotIdx].Frequency == 0) return; // nothing to save

    uint16_t targetChannel = 0xFFFF;
    for (uint16_t i = 0; i < 200; i++) {
        if (!RADIO_CheckValidChannel(i, false, 0)) { targetChannel = i; break; }
    }
    if (targetChannel == 0xFFFF) {
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL; // no free channel slots at all
        return;
    }

    // Start from a real VFO as a template for all the fields we don't
    // track ourselves (modulation, squelch thresholds, DTMF settings,
    // etc.), then override just what our grid cell actually knows about.
    VFO_Info_t tempVfo = *gRxVfo;
    tempVfo.freq_config_RX.Frequency = gScanList[slotIdx].Frequency;
    tempVfo.freq_config_TX.Frequency = gScanList[slotIdx].Frequency;
    tempVfo.freq_config_RX.CodeType  = gScanList[slotIdx].CodeType;
    tempVfo.freq_config_RX.Code      = gScanList[slotIdx].Code;
    tempVfo.freq_config_TX.CodeType  = gScanList[slotIdx].CodeType;
    tempVfo.freq_config_TX.Code      = gScanList[slotIdx].Code;
    tempVfo.CHANNEL_BANDWIDTH = BK4819_FILTER_BW_NARROW;
    tempVfo.TX_OFFSET_FREQUENCY = 0;
    memset(tempVfo.Name, 0, sizeof(tempVfo.Name));
    if (gScanList[slotIdx].Name[0] != '\0')
        strncpy(tempVfo.Name, gScanList[slotIdx].Name, 6);

    SETTINGS_SaveChannel((uint8_t)targetChannel, 0, &tempVfo, 3); // Mode 3 also saves the name

    // Now that this cell is backed by a real memory channel, mark it the
    // same way a manually-added one is: draws the underline, and protects
    // it from auto-eviction and popularity re-sorting. The cell already
    // holds exactly the frequency/name/tone just written to the channel,
    // so there's nothing to re-fetch - only the flag needs setting.
    gScanList[slotIdx].IsLocked = true;

    gInterceptorSavedChannelNotify = (int16_t)targetChannel;
    gInterceptorSaveNotifyCountdown = 300; // ~3 seconds, decremented in INTERCEPTOR_TimeSlice10ms
    gInterceptorSaveFlashSlot = (int8_t)slotIdx; // flash the channel number in this cell as visual confirmation
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
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
        // Mutually exclusive: long-press-detected fires once while still
        // held; short-press only resolves on release, and only if the
        // long-press event never fired for this same hold. Treating both
        // as independent events (as this used to) meant a held F+7 could
        // toggle sniffing on and then immediately force it back off from
        // one single button hold.
        if (bKeyHeld) {
            gWasFKeyPressed = false;
            gUpdateStatus   = true;
            gInterceptorBandSweepActive = false;
            gSniffingEnabled = false; // long press: force off
            gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
            gUpdateDisplay = true;
            return;
        }
        if (!bKeyPressed) {
            gWasFKeyPressed = false;
            gUpdateStatus   = true;
            gInterceptorBandSweepActive = false;
            gSniffingEnabled = !gSniffingEnabled; // genuine short press: toggle
            gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
            gUpdateDisplay = true;
            return;
        }
        return; // initial press - wait to see if this becomes short or long
    }

    // F+5 toggles the wide VHF/UHF band sweep, same "handle it on our own
    // screen too" reasoning as F+7 above.
    // F+1 saves the highlighted cell out to the first open real memory
    // channel - frequency, name (if one was set) and CTCSS/DCS tone (if
    // one was detected). Same clean-initial-press-only guard as the other
    // F+key handlers, so a held F+1 can't fire it twice.
    if (gWasFKeyPressed && Key == KEY_1 && gInterceptorNameEditIndex < 0 && !gInterceptorEnteringChannel) {
        if (!bKeyPressed || bKeyHeld) return;
        gWasFKeyPressed = false;
        gUpdateStatus   = true;
        Save_Cell_To_Memory(CurrentSlotIndex());
        return;
    }

    if (gWasFKeyPressed && Key == KEY_5) {
        // Only the clean initial press, not the long-press-detected event
        // that fires later on the same physical hold (without waiting for
        // release) - reacting to both meant a held F+5 could trigger this
        // twice, with the second firing landing on whatever screen the
        // first one had already switched to.
        if (!bKeyPressed || bKeyHeld) return;
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        // Opens the band-selection screen to reconfigure, instead of
        // directly toggling - F+5 again from there confirms and starts.
        gRequestDisplayScreen = DISPLAY_BAND_SELECT;
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
        gUpdateDisplay = true;
        return;
    }

    // F+STAR mutes/unmutes the highlighted cell - takes it out of scan
    // checking without deleting it, shown crossed-out on the grid.
    if (gWasFKeyPressed && Key == KEY_STAR) {
        if (!bKeyPressed || bKeyHeld) return;
        gWasFKeyPressed = false;
        gUpdateStatus   = true;

        uint16_t idx = CurrentSlotIndex();
        if (gScanList[idx].Frequency != 0) {
            gScanList[idx].Muted = !gScanList[idx].Muted;
            gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
            gUpdateDisplay = true;
        }
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
                INTERCEPTOR_DeleteAndBlacklist(idx, true);
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

    // Long-press MENU on a filled cell saves it out to a real memory
    // channel - short-press MENU (renaming) is unaffected, handled by the
    // normal switch below since bKeyHeld is never true for it. Fires the
    // moment the long-press is detected, while still held, so the status
    // bar confirmation is visible before you let go - not on release.
    if (Key == KEY_MENU && bKeyHeld && gInterceptorNameEditIndex < 0 && !gInterceptorEnteringChannel) {
        static bool sAlreadySavedThisHold = false;
        if (bKeyPressed) {
            if (!sAlreadySavedThisHold) {
                Save_Cell_To_Memory(CurrentSlotIndex());
                sAlreadySavedThisHold = true;
            }
        } else {
            sAlreadySavedThisHold = false; // released - ready for next time
        }
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
