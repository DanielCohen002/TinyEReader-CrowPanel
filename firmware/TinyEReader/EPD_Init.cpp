#include "EPD_Init.h"

/**
   @brief       Poll the EPD busy line
   @param       none
   @retval      none
   @note        BUSY high = busy, BUSY low = idle
*/
void EPD_READBUSY(void)
{
  while (1)
  {
    if (EPD_ReadBUSY == 0)
    {
      break;
    }
  }
  delayMicroseconds(100);
}

/**
   @brief       Hardware + software reset of the EPD
   @param       none
   @retval      none
   @note        A hardware reset is required when waking from sleep
*/
void EPD_HW_SW_RESET(void)
{
  delay(100);
  EPD_RES_Set();
  delay(10);
  EPD_RES_Clr();
  delay(10);
  EPD_RES_Set();
  delay(10);
  EPD_READBUSY();   /* wait for the hardware reset to finish */
  EPD_WR_REG(0x12); /* software reset command, see SSD1680 datasheet */
  EPD_READBUSY();   /* wait for the software reset to finish */
}

/**
   @brief       Put the EPD into sleep mode
   @param       none
   @retval      none
   @note        Power draw varies by sleep mode; default here is mode 1, which retains RAM contents
*/
void EPD_Sleep(void)
{
  EPD_WR_REG(0x10); /* sleep command, see SSD1680 datasheet */
  EPD_WR_DATA8(0x01);

  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x01);
  delay(20);
}

/**
****************************************************************************
  Quick reference for register 0x22 (Display Update Control 2) to make its
  bitfields easier to follow.
  Bits A7 A6 A5 A4 A3 A2 A1 A0 do the following:
  A7 Enable internal clock oscillator
  A6 Turn-on DC-DC boost
  A5 Read the temperature sensor
  A4 Search and load the LUT from OTP
  A3 Perform 0=DISPLAY Mode 1 1=DISPLAY Mode 2
  A2 Perform image display (content in RAM) sequence
  A1 Turn-off DC-DC boost
  A0 Disable the internal clock oscillator
  On the temperature sensor: the EPD loads its LUT based on the built-in
  temperature sensor reading, which is how different display modes (like the
  fast-refresh mode in this project) apply different waveforms for a given
  temperature coefficient.
  On display modes: Mode 1 and Mode 2 roughly correspond to full refresh and
  partial refresh respectively.
****************************************************************************
*/

/**
   @brief       Trigger a full-refresh display update
   @param       none
   @retval      none
*/
void EPD_Update(void)
{
  EPD_WR_REG(0x22); /* display update control command, see SSD1680 datasheet */
  /* Before updating: enable clock, enable DC-DC, read ambient temperature,
     load LUT, run in full-refresh mode, execute the image refresh, and keep
     DC-DC/clock enabled afterward. */
  EPD_WR_DATA8(0xF4);
  EPD_WR_REG(0x20); /* activate command, see SSD1680 datasheet */
  EPD_READBUSY();
}

//Fast refresh 1 update function
void EPD_Update_Fast(void)
{
  EPD_WR_REG(0x22); //Display Update Control
  EPD_WR_DATA8(0xC7);
  EPD_WR_REG(0x20); //Activate Display Update Sequence
  EPD_READBUSY();
}

/**
   @brief       Trigger a partial-refresh display update
   @param       none
   @retval      none
*/
void EPD_PartUpdate(void)
{
  EPD_WR_REG(0x22);  /* display update control command, see SSD1680 datasheet */
  /* If the clock and DC-DC were already enabled before this partial update
     and haven't been turned off, there's no need to re-enable them here --
     otherwise they must be enabled. */
  /* Enable clock, enable DC-DC, read ambient temperature, load LUT, run in
     partial-refresh mode, execute the image refresh, keep DC-DC/clock on. */
  EPD_WR_DATA8(0xFC); /* Defaults to the "enable" configuration so callers
                          unfamiliar with the flow don't have to think about
                          it; if clock/DC-DC are already on this can be
                          changed to 0x1C instead. */
  EPD_WR_REG(0x20);
  EPD_READBUSY();

  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x01);
}

/**
************************************************************************
  Quick reference for register 0x26 (the "old data" RAM used for partial
  refresh differencing).
  The SSD1680 can drive either black/white or black/white/red panels.
  For black/white panels, register 0x26 holds the previous frame's data --
  clearing it is required for partial refresh to look right.
  For black/white/red panels, register 0x26 instead holds the red image
  data.
************************************************************************
*/
/**
   @brief       Clear register 0x26 (the previous-frame buffer)
   @param       none
   @retval      none
*/
void EPD_Clear_R26H(void)
{
  uint32_t i;
  EPD_WR_REG(0x26); /* write-to-RAM command, see SSD1680 datasheet */
  for (i = 0; i < ALLSCREEN_BYTES; i++)
  {
    EPD_WR_DATA8(WHITE);
  }
  EPD_READBUSY();
}

/**
   @brief       Fill the whole screen with one color
   @param       color: fill color value
   @retval      none
*/
void EPD_ALL_Fill(uint8_t color)
{
  uint32_t i;
  EPD_WR_REG(0x3C);/* border waveform control command, see SSD1680 datasheet */
  if (color)
  {
    EPD_WR_DATA8(0x01);
  }
  else
  {
    EPD_WR_DATA8(0x00);
  }
  EPD_WR_REG(0x24); /* write-to-RAM command, see SSD1680 datasheet */
  for (i = 0; i < ALLSCREEN_BYTES; i++)
  {
    EPD_WR_DATA8(color);
  }
  EPD_READBUSY();
}

/**
   @brief       Push a full-screen image to the panel
   @param       ImageBW: image buffer to display
   @retval      none
*/
void EPD_DisplayImage(const uint8_t *ImageBW)
{
  uint32_t i;
  EPD_WR_REG(0x3C);
  EPD_WR_DATA8(0x01);
  EPD_WR_REG(0x24);
  for (i = 0; i < ALLSCREEN_BYTES; i++)
  {
    EPD_WR_DATA8(~ImageBW[i]);
  }
}

/**
   @brief       Copy the given image into register 0x26, the "previous frame"
                reference the panel diffs against on the next partial update
   @param       ImageBW: image buffer that was just displayed
   @retval      none
   @note        Call this right after EPD_PartUpdate() so R26H always matches
                what's actually on screen. Without it, partial updates diff
                against a stale reference and old/new content visibly blend
                together instead of cleanly replacing each other.
*/
void EPD_SyncOldData(const uint8_t *ImageBW)
{
  uint32_t i;
  EPD_WR_REG(0x26);
  for (i = 0; i < ALLSCREEN_BYTES; i++)
  {
    EPD_WR_DATA8(~ImageBW[i]);
  }
}

void EPD_Init(void)
{
  EPD_GPIOInit();
  EPD_HW_SW_RESET();

  EPD_WR_REG(0x01);    /* configure driver output control */
  EPD_WR_DATA8(0xF9); /* configure MUX line setting */
  EPD_WR_DATA8(0x00); /* configure MUX line setting */
  EPD_WR_DATA8(0x00); /* configure EPD scan direction */

  EPD_WR_REG(0x11); /* configure data entry mode / RAM write direction (row-by-row) */
  EPD_WR_DATA8(0x03);

  EPD_WR_REG(0x44); /* configure RAM X start/end address */
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x0F);

  EPD_WR_REG(0x45);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0xF9);
  EPD_WR_DATA8(0x00);

  EPD_WR_REG(0x3C);   /* configure border waveform color */
  EPD_WR_DATA8(0x01); // 0x01->0x05
  EPD_READBUSY();

  EPD_WR_REG(0x18); /* select the internal temperature sensor */
  EPD_WR_DATA8(0x80);

  //    EPD_WR_REG(0x22);
  //    EPD_WR_DATA8(0xF4);
  //    EPD_WR_REG(0x20);
  //    EPD_READBUSY();

  EPD_WR_REG(0x4E);
  EPD_WR_DATA8(0x00);
  EPD_WR_REG(0x4F);
  EPD_WR_DATA8(0x00);
  EPD_WR_DATA8(0x00);

  //    EPD_WR_REG(0x0C);            /* configure booster startup time */

  EPD_READBUSY();

//  EPD_WR_REG(0x3C);
//  EPD_WR_DATA8(0x3);
//  EPD_READBUSY();
}

//Fast refresh 1 initialization
void EPD_HW_Init_Fast(void)
{
  EPD_RES_Clr();  // Module reset
  delay(10);//At least 10ms delay
  EPD_RES_Set();
  delay(10); //At least 10ms delay

  EPD_WR_REG(0x12);  //SWRESET
  EPD_READBUSY();

  EPD_WR_REG(0x18); //Read built-in temperature sensor
  EPD_WR_DATA8(0x80);

  EPD_WR_REG(0x22); // Load temperature value
  EPD_WR_DATA8(0xB1);
  EPD_WR_REG(0x20);
  EPD_READBUSY();

  EPD_WR_REG(0x1A); // Write to temperature register
  EPD_WR_DATA8(0x64);
  EPD_WR_DATA8(0x00);

  EPD_WR_REG(0x22); // Load temperature value
  EPD_WR_DATA8(0x91);
  EPD_WR_REG(0x20);
  EPD_READBUSY();
}
