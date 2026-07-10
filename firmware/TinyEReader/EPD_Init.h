#ifndef _EPD_INIT_H_
#define _EPD_INIT_H_

#include "spi.h"

/* Display orientation */
#define USE_HORIZONTIAL 2 /* 0 1 2 3 */
#if USE_HORIZONTIAL == 0 | USE_HORIZONTIAL == 2
#define EPD_W 250
#define EPD_H 122
#define ALLSCREEN_BYTES ((EPD_H % 8) ? (EPD_H / 8 + 1) : (EPD_H / 8)) * EPD_W
#else
#define EPD_W 122
#define EPD_H 250
#define ALLSCREEN_BYTES ((EPD_W % 8) ? (EPD_W / 8 + 1) : (EPD_W / 8)) * EPD_H
#endif
/* Color constants */
/* R24H register: write 0 for a black pixel, 1 for a white pixel */
#define WHITE 0xFF
#define BLACK 0x00

/* Function declarations */
void EPD_READBUSY(void);
void EPD_HW_SW_RESET(void);
void EPD_Sleep(void);
void EPD_Update(void);
void EPD_Update_Fast(void);
void EPD_PartUpdate(void);
void EPD_Clear_R26H(void);
void EPD_ALL_Fill(uint8_t color);
void EPD_DisplayImage(const uint8_t *ImageBW);
void EPD_SyncOldData(const uint8_t *ImageBW);
void EPD_Init(void);
void EPD_HW_Init_Fast(void);
#endif
