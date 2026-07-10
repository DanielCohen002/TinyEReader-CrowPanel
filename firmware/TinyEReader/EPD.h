#ifndef _EPD_H_
#define _EPD_H_

#include "EPD_Init.h"

/* Function declarations */
void EPD_DrawPoint(uint16_t x, uint16_t y, uint8_t color);
void EPD_DrawLine(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color);                              // draw a line
void EPD_DrawRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color);                         // draw a hollow rectangle
void Draw_Circle(int xc, int yc, int x, int y, uint8_t color);                                                     // 8-way circle plot (internal use)
void EPD_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint8_t color, uint16_t mode);                           // draw a circle
void EPD_DrawTriangel(uint16_t x, uint16_t y, uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color);  // draw a hollow triangle
void EPD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint8_t color, uint8_t sizey);                              // draw a single character
void EPD_ShowString(uint16_t x, uint16_t y, const char *s, uint8_t color, uint16_t sizey);                         // draw a string
uint32_t mypow(uint8_t m, uint8_t n);                                                                              // integer power (internal use)
void EPD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t color, uint8_t sizey);                 // draw a number
void EPD_ShowFloatNum(uint16_t x, uint16_t y, float num, uint8_t pre, uint8_t len, uint8_t color, uint8_t sizey);  // draw a float
void EPD_ShowPicture(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint8_t pic[], uint8_t color); // draw a bitmap
void EPD_ShowWatch(uint16_t x, uint16_t y, float num, uint8_t pre, uint8_t len, uint8_t color, uint8_t sizey);     // draw a stopwatch-style HH:MM readout
#endif
