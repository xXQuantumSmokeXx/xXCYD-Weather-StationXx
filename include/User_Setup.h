// TFT_eSPI User_Setup for CYD (ESP32-2432S028 / cheap yellow display)
// Loaded when USER_SETUP_LOADED=1 is defined in build_flags.
//
// All defines use #ifndef so PlatformIO build_flags (-D) override
// individual pins without needing USER_SETUP_LOADED=0.  Keep the
// defaults here for quick edits; override from platformio.ini for
// board variants (e.g. TFT_RST=4 on some 2USB boards).

#ifndef ILI9341_DRIVER
#define ILI9341_DRIVER
#endif

#ifndef TFT_WIDTH
#define TFT_WIDTH  240
#endif

#ifndef TFT_HEIGHT
#define TFT_HEIGHT 320
#endif

#ifndef TFT_MOSI
#define TFT_MOSI 13
#endif

#ifndef TFT_MISO
#define TFT_MISO 12
#endif

#ifndef TFT_SCLK
#define TFT_SCLK 14
#endif

#ifndef TFT_CS
#define TFT_CS   15
#endif

#ifndef TFT_DC
#define TFT_DC    2
#endif

#ifndef TFT_RST
#define TFT_RST  -1
#endif

#ifndef TFT_BL
#define TFT_BL   21
#endif

#ifndef TFT_BACKLIGHT_ON
#define TFT_BACKLIGHT_ON HIGH
#endif

#ifndef LOAD_GLCD
#define LOAD_GLCD
#endif

#ifndef LOAD_FONT2
#define LOAD_FONT2
#endif

#ifndef LOAD_FONT4
#define LOAD_FONT4
#endif

#ifndef LOAD_FONT6
#define LOAD_FONT6
#endif

#ifndef LOAD_FONT7
#define LOAD_FONT7
#endif

#ifndef LOAD_FONT8
#define LOAD_FONT8
#endif

#ifndef SMOOTH_FONT
#define SMOOTH_FONT
#endif

#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY       27000000
#endif

#ifndef SPI_READ_FREQUENCY
#define SPI_READ_FREQUENCY  20000000
#endif

#ifndef SPI_TOUCH_FREQUENCY
#define SPI_TOUCH_FREQUENCY  2500000
#endif
