#include "EPD.h"
#include "EPDfont.h"
#include "string.h"

uint8_t ImageBW[ALLSCREEN_BYTES];

/**
 * @brief       Draw a single pixel
 * @param       x: pixel column
 * @param       y: pixel row
 * @param       color: pixel color
 * @retval      none
 */
void EPD_DrawPoint(uint16_t x, uint16_t y, uint8_t color)
{
    uint8_t dat = 0;
    uint16_t xpoint, ypoint;
    uint32_t Addr;
    switch (USE_HORIZONTIAL)
    {
    case 0:
        xpoint = EPD_H - y - 1;
        ypoint = x;
        break;
    case 1:
        xpoint = x;
        ypoint = y;
        break;
    case 2:
        xpoint = y;
        ypoint = EPD_W - x - 1;
        break;
    case 3:
        xpoint = EPD_W - x - 1;
        ypoint = EPD_H - y - 1;
        break;
    default:
        return;
    }
#if USE_HORIZONTIAL == 0 | USE_HORIZONTIAL == 2
    Addr = xpoint / 8 + ypoint * ((EPD_H % 8 == 0) ? (EPD_H / 8) : (EPD_H / 8 + 1));
#else
    Addr = xpoint / 8 + ypoint * ((EPD_W % 8 == 0) ? (EPD_W / 8) : (EPD_W / 8 + 1));
#endif
    dat = ImageBW[Addr];
    if (color == BLACK)
    {
        ImageBW[Addr] = dat | (0x80 >> (xpoint % 8));
    }
    else
    {
        ImageBW[Addr] = dat & ~(0x80 >> (xpoint % 8));
    }
}

/**
 * @brief       Draw a line between two points
 * @param       xs: start column
 * @param       ys: start row
 * @param       xe: end column
 * @param       ye: end row
 * @param       color: line color
 * @retval      none
 */
void EPD_DrawLine(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color)
{
    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, uRow, uCol;
    delta_x = xe - xs; // coordinate deltas
    delta_y = ye - ys;
    uRow = xs;
    uCol = ys;
    if (delta_x > 0)
    {
        incx = 1; // step direction
    }
    else if (delta_x == 0)
    {
        incx = 0; // vertical line
    }
    else
    {
        incx = -1;
        delta_x = -delta_x;
    }
    if (delta_y > 0)
    {
        incy = 1;
    }
    else if (delta_y == 0)
    {
        incy = 0; // horizontal line
    }
    else
    {
        incy = -1;
        delta_y = -delta_y;
    }
    if (delta_x > delta_y)
    {
        distance = delta_x; // pick the dominant axis
    }
    else
    {
        distance = delta_y;
    }
    for (t = 0; t <= distance + 1; t++) // step along the line
    {
        EPD_DrawPoint(uRow, uCol, color); // plot the point
        xerr += delta_x;
        yerr += delta_y;
        if (xerr > distance)
        {
            xerr -= distance;
            uRow += incx;
        }
        if (yerr > distance)
        {
            yerr -= distance;
            uCol += incy;
        }
    }
}

/**
 * @brief       Draw a hollow rectangle
 * @param       xs: start column
 * @param       ys: start row
 * @param       xe: end column
 * @param       ye: end row
 * @param       color: rectangle color
 * @retval      none
 */
void EPD_DrawRectangle(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color)
{
    EPD_DrawLine(xs, ys, xe, ys, color);
    EPD_DrawLine(xs, ys, xs, ye, color);
    EPD_DrawLine(xs, ye, xe, ye, color);
    EPD_DrawLine(xe, ys, xe, ye, color);
}

/**
 * @brief       8-way circle plot (internal use)
 * @param       xc: circle center column
 * @param       yc: circle center row
 * @param       x: column relative to center
 * @param       y: row relative to center
 * @param       color: circle color
 * @retval      none
 */
void Draw_Circle(int xc, int yc, int x, int y, uint8_t color)
{
    EPD_DrawPoint(xc + x, yc + y, color);
    EPD_DrawPoint(xc - x, yc + y, color);
    EPD_DrawPoint(xc + x, yc - y, color);
    EPD_DrawPoint(xc - x, yc - y, color);
    EPD_DrawPoint(xc + y, yc + x, color);
    EPD_DrawPoint(xc - y, yc + x, color);
    EPD_DrawPoint(xc + y, yc - x, color);
    EPD_DrawPoint(xc - y, yc - x, color);
}

/**
 * @brief       Draw a circle
 * @param       xc: circle center column
 * @param       yc: circle center row
 * @param       r: radius
 * @param       color: circle color
 * @param       mode: whether to fill the circle
 * @retval      none
 */
void EPD_DrawCircle(uint16_t xc, uint16_t yc, uint16_t r, uint8_t color, uint16_t mode)
{
    int x = 0, y = r, yi, d;
    d = 3 - 2 * r;
    /* filled circle */
    if (mode)
    {
        while (x <= y)
        {
            for (yi = x; yi <= y; yi++)
            {
                Draw_Circle(xc, yc, x, yi, color);
            }
            if (d < 0)
            {
                d = d + 4 * x + 6;
            }
            else
            {
                d = d + 4 * (x - y) + 10;
                y--;
            }
            x++;
        }
    }
    /* hollow circle */
    else
    {
        while (x <= y)
        {
            Draw_Circle(xc, yc, x, y, color);
            if (d < 0)
            {
                d = d + 4 * x + 6;
            }
            else
            {
                d = d + 4 * (x - y) + 10;
                y--;
            }
            x++;
        }
    }
}

/**
 * @brief       Draw a hollow triangle
 * @param       x: first vertex column
 * @param       y: first vertex row
 * @param       xs: second vertex column
 * @param       ys: second vertex row
 * @param       xe: third vertex column
 * @param       ye: third vertex row
 * @param       color: triangle color
 * @retval      none
 */
void EPD_DrawTriangel(uint16_t x, uint16_t y, uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint8_t color)
{
    EPD_DrawLine(x, y, xs, ys, color);
    EPD_DrawLine(xs, ys, xe, ye, color);
    EPD_DrawLine(xe, ye, x, y, color);
}

/**
 * @brief       Draw a single character
 * @param       x: column to draw at
 * @param       y: row to draw at
 * @param       num: ASCII code of the character
 * @param       color: character color
 * @param       sizey: font size (pixel height)
 * @retval      none
 */
void EPD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint8_t color, uint8_t sizey)
{
    uint8_t temp, sizex, t;
    uint16_t i, TypefaceNum; // bytes occupied by one character's glyph
    uint16_t x0 = x;
    sizex = sizey / 2;
    TypefaceNum = (sizex / 8 + ((sizex % 8) ? 1 : 0)) * sizey;
    num = num - ' '; // offset into the font table
    for (i = 0; i < TypefaceNum; i++)
    {
        if (sizey == 12)
            temp = ascii_1206[num][i]; // 6x12 font
        else if (sizey == 16)
            temp = ascii_1608[num][i]; // 8x16 font
        else if (sizey == 24)
            temp = ascii_2412[num][i]; // 12x24 font
        else if (sizey == 32)
            temp = ascii_3216[num][i]; // 16x32 font
        else if (sizey == 48)
            temp = ascii_4824[num][i]; // 24x48 font
        else
            return;
        for (t = 0; t < 8; t++)
        {
            if (temp & (0x01 << t))
            {
                EPD_DrawPoint(x, y, color); // foreground pixel
            }
            else
            {
                EPD_DrawPoint(x, y, !color);
            }
            x++;
            if ((x - x0) == sizex)
            {
                x = x0;
                y++;
                break;
            }
        }
    }
}

/**
 * @brief       Draw a string
 * @param       x: column to start at
 * @param       y: row to start at
 * @param       *s: string to draw
 * @param       color: character color
 * @param       sizey: font size (pixel height)
 * @retval      none
 */
void EPD_ShowString(uint16_t x, uint16_t y, const char *s, uint8_t color, uint16_t sizey)
{
    while ((*s <= '~') && (*s >= ' ')) // stop at non-printable characters
    {
        if (x > (EPD_W - 1) || y > (EPD_H - 1))
            return;
        EPD_ShowChar(x, y, *s, color, sizey);
        x += sizey / 2;
        s++;
    }
}

/**
 * @brief       Integer power (internal use)
 * @param       m: base
 * @param       n: exponent
 * @retval      result: m raised to the n
 */
uint32_t mypow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;
    while (n--)
    {
        result *= m;
    }
    return result;
}

/**
 * @brief       Draw a number
 * @param       x: column to start at
 * @param       y: row to start at
 * @param       num: number to draw (0~4294967295)
 * @param       len: number of digits to draw
 * @param       color: character color
 * @param       sizey: font size (pixel height)
 * @retval      none
 */
void EPD_ShowNum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t color, uint8_t sizey)
{
    uint8_t t, temp, enshow = 0;
    uint8_t sizex = sizey / 2;
    for (t = 0; t < len; t++)
    {
        temp = (num / mypow(10, len - t - 1)) % 10;
        if (enshow == 0 && t < (len - 1))
        {
            if (temp == 0)
            {
                EPD_ShowChar(x + t * sizex, y, ' ', color, sizey);
                continue;
            }
            else
            {
                enshow = 1;
            }
        }
        EPD_ShowChar(x + t * sizex, y, temp + '0', color, sizey);
    }
}

/**
 * @brief       Draw a floating point number
 * @param       x: column to start at
 * @param       y: row to start at
 * @param       num: value to draw
 * @param       pre: decimal precision to draw
 * @param       len: total digits to draw (excluding the decimal point)
 * @param       color: character color
 * @param       sizey: font size (pixel height)
 * @retval      none
 */
void EPD_ShowFloatNum(uint16_t x, uint16_t y, float num, uint8_t pre, uint8_t len, uint8_t color, uint8_t sizey)
{
    uint32_t i, temp, num1;
    uint8_t sizex = sizey / 2;
    num1 = num * mypow(10, pre);
    for (i = 0; i < len; i++)
    {
        temp = (num1 / mypow(10, len - i - 1)) % 10;
        if (i == (len - pre))
        {
            EPD_ShowChar(x + (len - pre) * sizex, y, '.', color, sizey);
            i++;
            len += 1;
        }
        EPD_ShowChar(x + i * sizex, y, temp + '0', color, sizey);
    }
}


/**
 * @brief       Draw a 1-bit bitmap
 * @param       x: column to start at
 * @param       y: row to start at
 * @param       width: bitmap width
 * @param       height: bitmap height
 * @param       pic: bitmap data
 * @param       color: bitmap color
 * @note        Bitmap width must be 248px or less
 * @retval      none
 */
void EPD_ShowPicture(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint8_t pic[], uint8_t color)
{
    uint8_t t, temp;
    uint16_t x0 = x;
    uint32_t i, TypefaceNum; // bytes occupied by one bitmap
    TypefaceNum = ((width % 8) ? (width / 8 + 1) : (width / 8)) * height;
    for (i = 0; i < TypefaceNum; i++)
    {
        temp = pic[i];
        for (t = 0; t < 8; t++)
        {
            if (temp & 0x80)
            {
                EPD_DrawPoint(x, y, color);
            }
            else
            {
                EPD_DrawPoint(x, y, !color);
            }
            x++;
            temp <<= 1;
        }
        if ((x - x0) == width)
        {
            x = x0;
            y++;
        }
    }
}

/**
 * @brief       Draw a stopwatch-style HH:MM/MM:SS readout using the character functions
 * @param       x: column to start at
 * @param       y: row to start at
 * @param       num: value to draw
 * @param       pre: decimal precision to draw
 * @param       len: total digits to draw (excluding the colon)
 * @param       color: character color
 * @param       sizey: font size (pixel height)
 * @retval      none
 */
void EPD_ShowWatch(uint16_t x, uint16_t y, float num, uint8_t pre, uint8_t len, uint8_t color, uint8_t sizey)
{
    uint8_t t, temp, sizex;
    uint16_t num1;
    sizex = sizey / 2;
    num1 = num * mypow(10, pre);
    for (t = 0; t < len; t++)
    {
        temp = (num1 / mypow(10, len - t - 1)) % 10;
        if (t == (len - pre))
        {
            EPD_ShowChar(x + (len - pre) * sizex + (sizex / 2 - 2), y - 6, ':', color, sizey);
            t++;
            len += 1;
        }
        EPD_ShowChar(x + t * sizex, y, temp + 48, color, sizey);
    }
}
