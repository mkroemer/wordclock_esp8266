#ifndef CONFIG_H
#define CONFIG_H

// credentials for Access Point
#define AP_SSID "WordclockAP"

// Enable automatic restarting of the ESP to prevent overflowing the millis function
#define AUTO_RESTART_ENABLED true
// millis() overflows at    4294967295
#define AUTO_RESTART_MILLIS 3000000000
// At which hour of the day the automatic should be triggered
#define AUTO_RESTART_HOUR 4
// 2x times pressing reset button will start AP for 60 seconds
#define DBD_CONFIG_PORTAL_TIMEOUT 60


// width of the grid in characters
#define GRID_WIDTH 11
// height of the gird in characters
#define GRID_HEIGHT 10

#define ORIENTATION 1

#define NEOPIXELPIN 13       // pin to which the NeoPixels are attached
#define NEOPIXEL_MATRIX_TYPE (NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG)
#define NEOPIXEL_LED_TYPE (NEO_GRB + NEO_KHZ800)
#define NUMPIXELS 113       // number of pixels attached to Attiny85
#define BUTTONPIN 14        // pin to which the button is attached
#define LEFT 1
#define RIGHT 2

#define PERIOD_HEARTBEAT 1000
#define PERIOD_ANIMATION 200
#define PERIOD_TETRIS 50
#define PERIOD_SNAKE 50
#define PERIOD_PONG 10
#define TIMEOUT_LEDDIRECT 5000
#define PERIOD_STATECHANGE 10000
#define PERIOD_NTPUPDATE 30000
#define PERIOD_TIMEVISUUPDATE 1000
#define PERIOD_MATRIXUPDATE 100
#define PERIOD_NIGHTMODECHECK 20000

#define SHORTPRESS 100
#define LONGPRESS 2000

#define CURRENT_LIMIT_LED 2500 // limit the total current sonsumed by LEDs (mA)

#define DEFAULT_SMOOTHING_FACTOR 0.5



// Number of seconds after reset during which a 
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

#define ESP_DRD_USE_LITTLEFS true

#endif
