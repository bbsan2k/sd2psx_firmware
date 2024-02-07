#include "history_tracker/ps2_history_tracker.h"

#include <debug.h>
#include <game_names/game_names.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "card_emu/ps2_sd2psxman.h"
#include "card_emu/ps2_sd2psxman_commands.h"
#include "mcfat.h"
#include "mcio.h"
#include "pico/time.h"
#include "pico/types.h"
#include "ps2_cardman.h"
#include "ps2_dirty.h"
#include "ps2_psram.h"

//#define USE_INJECT_LOGIC

#define HISTORY_FILE_SIZE           462
#define HISTORY_ENTRY_COUNT         21
#define HISTORY_ENTRY_SIZE          22
#define HISTORY_ENTRY_POS_LAUNCH    16
#define HISTORY_WRITE_HYST_US       3 * 1000 * 1000
#define HISTORY_NUMBER_OF_REGIONS   4

#define CHAR_CHINA              'C'
#define CHAR_NORTHAMERICA       'A'
#define CHAR_EUROPE             'E'
#define CHAR_JAPAN              'I'
#define SYSTEMDATA_DIRNAME      "/B%cDATA-SYSTEM"
#define HISTORY_FILENAME_FORMAT "/B%cDATA-SYSTEM/history"

static mcfat_cardspecs_t cardspecs;
static mcfat_mcops_t mcOps;
static uint64_t lastWrite = 0U;

const char regionList[] = {CHAR_CHINA, CHAR_NORTHAMERICA, CHAR_EUROPE, CHAR_JAPAN};
static uint8_t slotCount[HISTORY_NUMBER_OF_REGIONS][HISTORY_ENTRY_COUNT] = {};
static uint32_t fileCluster[HISTORY_NUMBER_OF_REGIONS] = {};
static bool refreshRequired[HISTORY_NUMBER_OF_REGIONS];

int page_erase(mcfat_cardspecs_t* info, uint32_t page) {
    if (page * info->pagesize + info->pagesize <= ps2_cardman_get_card_size()) {
        uint8_t buff[info->pagesize];
        memset((void*)buff, 0xFF, info->pagesize);
        ps2_dirty_lockout_renew();
        ps2_dirty_lock();
        psram_write(page * info->pagesize, buff, info->pagesize);
        ps2_dirty_mark(page);
        ps2_dirty_unlock();
    }
    return sceMcResSucceed;
}

int page_write(mcfat_cardspecs_t* info, uint32_t page, void* buff) {
    if (page * info->pagesize + info->pagesize <= ps2_cardman_get_card_size()) {
        ps2_dirty_lockout_renew();
        ps2_dirty_lock();
        psram_write(page * info->pagesize, buff, info->pagesize);
        ps2_dirty_mark(page);
        ps2_dirty_unlock();
    }
    return sceMcResSucceed;
}

int page_read(mcfat_cardspecs_t* info, uint32_t page, uint32_t count, void* buff) {
    psram_read(page * info->pagesize, buff, count);
    return sceMcResSucceed;
}

static bool fileExists(char* filename) {
    int fd = mcio_mcOpen(filename, sceMcFileAttrReadable);
    debug_printf("File %s status %d\n", filename, fd);
    if ( fd < 0 )
        return false;
    else
        mcio_mcClose(fd);

    return true;
}

static bool dirExists(char* dirname) {
    int fd = mcio_mcDopen(dirname);
    debug_printf("Dir %s status %d\n", dirname, fd);

    if ( fd < 0 )
        return false;
    else
        mcio_mcDclose(fd);
    return true;
}

static void readSlots(uint8_t historyFile[HISTORY_FILE_SIZE], uint8_t slots[HISTORY_ENTRY_COUNT]) {
    for (int i = 0; i < HISTORY_ENTRY_COUNT; i++) {
        if (historyFile[i * HISTORY_ENTRY_SIZE]) {
            slots[i] = historyFile[i * HISTORY_ENTRY_SIZE + HISTORY_ENTRY_POS_LAUNCH];
            debug_printf("Found game %s with %d counts\n", (char*)&historyFile[i * HISTORY_ENTRY_SIZE],
                   historyFile[i * HISTORY_ENTRY_SIZE + HISTORY_ENTRY_POS_LAUNCH]);
        } else {
            slots[i] = 0;
        }
    }
}

void ps2_history_tracker_registerPageWrite(uint32_t page) {
    uint32_t cluster = page / 2;
    for (int i = 0; i < HISTORY_NUMBER_OF_REGIONS; i++) {
        if (cluster == fileCluster[i]) {
            refreshRequired[i] = true;
        }
    }
}

void ps2_history_tracker_card_changed() {
    mcfat_setCardChanged(true);
    mcio_init();
    uint8_t buff[HISTORY_FILE_SIZE] = {0x00};
    char filename[23] = {0x00};
    char dirname[15] = {0x00};
    for (int i = 0; i < HISTORY_NUMBER_OF_REGIONS; i++) {
        // Read current history file for each region
        snprintf(dirname, 15, SYSTEMDATA_DIRNAME, regionList[i]);
        snprintf(filename, 23, HISTORY_FILENAME_FORMAT, regionList[i]);
        memset((void*)buff, 0x00, HISTORY_FILE_SIZE);
        #ifdef USE_INJECT_LOGIC
        if (!dirExists(dirname)) {
            int dirsts = mcio_mcMkDir(dirname);
            debug_printf("Dir Creating Status is %d\n", dirsts);
        }
        #endif
        if (fileExists(filename)) {
            int fh = mcio_mcOpen(filename, sceMcFileAttrReadable);
            debug_printf("Initially reading filename %s, fd %d\n", filename, fh);
            if (fh >= 0) {
                mcio_mcRead(fh, buff, HISTORY_FILE_SIZE);
                readSlots(buff, slotCount[i]);
                fileCluster[i] = mcio_mcGetCluster(fh);
                debug_printf("Registering Cluster %d\n", fileCluster[i]);
                mcio_mcClose(fh);
            }
        } else {
            #ifdef USE_INJECT_LOGIC
            debug_printf("Writing history to %s\n", filename);
            int flag = sceMcFileAttrWriteable | sceMcFileCreateFile;
            int fh = mcio_mcOpen(filename, flag);
            if (fh >= 0)
            {
                mcio_mcWrite(fh, (void*)buff, HISTORY_FILE_SIZE);
                mcio_mcClose(fh);
            } else {
                debug_printf("File handle is %d\n", fh);
            }
            #endif
            memset(slotCount[i], 0x00, HISTORY_ENTRY_COUNT);
        
        }
    }
}

void ps2_history_tracker_init() {
    mcOps.page_erase = &page_erase;
    mcOps.page_read = &page_read;
    mcOps.page_write = &page_write;
    
    cardspecs.pagesize = 512;
    cardspecs.blocksize = 16;
    cardspecs.cardsize = ps2_cardman_get_card_size() ;
    cardspecs.flags = 0x08 | 0x10;

    mcfat_setConfig(mcOps, cardspecs);
}

void ps2_history_tracker_run() {
    static bool prevDirty = false;
    absolute_time_t time = get_absolute_time();
    uint64_t micros = to_us_since_boot(time);
    if (ps2_dirty_activity) {
        prevDirty = true;
        lastWrite = micros;
    } else if (prevDirty && ((micros - lastWrite)  > HISTORY_WRITE_HYST_US)) {
        // If Writing to MC has just finished...
        uint8_t buff[HISTORY_FILE_SIZE] = {0x00};
        char filename[23] = {0x00};
        char dirname[15] = {0x00};        

        for (int i = 0; i < HISTORY_NUMBER_OF_REGIONS; i++) {
            uint8_t slots_new[21] = {};
            // Read current history file for each region
            memset((void*)buff, 0x00, HISTORY_FILE_SIZE);
            snprintf(dirname, 15, SYSTEMDATA_DIRNAME, regionList[i]);
            snprintf(filename, 23, HISTORY_FILENAME_FORMAT, regionList[i]);
            if (refreshRequired[i] && dirExists(dirname) && fileExists(filename)) {
                int fh = mcio_mcOpen(filename, sceMcFileAttrReadable);

                debug_printf("Updating filename %s, fd %d\n", filename, fh);
                if (fh >= 0) {
                    mcio_mcRead(fh, buff, HISTORY_FILE_SIZE);
                    readSlots(buff, slots_new);
                    for (int j = 0; j < HISTORY_ENTRY_COUNT; j++) {
                        if (slots_new[j] != slotCount[i][j]) {
                            char sanitized_game_id[11] = {0};
                            game_names_extract_title_id(&buff[j * HISTORY_ENTRY_SIZE], sanitized_game_id, 16, sizeof(sanitized_game_id));
                            debug_printf("Game ID: %s\n", sanitized_game_id);
                            if (game_names_sanity_check_title_id(sanitized_game_id)) {
                                ps2_sd2psxman_set_gameid(sanitized_game_id);
                                sd2psxman_cmd = SD2PSXMAN_SET_GAMEID;
                            }
                        }
                    }
                    mcio_mcClose(fh);
                    memcpy((void*)slotCount[i], (void*)slots_new, HISTORY_ENTRY_COUNT);
                } else {
                    debug_printf("File exists, but handle returned %d\n", fh);
                }
            } else {
                #ifdef USE_INJECT_LOGIC
                debug_printf("Writing history to %s\n", filename);
                int flag = sceMcFileAttrWriteable | sceMcFileCreateFile;
                int dirsts = mcio_mcMkDir(dirname);
                int fh = mcio_mcOpen(filename, flag);
                if (fh >= 0)
                {
                    mcio_mcWrite(fh, (void*)buff, HISTORY_FILE_SIZE);
                    mcio_mcClose(fh);
                } else {
                    debug_printf("File handle is %d, dir status is %d\n", fh, dirsts);
                }
                #endif
            }
            refreshRequired[i] = false;
        }
        prevDirty = false;
    }
}