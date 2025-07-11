/**
 * Wordclock 2.0 - Wordclock with ESP8266 and NTP time update
 *
 * created by techniccontroller 04.12.2021
 *
 * components:
 * - ESP8266
 * - Neopixelstrip
 *
 * Board settings:
 * - Board: NodeMCU 1.0 (ESP-12E Module)
 * - Flash Size: 4MB (FS:2MB OTA:~1019KB)
 * - Upload Speed: 115200
 *
 *
 * with code parts from:
 * - Adafruit NeoPixel strandtest.ino, https://github.com/adafruit/Adafruit_NeoPixel/blob/master/examples/strandtest/strandtest.ino
 * - Esp8266 und Esp32 webserver https://fipsok.de/
 * - https://github.com/pmerlin/PMR-LED-Table/blob/master/tetrisGame.ino
 * - https://randomnerdtutorials.com/wifimanager-with-esp8266-autoconnect-custom-parameter-and-manage-your-ssid-and-password/
 *
 */

#include "config.h"
#include <Adafruit_GFX.h>       // https://github.com/adafruit/Adafruit-GFX-Library
#include <Adafruit_NeoMatrix.h> // https://github.com/adafruit/Adafruit_NeoMatrix
#include <Adafruit_NeoPixel.h>  // NeoPixel library used to run the NeoPixel LEDs: https://github.com/adafruit/Adafruit_NeoPixel
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <LittleFS.h>  // Add LittleFS support for ESP32
#include "Base64.h" // copied from https://github.com/Xander-Electronics/Base64
#include <DNSServer.h>
#include <WiFiManager.h>             //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <EEPROM.h>                  //from ESP8266 Arduino Core (automatically installed when ESP8266 was installed via Boardmanager)
#include <ESP_DoubleResetDetector.h> //https://github.com/khoih-prog/ESP_DoubleResetDetector

// ----------------------------------------------------------------------------------
//                                        CONSTANTS
// ----------------------------------------------------------------------------------

#define EEPROM_SIZE 28 // size of EEPROM to save persistent variables
#define ADR_NM_START_H 0
#define ADR_NM_END_H 4
#define ADR_NM_START_M 8
#define ADR_NM_END_M 12
#define ADR_BRIGHTNESS 16
#define ADR_MAINCOLOR_CLOCK 20
#define ADR_SECONDCOLOR_CLOCK 24

// number of colors in colors array
#define NUM_COLORS 7

// own datatype for matrix movement (snake and spiral)
enum direction
{
  right,
  left,
  up,
  down
};

// own datatype for state machine states
#define NUM_STATES 6
enum ClockState
{
  st_clock,
  st_diclock,
  st_spiral,
  st_tetris,
  st_snake,
  st_pingpong
};
const String stateNames[] = {"Clock", "DiClock", "Spiral", "Tetris", "Snake", "PingPong"};
// PERIODS for each state (different for stateAutoChange or Manual mode)
const uint16_t PERIODS[2][NUM_STATES] = {{PERIOD_TIMEVISUUPDATE, // stateAutoChange = 0
                                          PERIOD_TIMEVISUUPDATE,
                                          PERIOD_ANIMATION,
                                          PERIOD_TETRIS,
                                          PERIOD_SNAKE,
                                          PERIOD_PONG},
                                         {PERIOD_TIMEVISUUPDATE, // stateAutoChange = 1
                                          PERIOD_TIMEVISUUPDATE,
                                          PERIOD_ANIMATION,
                                          PERIOD_ANIMATION,
                                          PERIOD_ANIMATION,
                                          PERIOD_PONG}};

// ports
const unsigned int localPort = 2390;
const unsigned int HTTPPort = 80;
const unsigned int logMulticastPort = 8123;
const unsigned int DNSPort = 53;

// ip addresses for multicast logging
IPAddress logMulticastIP = IPAddress(230, 120, 10, 2);

// ip addresses for Access Point
IPAddress IPAdress_AccessPoint(192, 168, 10, 2);
IPAddress Gateway_AccessPoint(192, 168, 10, 0);
IPAddress Subnetmask_AccessPoint(255, 255, 255, 0);

// hostname
const String hostname = "wordclock";

// URL DNS server
const char WebserverURL[] = "www.wordclock.local";

// own libraries
#include "otafunctions.h"
#include "udplogger.h"
#include "ledmatrix.h"
#include "animationfunctions.h"
#include "ntp_client_plus.h"
#include "tetris.h"
#include "snake.h"
#include "pong.h"
#include "wordclockfunctions.h"
#include "LittleFS_helper.h"

// ----------------------------------------------------------------------------------
//                                        GLOBAL VARIABLES
// ----------------------------------------------------------------------------------

// Webserver
WebServer server(HTTPPort);

// DNS Server
DNSServer DnsServer;

// When we setup the NeoPixel library, we tell it how many pixels, and which pin to use to send signals.
// Note that for older NeoPixel strips you might need to change the third parameter--see the strandtest
// example for more information on possible values.
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(GRID_WIDTH + 1, GRID_HEIGHT, NEOPIXELPIN, NEOPIXEL_MATRIX_TYPE, NEOPIXEL_LED_TYPE);

// seven predefined colors24bit (green, red, yellow, purple, orange, lightgreen, blue)
const uint32_t colors24bit[NUM_COLORS] = {
    LEDMatrix::Color24bit(0, 255, 0),
    LEDMatrix::Color24bit(255, 0, 0),
    LEDMatrix::Color24bit(200, 200, 0),
    LEDMatrix::Color24bit(255, 0, 200),
    LEDMatrix::Color24bit(255, 128, 0),
    LEDMatrix::Color24bit(0, 128, 0),
    LEDMatrix::Color24bit(0, 0, 255)};

uint8_t brightness = 40; // current brightness of leds
bool spiralDir = false;

// timestamp variables
long lastheartbeat = millis();      // time of last heartbeat sending
long lastStep = millis();           // time of last animation step
long lastLEDdirect = 0;             // time of last direct LED command (=> fall back to normal mode after timeout)
long lastStateChange = millis();    // time of last state change
long lastNTPUpdate = millis();      // time of last NTP update
long lastAnimationStep = millis();  // time of last Matrix update
long lastNightmodeCheck = millis(); // time of last nightmode check
long buttonPressStart = 0;          // time of push button press start

// Create necessary global objects
UDPLogger logger;
WiFiUDP NTPUDP;
NTPClientPlus ntp = NTPClientPlus(NTPUDP, "pool.ntp.org", 1, true);
LEDMatrix ledmatrix = LEDMatrix(&matrix, brightness, &logger);
Tetris mytetris = Tetris(&ledmatrix, &logger);
Snake mysnake = Snake(&ledmatrix, &logger);
Pong mypong = Pong(&ledmatrix, &logger);

float filterFactor = DEFAULT_SMOOTHING_FACTOR; // stores smoothing factor for led transition
uint8_t currentState = st_clock;               // stores current state
bool stateAutoChange = false;                  // stores state of automatic state change
bool nightMode = false;                        // stores state of nightmode
uint32_t maincolor_clock = colors24bit[1];     // color of the clock and digital clock
uint32_t secondcolor_clock = colors24bit[5];   // color of the clock and digital clock
uint32_t maincolor_snake = colors24bit[1];     // color of the random snake animation
bool apmode = false;                           // stores if WiFi AP mode is active

// nightmode settings
int nightModeStartHour = 23;
int nightModeStartMin = 0;
int nightModeEndHour = 7;
int nightModeEndMin = 0;

DoubleResetDetector *drd;

/**
 * @brief Write any type to EEPROM
 */
template<typename T>
void writeEEPROM(int address, T value) {
  EEPROM.put(address, value);
  EEPROM.commit();
}

/**
 * @brief Read any type from EEPROM
 */
template<typename T>
T readEEPROM(int address) {
  T value;
  EEPROM.get(address, value);
  return value;
}

// ----------------------------------------------------------------------------------
//                                        OTHER FUNCTIONS
// ----------------------------------------------------------------------------------

/**
 * @brief Splits a string at given character and return specified element
 *
 * @param s string to split
 * @param parser separating character
 * @param index index of the element to return
 * @return String
 */
String split(String s, char parser, int index)
{
  String rs = "";
  int parserIndex = index;
  int parserCnt = 0;
  int rFromIndex = 0, rToIndex = -1;
  while (index >= parserCnt)
  {
    rFromIndex = rToIndex + 1;
    rToIndex = s.indexOf(parser, rFromIndex);
    if (index == parserCnt)
    {
      if (rToIndex == 0 || rToIndex == -1)
        return "";
      return s.substring(rFromIndex, rToIndex);
    }
    else
      parserCnt++;
  }
  return rs;
}

/**
 * @brief Set the nightmode state
 *
 * @param on true -> nightmode on
 */
void setNightmode(bool on)
{
  ledmatrix.gridFlush();
  ledmatrix.drawOnMatrixInstant();
  nightMode = on;
}

/**
 * @brief Write value to EEPROM
 *
 * @param address address to write the value
 * @param value value to write
 */
void writeIntEEPROM(int address, int value)
{
  EEPROM.put(address, value);
  EEPROM.commit();
}

/**
 * @brief Read value from EEPROM
 *
 * @param address address
 * @return int value
 */
int readIntEEPROM(int address)
{
  int value;
  EEPROM.get(address, value);
  return value;
}

/**
 * @brief Convert Integer to String with leading zero
 *
 * @param value
 * @return String
 */
String leadingZero2Digit(int value)
{
  String msg = "";
  if (value < 10)
  {
    msg = "0";
  }
  msg += String(value);
  return msg;
}

/**
 * @brief call entry action of given state
 *
 * @param state
 */
void entryAction(uint8_t state)
{
  filterFactor = 0.5;
  switch (state)
  {
  case st_spiral:
    // Init spiral with normal drawing mode
    spiralDir = 0;
    spiral(true, spiralDir, GRID_WIDTH - 4);
    break;
  case st_tetris:
    filterFactor = 1.0; // no smoothing
    if (stateAutoChange)
    {
      randomtetris(true);
    }
    else
    {
      mytetris.ctrlStart();
    }
    break;
  case st_snake:
    if (stateAutoChange)
    {
      randomsnake(true, 8, colors24bit[1], -1);
    }
    else
    {
      filterFactor = 1.0; // no smoothing
      mysnake.initGame();
    }
    break;
  case st_pingpong:
    if (stateAutoChange)
    {
      mypong.initGame(2);
    }
    else
    {
      filterFactor = 1.0; // no smoothing
      mypong.initGame(1);
    }
    break;
  }
}

/**
 * @brief execute a state change to given newState
 *
 * @param newState the new state to be changed to
 */
void stateChange(uint8_t newState)
{
  if (nightMode)
  {
    // deactivate Nightmode
    setNightmode(false);
  }
  // first clear matrix
  ledmatrix.gridFlush();
  // set new state
  currentState = newState;
  entryAction(currentState);
  logger.logString("State change to: " + stateNames[currentState]);
  delay(5);
  logger.logString("FreeMemory=" + String(ESP.getFreeHeap()));
}

/**
 * @brief Handler for POST requests to /leddirect.
 *
 * Allows the control of all LEDs from external source.
 * It will overwrite the normal program for 5 seconds.
 * A 11x11 picture can be sent as base64 encoded string to be displayed on matrix.
 *
 */
void handleLEDDirect()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "Method Not Allowed");
  }
  else
  {
    String message = "POST data was:\n";
    /*logger.logString(message);
    delay(10);
    for (uint8_t i = 0; i < server.args(); i++) {
      message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
      logger.logString(server.arg(i));
      delay(10);
    }*/
    if (server.args() == 1)
    {
      String data = String(server.arg(0));
      int dataLength = data.length();
      // char byteArray[dataLength];
      // data.toCharArray(byteArray, dataLength);

      // base64 decoding
      char base64data[dataLength];
      data.toCharArray(base64data, dataLength);
      int base64dataLen = dataLength;
      int decodedLength = Base64.decodedLength(base64data, base64dataLen);
      char byteArray[decodedLength];
      Base64.decode(byteArray, base64data, base64dataLen);

      /*for(int i = 0; i < 10; i++){
        logger.logString(String((int)(byteArray[i])));
        delay(10);
      }*/

      for (int i = 0; i < dataLength; i += 4)
      {
        uint8_t red = byteArray[i];       // red
        uint8_t green = byteArray[i + 1]; // green
        uint8_t blue = byteArray[i + 2];  // blue
        ledmatrix.gridAddPixel((i / 4) % GRID_WIDTH, (i / 4) / GRID_HEIGHT, LEDMatrix::Color24bit(red, green, blue));
      }
      ledmatrix.drawOnMatrixInstant();

      lastLEDdirect = millis();
    }
    server.send(200, "text/plain", message);
  }
}

/**
 * @brief Check button commands
 *
 */
void handleButton()
{
  static bool lastButtonState = false;
  bool buttonPressed = !digitalRead(BUTTONPIN);
  // check rising edge
  if (buttonPressed == true && lastButtonState == false)
  {
    // button press start
    Serial.println("Button press started");
    logger.logString("Button press started");
    buttonPressStart = millis();
  }
  // check falling edge
  if (buttonPressed == false && lastButtonState == true)
  {
    // button press ended
    if ((millis() - buttonPressStart) > LONGPRESS)
    {
      // longpress -> nightmode
      Serial.println("Button press ended - longpress");
      logger.logString("Button press ended - longpress");

      setNightmode(true);
    }
    else if ((millis() - buttonPressStart) > SHORTPRESS)
    {
      // shortpress -> state change
      Serial.println("Button press ended - shortpress");
      logger.logString("Button press ended - shortpress");

      if (nightMode)
      {
        setNightmode(false);
      }
      else
      {
        stateChange((currentState + 1) % NUM_STATES);
      }
    }
  }
  lastButtonState = buttonPressed;
}

/**
 * @brief Handler for handling commands sent to "/cmd" url
 *
 */
void handleCommand()
{
  // receive command and handle accordingly
  for (uint8_t i = 0; i < server.args(); i++)
  {
    Serial.print(server.argName(i));
    Serial.print(F(": "));
    Serial.println(server.arg(i));
  }

  if (server.argName(0) == "led") // the parameter which was sent to this server is led color
  {
    String colorstr = server.arg(0) + "-";
    String redstr = split(colorstr, '-', 0);
    String greenstr = split(colorstr, '-', 1);
    String bluestr = split(colorstr, '-', 2);
    logger.logString(colorstr);
    logger.logString("r: " + String(redstr.toInt()));
    logger.logString("g: " + String(greenstr.toInt()));
    logger.logString("b: " + String(bluestr.toInt()));
    // set new main color
    maincolor_clock = LEDMatrix::Color24bit(redstr.toInt(), greenstr.toInt(), bluestr.toInt());
    secondcolor_clock = LEDMatrix::Color24bit(255 - redstr.toInt(), 255 - greenstr.toInt(), 255 - bluestr.toInt());
    if (secondcolor_clock == LEDMatrix::Color24bit(0, 0, 0))
    {
      maincolor_clock = LEDMatrix::Color24bit(125, 125, 125);
      secondcolor_clock = LEDMatrix::Color24bit(255, 255, 255);
    }
    // Save colors to EEPROM
    writeEEPROM<uint32_t>(ADR_MAINCOLOR_CLOCK, maincolor_clock);
    writeEEPROM<uint32_t>(ADR_SECONDCOLOR_CLOCK, secondcolor_clock);
  }
  else if (server.argName(0) == "mode") // the parameter which was sent to this server is mode change
  {
    String modestr = server.arg(0);
    logger.logString("Mode change via Webserver to: " + modestr);
    // set current mode/state accordant sent mode
    if (modestr == "clock")
    {
      stateChange(st_clock);
    }
    else if (modestr == "diclock")
    {
      stateChange(st_diclock);
    }
    else if (modestr == "spiral")
    {
      stateChange(st_spiral);
    }
    else if (modestr == "tetris")
    {
      stateChange(st_tetris);
    }
    else if (modestr == "snake")
    {
      stateChange(st_snake);
    }
    else if (modestr == "pingpong")
    {
      stateChange(st_pingpong);
    }
  }
  else if (server.argName(0) == "nightmode")
  {
    String modestr = server.arg(0);
    logger.logString("Nightmode change via Webserver to: " + modestr);
    if (modestr == "1")
      setNightmode(true);
    else
      setNightmode(false);
  }
  else if (server.argName(0) == "setting")
  {
    String timestr = server.arg(0) + "-";
    logger.logString("Nightmode setting change via Webserver to: " + timestr);
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
    logger.logString("Nightmode starts at: " + String(nightModeStartHour) + ":" + String(nightModeStartMin));
    logger.logString("Nightmode ends at: " + String(nightModeEndHour) + ":" + String(nightModeEndMin));
    logger.logString("Brightness: " + String(brightness));
    ledmatrix.setBrightness(brightness);
  }
  else if (server.argName(0) == "stateautochange")
  {
    String modestr = server.arg(0);
    logger.logString("stateAutoChange change via Webserver to: " + modestr);
    if (modestr == "1")
      stateAutoChange = true;
    else
      stateAutoChange = false;
  }
  else if (server.argName(0) == "tetris")
  {
    String cmdstr = server.arg(0);
    logger.logString("Tetris cmd via Webserver to: " + cmdstr);
    if (cmdstr == "up")
    {
      mytetris.ctrlUp();
    }
    else if (cmdstr == "left")
    {
      mytetris.ctrlLeft();
    }
    else if (cmdstr == "right")
    {
      mytetris.ctrlRight();
    }
    else if (cmdstr == "down")
    {
      mytetris.ctrlDown();
    }
    else if (cmdstr == "play")
    {
      mytetris.ctrlStart();
    }
    else if (cmdstr == "pause")
    {
      mytetris.ctrlPlayPause();
    }
  }
  else if (server.argName(0) == "snake")
  {
    String cmdstr = server.arg(0);
    logger.logString("Snake cmd via Webserver to: " + cmdstr);
    if (cmdstr == "up")
    {
      mysnake.ctrlUp();
    }
    else if (cmdstr == "left")
    {
      mysnake.ctrlLeft();
    }
    else if (cmdstr == "right")
    {
      mysnake.ctrlRight();
    }
    else if (cmdstr == "down")
    {
      mysnake.ctrlDown();
    }
    else if (cmdstr == "new")
    {
      mysnake.initGame();
    }
  }
  else if (server.argName(0) == "pong")
  {
    String cmdstr = server.arg(0);
    logger.logString("Pong cmd via Webserver to: " + cmdstr);
    if (cmdstr == "up")
    {
      mypong.ctrlUp(1);
    }
    else if (cmdstr == "down")
    {
      mypong.ctrlDown(1);
    }
    else if (cmdstr == "new")
    {
      mypong.initGame(1);
    }
  }
  server.send(204, "text/plain", "No Content"); // this page doesn't send back content --> 204
}

/**
 * @brief Handler for GET requests
 *
 */
void handleDataRequest()
{
  // receive data request and handle accordingly
  for (uint8_t i = 0; i < server.args(); i++)
  {
    Serial.print(server.argName(i));
    Serial.print(F(": "));
    Serial.println(server.arg(i));
  }

  if (server.argName(0) == "key") // the parameter which was sent to this server is led color
  {
    String message = "{";
    String keystr = server.arg(0);
    if (keystr == "mode")
    {
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

// ----------------------------------------------------------------------------------
//                                        SETUP
// ----------------------------------------------------------------------------------

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.printf("\nSketchname: %s\nBuild: %s\n", (__FILE__), (__TIMESTAMP__));
  Serial.println();

  // Init EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // configure button pin as input
  pinMode(BUTTONPIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);

  // Double Reset Detector
  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);

  // setup Matrix LED functions
  ledmatrix.setupMatrix();
  ledmatrix.setCurrentLimit(CURRENT_LIMIT_LED);

  // Turn on minutes leds (blue)
  ledmatrix.setMinIndicator(15, colors24bit[6]);
  ledmatrix.drawOnMatrixInstant();

  /** Use WiFiMaanger for handling initial Wifi setup **/

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  wifiManager.setAPCallback([](WiFiManager *wm)
                            { digitalWrite(LED_BUILTIN, LOW); });

  if (drd->detectDoubleReset())
  {
    Serial.println("Double Reset Detected");
    wifiManager.setConfigPortalTimeout(DBD_CONFIG_PORTAL_TIMEOUT);
    wifiManager.startConfigPortal(AP_SSID);
  }
  // Uncomment and run it once, if you want to erase all the stored information
  // wifiManager.resetSettings();

  // set custom ip for portal
  // wifiManager.setAPStaticIPConfig(IPAdress_AccessPoint, Gateway_AccessPoint, Subnetmask_AccessPoint);

  // fetches ssid and pass from eeprom and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here "wordclockAP"
  // and goes into a blocking loop awaiting configuration
  wifiManager.autoConnect(AP_SSID);

  digitalWrite(LED_BUILTIN, HIGH);

  // if you get here you have connected to the WiFi
  Serial.println("Connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Turn off minutes leds
  ledmatrix.setMinIndicator(15, 0);
  ledmatrix.drawOnMatrixInstant();

  /** (alternative) Use directly STA/AP Mode of ESP8266   **/

  /*
  // We start by connecting to a WiFi network
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  // We start by connecting to a WiFi network
  WiFi.mode(WIFI_STA);
  //Set new hostname
  WiFi.hostname(hostname.c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  //wifi_station_set_hostname("esplamp");

  int timeoutcounter = 0;
  while (WiFi.status() != WL_CONNECTED && timeoutcounter < 30) {
    ledmatrix.setMinIndicator(15, colors24bit[6]);
    ledmatrix.drawOnMatrixInstant();
    delay(250);
    ledmatrix.setMinIndicator(15, 0);
    ledmatrix.drawOnMatrixInstant();
    delay(250);
    Serial.print(".");
    timeoutcounter++;
  }

  // start request of program
  if (WiFi.status() == WL_CONNECTED) {      //Check WiFi connection status
    Serial.println("");

    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

  } else {
    // no wifi found -> open access point
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAdress_AccessPoint, Gateway_AccessPoint, Subnetmask_AccessPoint);
    WiFi.softAP(AP_SSID, AP_PASS);
    apmode = true;

    // start DNS Server
    DnsServer.setTTL(300);
    DnsServer.start(DNSPort, WebserverURL, IPAdress_AccessPoint);

    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
  }*/

  // init ESP8266 File manager (LittleFS)
  setupFS();

  // setup OTA
  setupOTA(hostname);

  server.on("/cmd", handleCommand);                    // process commands
  server.on("/data", handleDataRequest);               // process datarequests
  server.on("/leddirect", HTTP_POST, handleLEDDirect); // Call the 'handleLEDDirect' function when a POST request is made to URI "/leddirect"
  server.begin();

  // create UDP Logger to send logging messages via UDP multicast
  logger = UDPLogger(WiFi.localIP(), logMulticastIP, logMulticastPort);
  logger.setName("Wordclock 2.0");
  logger.logString("Start program\n");
  delay(10);
  logger.logString("Sketchname: " + String(__FILE__));
  delay(10);
  logger.logString("Build: " + String(__TIMESTAMP__));
  delay(10);
  logger.logString("IP: " + WiFi.localIP().toString());

  for (int r = 0; r < GRID_HEIGHT; r++)
  {
    for (int c = 0; c < GRID_WIDTH; c++)
    {
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
  ledmatrix.printNumber(0, 6, (address / 100), maincolor_clock);
  ledmatrix.printNumber(4, 6, (address / 10) % 10, maincolor_clock);
  ledmatrix.printNumber(8, 6, address % 10, maincolor_clock);
  ledmatrix.drawOnMatrixInstant();
  delay(500);

  // clear matrix
  ledmatrix.gridFlush();
  ledmatrix.drawOnMatrixInstant();

  // setup NTP
  ntp.setupNTPClient();
  logger.logString("NTP running");
  logger.logString("Time: " + ntp.getFormattedTime());
  logger.logString("TimeOffset (seconds): " + String(ntp.getTimeOffset()));

  // show the current time for short time in words
  int hours = ntp.getHours24();
  int minutes = ntp.getMinutes();
  String timeMessage = timeToString(hours, minutes);
  showStringOnClock(timeMessage, maincolor_clock);
  drawMinuteIndicator(minutes, maincolor_clock);
  ledmatrix.drawOnMatrixSmooth(filterFactor);
  delay(500);

  // init all animation modes
  // init snake
  randomsnake(true, 8, colors24bit[1], -1);
  // init spiral
  spiral(true, spiralDir, GRID_WIDTH - 4);
  // init random tetris
  randomtetris(true);

  // show countdown
  /*for(int i = 9; i > 0; i--){
    logger.logString("DiTest: " + String(i));
    Serial.println("DiTest: " + String(i));
    ledmatrix.gridFlush();
    printNumber(4, 3, i, maincolor_clock);
    ledmatrix.drawOnMatrixInstant();
    delay(1000);
  }*/

  // Read nightmode setting from EEPROM
  nightModeStartHour = readIntEEPROM(ADR_NM_START_H);
  nightModeStartMin = readIntEEPROM(ADR_NM_START_M);
  nightModeEndHour = readIntEEPROM(ADR_NM_END_H);
  nightModeEndMin = readIntEEPROM(ADR_NM_END_M);
  logger.logString("Nightmode starts at: " + String(nightModeStartHour) + ":" + String(nightModeStartMin));
  logger.logString("Nightmode ends at: " + String(nightModeEndHour) + ":" + String(nightModeEndMin));

  // Read brightness setting from EEPROM
  brightness = readIntEEPROM(ADR_BRIGHTNESS);
  logger.logString("Brightness: " + String(brightness));
  ledmatrix.setBrightness(brightness);

  // Read clock colors from EEPROM
  uint32_t savedMainColor = readEEPROM<uint32_t>(ADR_MAINCOLOR_CLOCK);
  uint32_t savedSecondColor = readEEPROM<uint32_t>(ADR_SECONDCOLOR_CLOCK);
  if (savedMainColor != 0xFFFFFFFF && savedMainColor != 0) maincolor_clock = savedMainColor;
  if (savedSecondColor != 0xFFFFFFFF && savedSecondColor != 0) secondcolor_clock = savedSecondColor;

  // show initial state
  showStringOnClock("00:00", maincolor_clock);
  drawMinuteIndicator(0, maincolor_clock);
  ledmatrix.drawOnMatrixSmooth(filterFactor);

  // Set matrix rotation from config
  #if MATRIX_ROTATION == 0
    matrix.setRotation(0);
  #elif MATRIX_ROTATION == 90
    matrix.setRotation(1);
  #elif MATRIX_ROTATION == 180
    matrix.setRotation(2);
  #elif MATRIX_ROTATION == 270
    matrix.setRotation(3);
  #else
    matrix.setRotation(0); // fallback
  #endif
}

// ----------------------------------------------------------------------------------
//                                        LOOP
// ----------------------------------------------------------------------------------

void loop()
{
  drd->loop();

  // handle OTA
  handleOTA();

  // handle Webserver
  server.handleClient();

  if (AUTO_RESTART_ENABLED && ((millis() > AUTO_RESTART_MILLIS) && (ntp.getHours24() == AUTO_RESTART_HOUR)))
  {
    ESP.restart();
    delay(1000);
    return;
  }

  // send regularly heartbeat messages via UDP multicast
  if (millis() - lastheartbeat > PERIOD_HEARTBEAT)
  {
    logger.logString("Heartbeat, state: " + stateNames[currentState] + "\n");
    lastheartbeat = millis();

    // Check wifi status (only if no apmode)
    if (!apmode && WiFi.status() != WL_CONNECTED)
    {
      Serial.println("connection lost");
      ledmatrix.gridAddPixel(0, 5, colors24bit[1]);
      ledmatrix.drawOnMatrixInstant();
    }
  }

  // handle mode behaviours (trigger loopCycles of different modes depending on current mode)
  if (!nightMode && (millis() - lastStep > PERIODS[stateAutoChange][currentState]) && (millis() - lastLEDdirect > TIMEOUT_LEDDIRECT))
  {
    switch (currentState)
    {
    // state clock
    case st_clock:
    {
      int hours = ntp.getHours24();
      int minutes = ntp.getMinutes();
      showStringOnClock(timeToString(hours, minutes), maincolor_clock);
      drawMinuteIndicator(minutes, maincolor_clock);
    }
    break;
    // state diclock
    case st_diclock:
    {
      int hours = ntp.getHours24();
      int minutes = ntp.getMinutes();
      showDigitalClock(hours, minutes, maincolor_clock, secondcolor_clock);
    }
    break;
    // state spiral
    case st_spiral:
    {
      int res = spiral(false, spiralDir, GRID_WIDTH - 4);
      if (res && spiralDir == 0)
      {
        // change spiral direction to closing (draw empty leds)
        spiralDir = 1;
        // init spiral with new spiral direction
        spiral(true, spiralDir, GRID_WIDTH - 4);
      }
      else if (res && spiralDir == 1)
      {
        // reset spiral direction to normal drawing leds
        spiralDir = 0;
        // init spiral with new spiral direction
        spiral(true, spiralDir, GRID_WIDTH - 4);
      }
    }
    break;
    // state tetris
    case st_tetris:
    {
      if (stateAutoChange)
      {
        randomtetris(false);
      }
      else
      {
        mytetris.loopCycle();
      }
    }
    break;
    // state snake
    case st_snake:
    {
      if (stateAutoChange)
      {
        ledmatrix.gridFlush();
        int res = randomsnake(false, 8, maincolor_snake, -1);
        if (res)
        {
          // init snake for next run
          randomsnake(true, 8, maincolor_snake, -1);
        }
      }
      else
      {
        mysnake.loopCycle();
      }
    }
    break;
    // state pingpong
    case st_pingpong:
    {
      mypong.loopCycle();
    }
    break;
    }

    lastStep = millis();
  }

  // periodically write colors to matrix
  if (!nightMode && (millis() - lastAnimationStep > PERIOD_MATRIXUPDATE))
  {
    ledmatrix.drawOnMatrixSmooth(filterFactor);
    lastAnimationStep = millis();
  }

  // handle button press
  handleButton();

  // handle state changes
  if (stateAutoChange && (millis() - lastStateChange > PERIOD_STATECHANGE) && !nightMode)
  {
    // increment state variable and trigger state change
    stateChange((currentState + 1) % NUM_STATES);

    // save last automatic state change
    lastStateChange = millis();
  }

  // NTP time update
  if (millis() - lastNTPUpdate > PERIOD_NTPUPDATE)
  {
    if (ntp.updateNTP())
    {
      ntp.calcDate();
      logger.logString("NTP-Update successful");
      logger.logString("Time: " + ntp.getFormattedTime());
      logger.logString("TimeOffset (seconds): " + String(ntp.getTimeOffset()));
      Serial.println("NTP-Update successful");
    }
    else
    {
      logger.logString("NTP-Update not successful");
      Serial.println("NTP-Update not successful");
    }
    lastNTPUpdate = millis();
  }

  // check if nightmode need to be activated
  if (millis() - lastNightmodeCheck > PERIOD_NIGHTMODECHECK)
  {
    int hours = ntp.getHours24();
    int minutes = ntp.getMinutes();

    if (hours == nightModeStartHour && minutes == nightModeStartMin)
    {
      setNightmode(true);
    }
    else if (hours == nightModeEndHour && minutes == nightModeEndMin)
    {
      setNightmode(false);
    }

    lastNightmodeCheck = millis();
  }
}
