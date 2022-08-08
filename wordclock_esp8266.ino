/**
   Wordclock 2.0 - Wordclock with ESP8266 and NTP time update

   created by techniccontroller 04.12.2021

*/

#include "secrets.h"
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>

// own libraries
#include "ntp_client_plus.h"
#include "ledmatrix.h"


// ----------------------------------------------------------------------------------
//                                        CONSTANTS
// ----------------------------------------------------------------------------------

#define EEPROM_SIZE 20      // size of EEPROM to save persistent variables
#define ADR_NM_START_H 0
#define ADR_NM_END_H 4
#define ADR_NM_START_M 8
#define ADR_NM_END_M 12
#define ADR_BRIGHTNESS 16


#define NEOPIXELPIN 12       // pin to which the NeoPixels are attached
#define NUMPIXELS 114       // number of pixels attached to Attiny85

#define PERIOD_HEARTBEAT 1000
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

// number of colors in colors array
#define NUM_COLORS 7

// width of the led matrix
#define WIDTH 11
// height of the led matrix
#define HEIGHT 10
//
#define ORIENTATION 1 // 1: row

// own datatype for state machine states
#define NUM_STATES 1
enum ClockState {st_clock};
const String stateNames[] = {"Clock"};
// PERIODS for each state (different for stateAutoChange or Manual mode)
const uint16_t PERIODS[2][NUM_STATES] = { {
    PERIOD_TIMEVISUUPDATE // stateAutoChange = 0
  },
  { PERIOD_TIMEVISUUPDATE // stateAutoChange = 1
  }
};

// ports
const unsigned int localPort = 2390;
const unsigned int HTTPPort = 80;
//const unsigned int logMulticastPort = 8123;
const unsigned int DNSPort = 53;

// ip addresses for multicast logging
// IPAddress logMulticastIP = IPAddress(230, 120, 10, 2);

// ip addresses for Access Point
IPAddress IPAdress_AccessPoint(192, 168, 10, 2);
IPAddress Gateway_AccessPoint(192, 168, 10, 0);
IPAddress Subnetmask_AccessPoint(255, 255, 255, 0);

// hostname
const String hostname = "wordclock";

// URL DNS server
const char WebserverURL[] = "www.wordclock.local";

// ----------------------------------------------------------------------------------
//                                        GLOBAL VARIABLES
// ----------------------------------------------------------------------------------

// Webserver
WebServer server(HTTPPort);

//DNS Server
DNSServer DnsServer;

// When we setup the NeoPixel library, we tell it how many pixels, and which pin to use to send signals.
// Note that for older NeoPixel strips you might need to change the third parameter--see the strandtest
// example for more information on possible values.
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(WIDTH, HEIGHT + 1, NEOPIXELPIN,
                            NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
                            NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
                            NEO_GRB            + NEO_KHZ800);


// seven predefined colors24bit (green, red, yellow, purple, orange, lightgreen, blue)
const uint32_t colors24bit[NUM_COLORS] = {
  LEDMatrix::Color24bit(0, 255, 0),
  LEDMatrix::Color24bit(255, 0, 0),
  LEDMatrix::Color24bit(200, 200, 0),
  LEDMatrix::Color24bit(255, 0, 200),
  LEDMatrix::Color24bit(255, 128, 0),
  LEDMatrix::Color24bit(0, 128, 0),
  LEDMatrix::Color24bit(0, 0, 255)
};

uint8_t brightness = 40;            // current brightness of leds
bool sprialDir = false;

// timestamp variables
long lastheartbeat = millis();      // time of last heartbeat sending
long lastStep = millis();           // time of last animation step
long lastLEDdirect = 0;             // time of last direct LED command (=> fall back to normal mode after timeout)
long lastStateChange = millis();    // time of last state change
long lastNTPUpdate = millis();      // time of last NTP update
long lastAnimationStep = millis();  // time of last Matrix update
long lastNightmodeCheck = millis(); // time of last nightmode check

// Create necessary global objects
WiFiUDP NTPUDP;
NTPClientPlus ntp = NTPClientPlus(NTPUDP, "pool.ntp.org", 1, true);
LEDMatrix ledmatrix = LEDMatrix(&matrix, brightness);

float filterFactor = DEFAULT_SMOOTHING_FACTOR;// stores smoothing factor for led transition
uint8_t currentState = st_clock;              // stores current state
bool stateAutoChange = false;                 // stores state of automatic state change
bool nightMode = false;                       // stores state of nightmode
uint32_t maincolor_clock = colors24bit[2];    // color of the clock and digital clock
bool apmode = false;                          // stores if WiFi AP mode is active

// nightmode settings
int nightModeStartHour = 22;
int nightModeStartMin = 0;
int nightModeEndHour = 7;
int nightModeEndMin = 0;

// ----------------------------------------------------------------------------------
//                                        SETUP
// ----------------------------------------------------------------------------------

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.printf("\nSketchname: %s\nBuild: %s\n", (__FILE__), (__TIMESTAMP__));
  Serial.println();

  //Init EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // setup Matrix LED functions
  ledmatrix.setupMatrix();
  ledmatrix.setCurrentLimit(CURRENT_LIMIT_LED);

  // Turn on minutes leds (blue)
  ledmatrix.setMinIndicator(15, colors24bit[6]);
  ledmatrix.drawOnMatrixInstant();

  /** Use WiFiMaanger for handling initial Wifi setup **/

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // Uncomment and run it once, if you want to erase all the stored information
  // wifiManager.resetSettings();

  // set custom ip for portal
  //wifiManager.setAPStaticIPConfig(IPAdress_AccessPoint, Gateway_AccessPoint, Subnetmask_AccessPoint);

  // fetches ssid and pass from eeprom and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here "wordclockAP"
  // and goes into a blocking loop awaiting configuration
  wifiManager.autoConnect(AP_SSID);

  // if you get here you have connected to the WiFi
  ("Connected.");
  ("IP address: ");
  (WiFi.localIP());

  // Turn off minutes leds
  ledmatrix.setMinIndicator(15, 0);
  ledmatrix.drawOnMatrixInstant();
  
  // setup OTA
  setupOTA(hostname);

  server.on("/cmd", handleCommand); // process commands
  server.on("/data", handleDataRequest); // process datarequests
  server.begin();

  Serial.println("Wordclock");
  Serial.println("Start program\n");
  delay(10);
  Serial.println("Sketchname: " + String(__FILE__));
  delay(10);
  Serial.println("Build: " + String(__TIMESTAMP__));
  delay(10);
  Serial.println("IP: " + WiFi.localIP().toString());

  for (int r = 0; r < HEIGHT; r++) {
    for (int c = 0; c < WIDTH; c++) {
      matrix.fillScreen(0);
      matrix.drawPixel(c, r, LEDMatrix::color24to16bit(colors24bit[2]));
      matrix.show();
      delay(10);
    }
  }

  // clear Matrix
  matrix.fillScreen(0);
  matrix.show();
  delay(200);

  // display IP
  uint8_t address = WiFi.localIP()[3];
  ledmatrix.printChar(1, 0, 'I', maincolor_clock);
  ledmatrix.printChar(5, 0, 'P', maincolor_clock);
  ledmatrix.printNumber(0, 5, (address / 100), maincolor_clock);
  ledmatrix.printNumber(4, 5, (address / 10) % 10, maincolor_clock);
  ledmatrix.printNumber(8, 5, address % 10, maincolor_clock);
  ledmatrix.drawOnMatrixInstant();
  delay(2000);

  // clear matrix
  ledmatrix.gridFlush();
  ledmatrix.drawOnMatrixInstant();

  // setup NTP
  ntp.setupNTPClient();
  Serial.println("NTP running");
  Serial.println("Time: " +  ntp.getFormattedTime());
  Serial.println("TimeOffset (seconds): " + String(ntp.getTimeOffset()));

  // show the current time for short time in words
  int hours = ntp.getHours24();
  int minutes = ntp.getMinutes();
  String timeMessage = timeToString(hours, minutes);
  showStringOnClock(timeMessage, maincolor_clock);
  drawMinuteIndicator(minutes, maincolor_clock);
  ledmatrix.drawOnMatrixSmooth(filterFactor);
  delay(1000);

  // Read nightmode setting from EEPROM
  nightModeStartHour = readIntEEPROM(ADR_NM_START_H);
  nightModeStartMin = readIntEEPROM(ADR_NM_START_M);
  nightModeEndHour = readIntEEPROM(ADR_NM_END_H);
  nightModeEndMin = readIntEEPROM(ADR_NM_END_M);
  Serial.println("Nightmode starts at: " + String(nightModeStartHour) + ":" + String(nightModeStartMin));
  Serial.println("Nightmode ends at: " + String(nightModeEndHour) + ":" + String(nightModeEndMin));

  // Read brightness setting from EEPROM
  brightness = readIntEEPROM(ADR_BRIGHTNESS);
  Serial.println("Brightness: " + String(brightness));
  ledmatrix.setBrightness(brightness);
}


// ----------------------------------------------------------------------------------
//                                        LOOP
// ----------------------------------------------------------------------------------

void loop() {
  // handle OTA
  handleOTA();

  // handle Webserver
  server.handleClient();

  if (millis() - lastheartbeat > PERIOD_HEARTBEAT) {
    // Check wifi status (only if no apmode)
    if (!apmode && WiFi.status() != WL_CONNECTED) {
      ("connection lost");
      ledmatrix.gridAddPixel(0, 5, colors24bit[1]);
      ledmatrix.drawOnMatrixInstant();
    }
    lastheartbeat = millis();
  }

  // handle mode behaviours (trigger loopCycles of different modes depending on current mode)
  if (!nightMode && (millis() - lastStep > PERIODS[stateAutoChange][currentState])) {
    int hours = ntp.getHours24();
    int minutes = ntp.getMinutes();
    showStringOnClock(timeToString(hours, minutes), maincolor_clock);
    drawMinuteIndicator(minutes, maincolor_clock);
  }
  lastStep = millis();
}

// periodically write colors to matrix
if (!nightMode && (millis() - lastAnimationStep > PERIOD_MATRIXUPDATE)) {
  ledmatrix.drawOnMatrixSmooth(filterFactor);
  lastAnimationStep = millis();
}

// NTP time update
if (millis() - lastNTPUpdate > PERIOD_NTPUPDATE) {
  if (ntp.updateNTP()) {
    ntp.calcDate();
    Serial.println("NTP-Update successful");
    Serial.println("Time: " +  ntp.getFormattedTime());
    Serial.println("TimeOffset (seconds): " + String(ntp.getTimeOffset()));
    ("NTP-Update successful");
  }
  else {
    Serial.println("NTP-Update not successful");
    ("NTP-Update not successful");
  }
  lastNTPUpdate = millis();
}

// check if nightmode need to be activated
if (millis() - lastNightmodeCheck > PERIOD_NIGHTMODECHECK) {
  int hours = ntp.getHours24();
  int minutes = ntp.getMinutes();

  if (hours == nightModeStartHour && minutes == nightModeStartMin) {
    setNightmode(true);
  }
  else if (hours == nightModeEndHour && minutes == nightModeEndMin) {
    setNightmode(false);
  }

  lastNightmodeCheck = millis();
}

}


// ----------------------------------------------------------------------------------
//                                        OTHER FUNCTIONS
// ----------------------------------------------------------------------------------

/**
   @brief execute a state change to given newState

   @param newState the new state to be changed to
*/
void stateChange(uint8_t newState) {
  if (nightMode) {
    // deactivate Nightmode
    setNightmode(false);
  }
  // first clear matrix
  ledmatrix.gridFlush();
  // set new state
  currentState = newState;
  //entryAction(currentState);
  Serial.println("State change to: " + stateNames[currentState]);
  delay(5);
  Serial.println("FreeMemory=" + String(ESP.getFreeHeap()));
}

/**
   @brief Handler for handling commands sent to "/cmd" url

*/
void handleCommand() {
  // receive command and handle accordingly
  for (uint8_t i = 0; i < server.args(); i++) {
    Serial.print(server.argName(i));
    Serial.print(F(": "));
    (server.arg(i));
  }

  if (server.argName(0) == "led") // the parameter which was sent to this server is led color
  {
    String colorstr = server.arg(0) + "-";
    String redstr = split(colorstr, '-', 0);
    String greenstr = split(colorstr, '-', 1);
    String bluestr = split(colorstr, '-', 2);
    Serial.println(colorstr);
    Serial.println("r: " + String(redstr.toInt()));
    Serial.println("g: " + String(greenstr.toInt()));
    Serial.println("b: " + String(bluestr.toInt()));
    // set new main color
    maincolor_clock = LEDMatrix::Color24bit(redstr.toInt(), greenstr.toInt(), bluestr.toInt());
  }
  else if (server.argName(0) == "mode") // the parameter which was sent to this server is mode change
  {
    String modestr = server.arg(0);
    Serial.println("Mode change via Webserver to: " + modestr);
    // set current mode/state accordant sent mode
    if (modestr == "clock") {
      stateChange(st_clock);
    }
  }
  else if (server.argName(0) == "nightmode") {
    String modestr = server.arg(0);
    Serial.println("Nightmode change via Webserver to: " + modestr);
    if (modestr == "1") setNightmode(true);
    else setNightmode(false);
  }
  else if (server.argName(0) == "setting") {
    String timestr = server.arg(0) + "-";
    Serial.println("Nightmode setting change via Webserver to: " + timestr);
    nightModeStartHour = split(timestr, '-', 0).toInt();
    nightModeStartMin = split(timestr, '-', 1).toInt();
    nightModeEndHour = split(timestr, '-', 2).toInt();
    nightModeEndMin = split(timestr, '-', 3).toInt();
    brightness = split(timestr, '-', 4).toInt();
    writeIntEEPROM(ADR_NM_START_H, nightModeStartHour);
    writeIntEEPROM(ADR_NM_START_M, nightModeStartMin);
    writeIntEEPROM(ADR_NM_END_H, nightModeEndHour);
    writeIntEEPROM(ADR_NM_END_M, nightModeEndMin);
    writeIntEEPROM(ADR_BRIGHTNESS, brightness);
    Serial.println("Nightmode starts at: " + String(nightModeStartHour) + ":" + String(nightModeStartMin));
    Serial.println("Nightmode ends at: " + String(nightModeEndHour) + ":" + String(nightModeEndMin));
    Serial.println("Brightness: " + String(brightness));
    ledmatrix.setBrightness(brightness);
  }
  server.send(204, "text/plain", "No Content"); // this page doesn't send back content --> 204
}

/**
   @brief Splits a string at given character and return specified element

   @param s string to split
   @param parser separating character
   @param index index of the element to return
   @return String
*/
String split(String s, char parser, int index) {
  String rs = "";
  int parserIndex = index;
  int parserCnt = 0;
  int rFromIndex = 0, rToIndex = -1;
  while (index >= parserCnt) {
    rFromIndex = rToIndex + 1;
    rToIndex = s.indexOf(parser, rFromIndex);
    if (index == parserCnt) {
      if (rToIndex == 0 || rToIndex == -1) return "";
      return s.substring(rFromIndex, rToIndex);
    } else parserCnt++;
  }
  return rs;
}

/**
   @brief Handler for GET requests

*/
void handleDataRequest() {
  // receive data request and handle accordingly
  for (uint8_t i = 0; i < server.args(); i++) {
    Serial.print(server.argName(i));
    Serial.print(F(": "));
    (server.arg(i));
  }

  if (server.argName(0) == "key") // the parameter which was sent to this server is led color
  {
    String message = "{";
    String keystr = server.arg(0);
    if (keystr == "mode") {
      message += "\"mode\":\"" + stateNames[currentState] + "\"";
      message += ",";
      message += "\"modeid\":\"" + String(currentState) + "\"";
      message += ",";
      message += "\"stateAutoChange\":\"" + String(stateAutoChange) + "\"";
      message += ",";
      message += "\"nightMode\":\"" + String(nightMode) + "\"";
      message += ",";
      message += "\"nightModeStart\":\"" + leadingZero2Digit(nightModeStartHour) + "-" + leadingZero2Digit(nightModeStartMin) + "\"";
      message += ",";
      message += "\"nightModeEnd\":\"" + leadingZero2Digit(nightModeEndHour) + "-" + leadingZero2Digit(nightModeEndMin) + "\"";
      message += ",";
      message += "\"brightness\":\"" + String(brightness) + "\"";
    }
    message += "}";
    server.send(200, "application/json", message);
  }
}

/**
   @brief Set the nightmode state

   @param on true -> nightmode on
*/
void setNightmode(bool on) {
  ledmatrix.gridFlush();
  ledmatrix.drawOnMatrixInstant();
  nightMode = on;
}

/**
   @brief Write value to EEPROM

   @param address address to write the value
   @param value value to write
*/
void writeIntEEPROM(int address, int value) {
  EEPROM.put(address, value);
  EEPROM.commit();
}

/**
   @brief Read value from EEPROM

   @param address address
   @return int value
*/
int readIntEEPROM(int address) {
  int value;
  EEPROM.get(address, value);
  return value;
}

/**
   @brief Convert Integer to String with leading zero

   @param value
   @return String
*/
String leadingZero2Digit(int value) {
  String msg = "";
  if (value < 10) {
    msg = "0";
  }
  msg += String(value);
  return msg;
}
