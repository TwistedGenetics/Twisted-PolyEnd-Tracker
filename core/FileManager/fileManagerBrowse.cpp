#include "fileManagerDefs.h"
#include "mtConfig.h"
#include "mtStructs.h"

#include "string.h"
#include "util_string.h"

#include "mtSequencer.h"

#include "AudioDataUtilities.h"
#include "fileManager.h"
#include "fileTransfer.h"
#include "Tracker.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <stdio.h>

void cFileManager::updateBrowseSamples() // fmBrowseSamples - 5
{
    switch (currentOperationStep)
    {
        case 0: browseCurrentLocation(); break;
        case 1: listOnlyWavFromActualPath(); break;
        case 2: processDirFileSizes(); break;
        default: stopOperationWithError(fmBrowseSamplesError); break;
    }
}

void cFileManager::updateBrowseFirmwares() // fmBrowseFirmwares - 6
{
    switch (currentOperationStep)
    {
        case 0: browseFirmwaresLocation(); break;
        default: stopOperationWithError(fmBrowseFirmwaresError); break;
    }
}

void cFileManager::updateBrowseGames()
{
    switch (currentOperationStep)
    {
        case 0: browseGamesLocation(); break;
        default: stopOperationWithError(fmBrowseGamesError); break;
    }
}

void parseFolderName(const char* input, char* name, uint16_t* offset)
{
    const char* start = strchr(input, '(');
    uint16_t dummy;
    int items;

    if (start == NULL)
    {
        strcpy(name, input);
        *offset = 0;
        return;
    }

    items = sscanf(start, "(%" SCNd16 "-%" SCNd16 ")", offset, &dummy);

    if (items == 2)
    {
        strncpy(name, input, start - input);
        name[start - input] = '\0';
        (*offset)--;
    }
    else
    {
        strcpy(name, input);
        *offset = 0;
    }
}

bool cFileManager::browseSdCard(uint16_t* index)
{
    if (status != fmIdle && status != fmSavingProjectToWorkspace)
        return false;
    if (currentOperation != fmNoOperation && currentOperation != fmSaveWorkspaceProject)
        return false;

    // dla bezpieczenstwa reset sciezki jesli jestesmy na poziomie 0
    if (explorerDirLevel == 0)
    {
        strcpy(explorerCurrentPath, "/");
    }

    if (index == nullptr) // tylko odswiez
    {
        status = fmBrowsingSamples;
        currentOperationStep = 0;
        currentOperation = fmBrowseSamples;

        return true;
    }

    if (explorerDirLevel == 0)
    {
        explorerDirLevel = 1;

        parseFolderName(explorerList[*index], explorerCurrentPath, &explorerCurrentOffset);
        explorerPositionTable[explorerCurrentPosition] = *index;
        explorerCurrentPosition++;

        *index = 0;
    }
    else
    {
        if (*index > 0)
        {
            if (explorerCurrentPosition < PREVIOUS_POSITION_LIFO)
            {
                explorerDirLevel++;

                explorerPositionTable[explorerCurrentPosition] = *index;
                explorerCurrentPosition++;
                parseFolderName(explorerList[*index],
                                explorerCurrentPath + strlen(explorerCurrentPath),
                                &explorerCurrentOffset);
                *index = 0;
            }
        }
        else
        {
            explorerDirLevel--;

            if (explorerDirLevel > 0)
            {
                goUpInActualPath();
            }
            else
            {
                strcpy(explorerCurrentPath, "/");
            }

            explorerCurrentPosition--;
            *index = explorerPositionTable[explorerCurrentPosition];
            explorerCurrentOffset = 0;
        }
    }

    status = fmBrowsingSamples;
    currentOperationStep = 0;
    currentOperation = fmBrowseSamples;

    return true;
}

// wymusza przy nastepnym wywolaniu browseSdCard()
// wyzerowanie pokazywanej lokalizaji do poczatku karty SD
void cFileManager::resetBrowse()
{
    explorerDirLevel = 0;
    explorerCurrentPosition = 0;
    // strcpy(explorerCurrentPath, "/");
}

char** cFileManager::getExplorerList()
{
    return explorerList;
}

int cFileManager::getExplorerListLength()
{
    return explorerListLength;
}

char* cFileManager::getExplorerPath()
{
    return explorerCurrentPath;
}

uint16_t cFileManager::getBrowsedFilesList(char*** list, uint32_t** memoryList)
{
    *list = explorerList;
    *memoryList = currentFolderMemoryFileUsage;

    return explorerListLength;
}

void cFileManager::browseCurrentLocation()
{
    explorerListLength = 0;
    listWavStage = 0;

    listOnlyFolderNames();
    moveToNextOperationStep();
}

void cFileManager::listOnlyFolderNames()
{
    int allFoldersNum;
    uint8_t parentFolder = explorerDirLevel == 0 ? 0 : 1;
    FileSystem::enListFileFilter folderSizeFilter = FileSystem::enListFileFilter_all;

    switch (currentOperation)
    {
        case fmBrowseSamples: folderSizeFilter = FileSystem::enListFileFilter_wave; break;
        case fmBrowseProjects: folderSizeFilter = FileSystem::enListFileFilter_folder; break;
        case fmBrowseMods: folderSizeFilter = FileSystem::enListFileFilter_mod; break;
        case fmBrowseFirmwares: folderSizeFilter = FileSystem::enListFileFilter_firmware; break;
    }

    allFoldersNum = fileSystem->createFilesList(explorerCurrentPath,
                                                parentFolder,
                                                explorerList,
                                                explorer_list_max,
                                                8000,
                                                FileSystem::enListFileFilter_folder,
                                                0,
                                                explorer_list_max - 1,
                                                folderSizeFilter);

    if (allFoldersNum < parentFolder) // kListError
    {
        allFoldersNum = parentFolder;
    }

    explorerFolderCount = allFoldersNum - parentFolder;

    if (explorerCurrentOffset != 0)
    {
        allFoldersNum = parentFolder;

        for (int i = 0; i < explorer_list_max; i++)
        {
            if (explorerList[i] != nullptr)
            {
                delete explorerList[i];
                explorerList[i] = nullptr;
            }
        }
    }

    if (parentFolder)
    {
        explorerList[0] = new char[4];
        strcpy(explorerList[0], "/..");
    }

    std::sort(&explorerList[parentFolder],
              &explorerList[parentFolder] + allFoldersNum - parentFolder,
              [](char* a, char* b) { return compareStrings(a, b); });

    explorerListLength += allFoldersNum;
}

void cFileManager::listOnlyWavFromActualPath()
{
    // Twisted: faster browsing.
    //  - names are sorted straight after listing, then each file is opened ONCE
    //    to both validate it and work out its memory size (stock firmware opened
    //    every wav twice, plus an extra directory search)
    //  - as many files as fit in a short time slice are checked per update,
    //    instead of one file per update
    if (listWavStage == 0) // stage 0 - list
    {
        int filesFound =
          fileSystem->createFilesList(explorerCurrentPath,
                                      0,
                                      explorerList + explorerListLength,
                                      (explorer_list_max - explorerListLength),
                                      24000,
                                      FileSystem::enListFileFilter_wave_inst,
                                      std::max(explorerCurrentOffset - explorerFolderCount, 0));

        if (filesFound <= 0) // nie ma plikow wav w folderze (lub blad) - koncz listowanie
        {
            browseFinish();
            return;
        }

        openCalcStart = explorerListLength;
        explorerListLength += filesFound;
        openCalcEnd = explorerListLength;

        if (openCalcStart < explorerListLength - 1)
        {
            std::sort(explorerList + openCalcStart,
                      explorerList + explorerListLength,
                      [](char* a, char* b) { return compareStrings(a, b); });
        }

        openCurrentPos = openCalcStart;
        listWavStage = 1;
    }
    else // stage 1 - validate + size, several files per update
    {
        Timer* timer = Tracker::getInstance()->getPlatform()->getTimer();
        const uint32_t sliceStart = timer->millis();
        constexpr uint32_t kSliceMs = 12;

        while (openCurrentPos < explorerListLength)
        {
            if (browseCheckOneFile(openCurrentPos))
            {
                openCurrentPos++;
            }
            else
            {
                // drop unsupported file, keep the sorted order
                delete explorerList[openCurrentPos];
                uint16_t tail = explorerListLength - openCurrentPos - 1;
                if (tail > 0)
                {
                    memmove(&explorerList[openCurrentPos],
                            &explorerList[openCurrentPos + 1],
                            tail * sizeof(explorerList[0]));
                }
                explorerListLength--;
                explorerList[explorerListLength] = nullptr;
            }

            if ((timer->millis() - sliceStart) >= kSliceMs)
            {
                return; // continue on next update
            }
        }

        listWavStage = 0;
        browseFinish();
    }
}

bool cFileManager::browseCheckOneFile(uint16_t pos)
{
    const char* name = explorerList[pos];
    currentFolderMemoryFileUsage[pos] = 0;

    if (name == nullptr)
    {
        return false;
    }

    char filePath[512];
    snprintf(filePath,
             sizeof(filePath),
             (explorerDirLevel > 0) ? "%s/%s" : "%s%s",
             explorerCurrentPath,
             name);

    if (hasFileExtension(name, "pti"))
    {
        currentFolderMemoryFileUsage[pos] = getInstrumentSampleSizeIfValid(filePath);
        return true; // instruments are always listed (as stock)
    }

    if (!hasFileExtension(name, "wav"))
    {
        return false;
    }

    auto [isOpen, fileHandle] = fileSystem->openFile(filePath, FileSystem::FileOpenMode::kRead);
    if (!isOpen)
    {
        return false;
    }

    strWavFileHeader header = {};
    bool headerRead = readHeader(&header, fileHandle);
    uint32_t wavDataSize = headerRead ? fileSystem->getFileAvailable(fileHandle) : 0;
    fileSystem->closeFile(fileHandle);

    if (!headerRead || !checkSampleValid(header))
    {
        return false;
    }

    // same maths as getWaveSizeAfterConversion()
    uint16_t allChannelBytesPerOneSample = (header.bitsPerSample / 8) * header.numChannels;
    if (header.sampleRate == 0 || allChannelBytesPerOneSample == 0 || wavDataSize == 0)
    {
        return true;
    }

    uint32_t wavDataSizeBytes =
      (wavDataSize <= header.subchunk2Size) ? wavDataSize : header.subchunk2Size;
    uint32_t totalSamplesCountPerChannel = wavDataSizeBytes / allChannelBytesPerOneSample;
    uint64_t samplesCount =
      (uint64_t)totalSamplesCountPerChannel * targetSampleRate / header.sampleRate;

    currentFolderMemoryFileUsage[pos] =
      samplesCount * ((stereoSampleMode && header.numChannels >= 2) ? 4 : 2);
    return true;
}

void cFileManager::processDirFileSizes() // wykonywalne w petli
{
    char filePath[PATCH_SIZE];

    strcpy(filePath, explorerCurrentPath);
    if (explorerDirLevel > 0)
    {
        strcat(filePath, "/");
    }

    strcat(filePath, explorerList[openCurrentPos]);

    // if its wav file get size after conversion
    currentFolderMemoryFileUsage[openCurrentPos] =
      getWaveSizeAfterConversion(filePath, stereoSampleMode);

    // if its instrument file get pti sample data size
    if (currentFolderMemoryFileUsage[openCurrentPos] == 0)
    {
        currentFolderMemoryFileUsage[openCurrentPos] = getInstrumentSampleSizeIfValid(filePath);
    }

    openCurrentPos++;

    // uint8_t progress = (((openCurrentPos - openCalcStart) * 100) / (openCalcEnd-openCalcStart));
    // todo progress

    if (openCurrentPos >= explorerListLength)
    {
        browseFinish();
        // finisz
    }
}

void cFileManager::browseFinish()
{
    status = fmBrowseSamplesEnd;
    currentOperationStep = 0;
    currentOperation = fmNoOperation;
}

void cFileManager::goUpInActualPath()
{
    uint8_t length = strlen(explorerCurrentPath);

    explorerCurrentPath[length] = 0;

    char* ptr = strrchr(explorerCurrentPath, '/');

    if (ptr == nullptr)
    {
        strcpy(explorerCurrentPath, "/");
        return;
    }

    explorerCurrentPath[(ptr - explorerCurrentPath)] = 0;
}

///====================================================================================================
///====================================================================================================
///====================================================================================================
void cFileManager::browseFirmwaresLocation()
{

    uint8_t locationFileCount = fileSystem->createFilesList(
      "Firmware", 0, firmwaresList, list_length_max, 6000, fileSystem->enListFileFilter_firmware);

    if (locationFileCount == fileSystem->kListError)
    {
        stopOperationWithError(fmBrowseFirmwaresError);
        return;
    }

    std::sort(firmwaresList,
              firmwaresList + locationFileCount,
              [](char* a, char* b) { return !compareStrings(a, b); });

    firmwaresListLength = locationFileCount;

    status = fmBrowseFirmwaresEnd;
    currentOperationStep = 0;
    currentOperation = fmNoOperation;
}

bool cFileManager::browseFirmwares()
{
    status = fmBrowsingFirmwares;
    currentOperationStep = 0;
    currentOperation = fmBrowseFirmwares;

    return true;
}

uint8_t cFileManager::getFirmwaresList(char*** list)
{
    *list = firmwaresList;
    return firmwaresListLength;
}

///====================================================================================================
///====================================       GAMES ===============================================
///====================================================================================================
void cFileManager::browseGamesLocation()
{
    if (!fileSystem->exists(cGamesPath))
    {
        fileSystem->mkdir(0, cGamesPath);
    }

    uint8_t gamesfoundCount = fileSystem->createFilesList(
      cGamesPath, 0, projectsList, list_length_max, 3000, FileSystem::enListFileFilter_game);

    if (gamesfoundCount == FileSystem::kListError)
    {
        stopOperationWithError(fmBrowseGamesError);
        return;
    }

    std::sort(projectsList,
              projectsList + gamesfoundCount,
              [](char* a, char* b) { return compareStrings(a, b); });

    char lowCase[50];
    // usuwanie koncówek .nes
    for (uint8_t i = 0; i < gamesfoundCount; i++)
    {
        strcpy(lowCase, projectsList[i]);
        for (uint16_t a = 0; a < 50; a++)
        {
            lowCase[a] = tolower(lowCase[a]);
            if (lowCase[a] == 0)
                break;
        }

        char* nes = strstr(lowCase, ".nes\0");
        if (nes != nullptr)
        {
            *(projectsList[i] + (nes - lowCase)) = 0;
        }
    }

    projectsListLength = gamesfoundCount;

    status = fmBrowseGamesEnd;
    currentOperationStep = 0;
    currentOperation = fmNoOperation;
}

bool cFileManager::browseGames()
{
    if (status != fmIdle && status != fmSavingProjectToWorkspace)
        return false;
    if (currentOperation != fmNoOperation && currentOperation != fmSaveWorkspaceProject)
        return false;

    status = fmBrowsingGames;
    currentOperationStep = 0;
    currentOperation = fmBrowseGames;

    return true;
}

uint8_t cFileManager::getGamesList(char*** list)
{
    *list = projectsList;
    return projectsListLength;
}

const char* cFileManager::getProjectsDefaultPath()
{
    return cDefaultProjectsPath;
}

const char* cFileManager::getModsDefaultPath()
{
    return cModsPath;
}

void cFileManager::createProjectsAndModFolders()
{
    if (!fileSystem->exists(cDefaultProjectsPath))
    {
        fileSystem->mkdir(0, cDefaultProjectsPath);
    }

    if (!fileSystem->exists(cModsPath))
    {
        fileSystem->mkdir(0, cModsPath);
    }
}

void cFileManager::createPatchesFolder()
{
    if (!fileSystem->exists(cDefaultPatchesPath))
    {
        fileSystem->mkdir(0, cDefaultPatchesPath);
    }
}
