#include "songEditor/songEditor.h"

#include "Platform.h"
#include "core/interfacePopups.h"
#include "fileManager.h"
#include "mtSequencer.h"
#include "string.h"

#include <cr_section_macros.h>

// 1 = a song section starts at this slot (for the song player marks)
__NOINIT(EXTERNAL_RAM) static uint8_t sectionStartFlags[SONG_MAX + 1];

static const uint16_t framesPlaces[3][4] = {
    { (800 / 8) * 0 + 1, 31, 500 - 3, 391 },
    { (800 / 8) * 5 + 1, 31, 800 / 4 - 3, 391 },
    { (800 / 8) * 7 + 1, 31, 800 / 8 - 3, 391 },
};

void cSongEditor::initDisplayControls()
{
    strControlProperties prop;
    frameData.placesCount = 3;
    frameData.startPlace = 2;

    frameData.places[0] = &framesPlaces[0][0];
    frameData.places[1] = &framesPlaces[1][0];
    frameData.places[2] = &framesPlaces[2][0];

    prop.style = 0;
    prop.value = 0;
    prop.colors = nullptr;
    prop.data = &frameData;
    frameControl = display.createControl<cFrame>(&prop);

    strControlProperties prop1;
    prop1.y = 29;
    prop1.w = 800 / 8 - 3;
    prop1.style = controlStyleValue_0_100 | controlStyleBackground |
                  (display.getTheme().barShadow > 0 ? controlStyleGradient : 0) |
                  (display.getTheme().bottomScreenShadow > 0 ? controlStyleBottomShadow : 0);
    prop1.h = 394;
    prop1.x = (800 / 8) * 7 + 1;
    barControl = display.createControl<cBar>(&prop1);

    doubleArrowRed.bitmaps[0].bitmapIndex = displayDoubleArrowL;
    doubleArrowRed.bitmaps[0].xValue = (800 / 8) * 5 + (800 / 16) - 10;
    doubleArrowRed.bitmaps[0].yValue = 445;
    doubleArrowRed.bitmaps[1].bitmapIndex = displayDoubleArrowR;
    doubleArrowRed.bitmaps[1].xValue = (800 / 8) * 6 + (800 / 16) + 10;
    doubleArrowRed.bitmaps[1].yValue = 445;

    doubleArrowRed.line.color = display.getTheme().red;
    doubleArrowRed.line.x1 = -90;
    doubleArrowRed.line.y1 = 50;
    doubleArrowRed.line.x2 = 90;
    doubleArrowRed.line.y2 = 50;

    labelDataRed.line.color = display.getTheme().red;
    labelDataRed.line.x1 = -40;
    labelDataRed.line.y1 = 50;
    labelDataRed.line.x2 = 40;
    labelDataRed.line.y2 = 50;

    strControlProperties prop2;
    prop2.text = (char*)"";
    prop2.colors = display.getTheme().activeButtonLabelsColors;

    prop2.w = 800 / 8 - 6;
    prop2.y = 424;
    prop2.h = 55;
    prop2.value = 1;

    for (uint8_t i = 0; i < 8; i++)
    {
        prop2.style = controlStyleCenterX | controlStyleCenterY | controlStyleFont3 |
                      ((display.getTheme().patternEditorBottomButtonsLines && (i >= 2 && i <= 5)
                          ? controlStyleSpecialFx
                          : 0));

        prop2.x = (800 / 8) * i + (800 / 16);
        if (i == 5)
        {
            prop2.x = (800 / 8) * i + (800 / 8);
        }

        label[i] = display.createControl<cLabel>(&prop2);
    }

    prop2.value = 191; // 191
    prop2.text = nullptr;
    prop2.colors = nullptr;
    prop2.style = controlStyleNoTransparency;
    prop2.x = 0;
    prop2.w = 800;
    prop2.y = 425;
    prop2.h = 54;
    bgLabel = display.createControl<cBgLabel>(&prop2);

    prop.data = nullptr;
    prop.x = 0 + 1;
    prop.y = 29;
    prop.w = ((800 / 8) * 7) - 3;
    prop.h = 394;
    prop.style =
      controlStyleBackground | controlStyleVerticalText |
      (mtConfig.general.numberingMode == NumberingMode::kHexadecimal ? controlStyleHexNumbers : 0) |
      (mtConfig.general.numberingMode == NumberingMode::kDecimalWithStepsFrom1
         ? controlStyleStartNumbersFrom1
         : 0) |
      (display.getTheme().bottomScreenShadow > 0 ? controlStyleBottomShadow : 0);

    songPlayerControl = display.createControl<cSongPlayer>(&prop);
}

void cSongEditor::refreshTempoField()
{
    if (toolsPage || namingSection)
    {
        return;
    }

    if (tapTempoActive && tapTempoCount > 1)
    {
        memset(tapText, 0, sizeof(tapText));
        snprintf(tapText, sizeof(tapText), "TAP %d/4", tapTempoCount);

        display.setControlText(label[7], tapText);
    }
    else
    {
        if (sequencer.isSetInternalClock())
        {
            display.setControlText(label[7], "Tempo");
        }
        else
        {
            display.setControlText(label[7], "Ext Tempo");
        }
    }
    display.refreshControl(label[7]);
    showTempoValue();
}

void cSongEditor::refreshAddSlotLabel(bool isButtonPressed)
{
    if (toolsPage || namingSection)
    {
        return;
    }

    if (isButtonPressed)
    {
        display.setControlText(label[3], "Add");
        display.setControlText2(label[3], "Empty Slot");
    }
    else
    {
        display.setControlText(label[3], "Add");
        display.setControlText2(label[3], "Slot");
    }
    display.refreshControl(label[3]);
}

void cSongEditor::showDefaultScreen()
{
    titleBar->show();
    refreshTitle();

    // labels 5 / 6 are moved and re-styled on the tools page
    display.setControlPosition(label[5], (800 / 8) * 5 + (800 / 8), -1);
    display.setControlPosition(label[6], (800 / 8) * 6 + (800 / 16), -1);

    display.setControlStyle2(label[7],
                             controlStyleCenterX | controlStyleCenterY | controlStyleFont2);
    display.setControlData(label[2], &labelDataRed);
    display.setControlData(label[3], &labelDataRed);
    display.setControlData(label[4], &labelDataRed);
    display.setControlData(label[5], &doubleArrowRed);
    display.setAddControlStyle(label[5], controlStyleShowBitmap);

    playButtonsState = 0;

    display.setControlText(label[0], "Play");
    display.setControlText2(label[0], "Pattern");

    display.setControlText(label[1], "Play");
    display.setControlText2(label[1], "Song");

    display.setControlText(label[2], "Undo");

    if (buttons->isButtonPressed(interfaceButtonShift))
    {
        display.setControlText(label[3], "Add");
        display.setControlText2(label[3], "Empty Slot");
    }
    else
    {
        display.setControlText(label[3], "Add");
        display.setControlText2(label[3], "Slot");
    }

    display.setControlText(label[4], "Delete");
    display.setControlText2(label[4], "Slot");

    display.setControlText(label[5], "Pattern");
    display.setControlText2(label[5], "");

    display.setControlText(label[6], "");
    display.setControlText2(label[6], "");

    display.setControlText(label[7], "Tempo");

    if (sequencer.isSetInternalClock())
    {
        display.setControlColors(label[7], display.getTheme().activeLabelsColors);
    }
    else
    {
        display.setControlColors(label[7], display.getTheme().inactiveLabelsColors);
    }

    showTempoValue();

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlValue(label[i], 1);
        display.setControlShow(label[i]);
        display.refreshControl(label[i]);
    }

    display.setControlValue(label[2], 0);
    display.setControlValue(label[5], 0);

    display.setControlShow(bgLabel);
    display.refreshControl(bgLabel);

    display.setControlShow(songPlayerControl);
    display.refreshControl(songPlayerControl);

    display.synchronizeRefresh();
}

//==============================================================================================================
void cSongEditor::refreshSongPlayerControl()
{
    display.refreshControl(songPlayerControl);
    display.synchronizeRefresh();
}

void cSongEditor::refreshSongPlayerSelectedSlot()
{
    display.setControlValue(songPlayerControl, selectedSongPosition);
    display.refreshControl(songPlayerControl);
    display.synchronizeRefresh();
}

void cSongEditor::refreshButtonsLabels()
{
    if (toolsPage || namingSection)
    {
        return;
    }

    Sequencer::enSeqState seqState = sequencer.getSeqState();
    if (seqState == Sequencer::SEQ_STATE_STOP)
    {
        if (buttons->isButtonPressed(interfaceButtonShift) && selectedPlace <= 0 &&
            songPlayerData.selection.trackSelectionLength == 1 &&
            songPlayerData.selection.patternSelectionLength == 1)
        {
            if (playButtonsState != 1)
            {
                display.setControlText(label[0], "Preview");
                display.setControlText2(label[0], "Track");

                display.setControlText(label[1], "Play");
                display.setControlText2(label[1], "Song");

                display.refreshControl(label[0]);
                display.refreshControl(label[1]);
                playButtonsState = 1;
            }
        }
        else
        {
            if (playButtonsState != 0)
            {
                display.setControlText(label[0], "Play");
                display.setControlText2(label[0], "Pattern");

                display.setControlText(label[1], "Play");
                display.setControlText2(label[1], "Song");

                display.refreshControl(label[0]);
                display.refreshControl(label[1]);
                playButtonsState = 0;
            }
        }
    }
    else if (seqState == Sequencer::SEQ_STATE_PLAY_SONG)
    {
        if (playButtonsState != 2)
        {
            display.setControlText(label[0], "Loop");
            display.setControlText2(label[0], "Pattern");
            display.setControlText(label[1], "Stop");
            display.setControlText2(label[1], "");
            display.refreshControl(label[0]);
            display.refreshControl(label[1]);
            playButtonsState = 2;
        }
    }
    else if (seqState == Sequencer::SEQ_STATE_PLAY_PATTERN)
    {
        if (playButtonsState != 3)
        {
            display.setControlText(label[0], "Continue");
            display.setControlText2(label[0], "Song");
            display.setControlText(label[1], "Stop");
            display.setControlText2(label[1], "");
            display.refreshControl(label[0]);
            display.refreshControl(label[1]);
            playButtonsState = 3;
        }
    }
    else if (seqState == Sequencer::SEQ_STATE_PLAY_PERFORMANCE)
    {
        if (sequencer.isSongMode())
        {
            // ktoś wcisnął "Continue Song" i teraz ma jeszcze krótką chwilę,
            // żeby zmienić zdanie
            if (playButtonsState != 4)
            {
                display.setControlText(label[0], "Loop");
                display.setControlText2(label[0], "Performance");
                display.setControlText(label[1], "Stop");
                display.setControlText2(label[1], "");
                display.refreshControl(label[0]);
                display.refreshControl(label[1]);
                playButtonsState = 4;
            }
        }
        else
        {
            if (playButtonsState != 3)
            {
                display.setControlText(label[0], "Continue");
                display.setControlText2(label[0], "Song");
                display.setControlText(label[1], "Stop");
                display.setControlText2(label[1], "");
                display.refreshControl(label[0]);
                display.refreshControl(label[1]);
                playButtonsState = 3;
            }
        }
    }
    else
    {
        if (playButtonsState != 5)
        {
            display.setControlText(label[0], "Stop");
            display.setControlText2(label[0], "");
            display.setControlText(label[1], "Stop");
            display.setControlText2(label[1], "");
            display.refreshControl(label[0]);
            display.refreshControl(label[1]);
            playButtonsState = 5;
        }
    }
}

//==============================================================================================================

void cSongEditor::activateLabelsBorder()
{
    if (selectedPlace > frameData.placesCount - 1)
        return;

    if (selectedPlace == 2) // tempo
    {
        display.setControlValue(frameControl, selectedPlace);
        display.setControlShow(frameControl);
        display.refreshControl(frameControl);
        songPlayerData.isListActive = 0;
        songPlayerData.selection.isActive = 0;
        display.refreshControl(songPlayerControl);
        display.synchronizeRefresh();
        return;
    }
    else if (selectedPlace == 1) // lista
    {
        songPlayerData.isListActive = 1;
        songPlayerData.selection.isActive = 0;
        display.refreshControl(songPlayerControl);
    }
    else // tracks matrix
    {
        songPlayerData.isListActive = 0;
        songPlayerData.selection.isActive = 1;
        display.refreshControl(songPlayerControl);
    }

    display.setControlHide(frameControl);
    display.refreshControl(frameControl);
    display.synchronizeRefresh();
}

void cSongEditor::showTempoValue()
{
    if (sequencer.isSetInternalClock())
    {
        sprintf(globalTempoVal, "%.1f BPM", mtProject.values.globalTempo);
    }
    else
    {
        float externalTempo = sequencer.getExternalTempo();

        if (externalTempo == Sequencer::kUnknownExternalTempo)
        {
            sprintf(globalTempoVal, "NO CLK");
        }
        else
        {
            sprintf(globalTempoVal, "%.0f BPM", externalTempo);
        }
    }

    if (!toolsPage && !namingSection)
    {
        display.setControlText2(label[7], globalTempoVal);
        display.refreshControl(label[7]);
    }

    display.setControlShow(barControl);
    display.setControlValue(barControl,
                            (mtProject.values.globalTempo * 100) / Sequencer::MAX_TEMPO);
    display.refreshControl(barControl);
}

void cSongEditor::showIcon(icon_t iconType, uint8_t position)
{
    patternsList.icon.useSpecialIcons = 1;
    patternsList.icon.iconPositionInList = position;
    patternsList.icon.iconNum = displayPlayIcon + iconType;

    // display.setControlValue(songPlayerControl, selectedPattern);
    display.refreshControl(songPlayerControl);
}

void cSongEditor::hideIcon()
{
    patternsList.icon.useSpecialIcons = 0;
    patternsList.icon.iconNum = displayPlayIcon;
    patternsList.icon.iconPositionInList = 0;

    // display.setControlValue(songPlayerControl, selectedPattern);
    display.refreshControl(songPlayerControl);
}

//==============================================================================================================
// Twisted song tools
//==============================================================================================================

void cSongEditor::refreshTitle()
{
    int16_t loopStart = newFileManager.getSongLoopStart();
    int16_t loopEnd = newFileManager.getSongLoopEnd();

    if (loopMode == SongLoopMode::section && loopStart >= 0)
    {
        const char* name = newFileManager.getTwistedSong()->getSectionNameForPos(loopStart);
        if (name[0] != 0)
        {
            snprintf(titleText, sizeof(titleText), "Song   LOOP: %s", name);
        }
        else
        {
            snprintf(titleText, sizeof(titleText), "Song   LOOP: %d-%d", loopStart + 1, loopEnd + 1);
        }
    }
    else if (loopMode == SongLoopMode::selection && loopStart >= 0)
    {
        snprintf(titleText, sizeof(titleText), "Song   LOOP: %d-%d", loopStart + 1, loopEnd + 1);
    }
    else if (toolsPage)
    {
        snprintf(titleText, sizeof(titleText), "Song Tools");
    }
    else
    {
        snprintf(titleText, sizeof(titleText), "Song");
    }

    titleBar->setTitle(titleText);
}

void cSongEditor::refreshSongMarks()
{
    TwistedSong* ts = newFileManager.getTwistedSong();

    memset(sectionStartFlags, 0, sizeof(sectionStartFlags));
    for (uint8_t i = 0; i < ts->getSectionCount(); i++)
    {
        uint8_t start = ts->getSectionStart(i);
        if (start < SONG_MAX)
        {
            sectionStartFlags[start] = 1;
        }
    }

    songPlayerData.sectionStarts = sectionStartFlags;
    songPlayerData.loopStart = (loopMode != SongLoopMode::off) ? newFileManager.getSongLoopStart() : -1;
    songPlayerData.loopEnd = (loopMode != SongLoopMode::off) ? newFileManager.getSongLoopEnd() : -1;
    songPlayerData.queuedPos = newFileManager.getQueuedSongJump();
}

void cSongEditor::refreshToolsLabels()
{
    static const char* const kText1[8] = { "Section", "Section", "Loop",  "Loop",
                                           "Clone",   "Make",    "Back",  "Unlink" };
    static const char* const kText2[8] = { "Name",     "Remove",    "Section", "Selection",
                                           "In Place", "Variation", "",        "Variation" };
    // the ones that change the song need edit (REC) mode
    static const bool kNeedsEdit[8] = { true, true, false, false, true, true, false, true };

    display.setControlPosition(label[5], (800 / 8) * 5 + (800 / 16), -1);
    display.setControlPosition(label[6], (800 / 8) * 6 + (800 / 16), -1);
    display.setRemoveControlStyle(label[5], controlStyleShowBitmap);
    display.setControlData(label[5], &labelDataRed);
    display.setControlStyle2(label[7],
                             controlStyleCenterX | controlStyleCenterY | controlStyleFont3);

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlText(label[i], kText1[i]);
        display.setControlText2(label[i], kText2[i]);
        display.setControlValue(label[i], kText2[i][0] != 0 ? 1 : 0);

        bool active = !kNeedsEdit[i] || editMode;

        display.setControlColors(label[i],
                                 active ? display.getTheme().activeButtonLabelsColors
                                        : display.getTheme().inactiveButtonLabelsColors);
        display.setControlShow(label[i]);
        display.refreshControl(label[i]);
    }

    // show which loop is on
    display.setControlText2(label[2], loopMode == SongLoopMode::section ? "Section ON" : "Section");
    display.setControlText2(label[3],
                            loopMode == SongLoopMode::selection ? "Selection ON" : "Selection");
    display.refreshControl(label[2]);
    display.refreshControl(label[3]);

    display.synchronizeRefresh();
}

void cSongEditor::showMessage(const char* line1, const char* line2)
{
    strncpy(messageLine1, line1 != nullptr ? line1 : "", sizeof(messageLine1) - 1);
    messageLine1[sizeof(messageLine1) - 1] = 0;

    if (line2 != nullptr)
    {
        strncpy(messageLine2, line2, sizeof(messageLine2) - 1);
        messageLine2[sizeof(messageLine2) - 1] = 0;
    }

    mtPopups.showInfoPopup(messageLine1, line2 != nullptr ? messageLine2 : nullptr);
    messageVisible = true;
    messageTimer = 0;
}

void cSongEditor::updateMessage()
{
    if (messageVisible && messageTimer > 1500)
    {
        messageVisible = false;
        mtPopups.hideInfoPopup();
        display.refreshControl(songPlayerControl);
        display.synchronizeRefresh();
    }
}

void cSongEditor::showSectionKeyboard()
{
    snprintf(titleText, sizeof(titleText), "Name Section at Slot %d", namingSectionPos + 1);
    titleBar->show(true);
    titleBar->setTitle(titleText);

    display.setControlHide(songPlayerControl);
    display.setControlHide(frameControl);
    display.setControlHide(barControl);

    display.setControlPosition(label[5], (800 / 8) * 5 + (800 / 16), -1);
    display.setControlPosition(label[6], (800 / 8) * 6 + (800 / 16), -1);
    display.setRemoveControlStyle(label[5], controlStyleShowBitmap);
    display.setControlData(label[5], &labelDataRed);

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlValue(label[i], 0);
        display.setControlText(label[i], "");
        display.setControlText2(label[i], "");
        display.setControlColors(label[i], display.getTheme().activeButtonLabelsColors);
    }

    display.setControlText(label[0], "Enter");
    display.setControlText(label[6], "Cancel");
    display.setControlText(label[7], "Save");

    for (uint8_t i = 0; i < 8; i++)
    {
        display.refreshControl(label[i]);
    }

    display.setControlValue(bgLabel, 255);
    display.refreshControl(bgLabel);

    mtPopups.hideInfoPopup();
    messageVisible = false;

    display.synchronizeRefresh();
}
