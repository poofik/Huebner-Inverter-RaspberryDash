//#include <ArduinoOTA.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#include <Update.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif

#include <EEPROM.h>
#include <AESLib.h>
#include <ESPAsyncWebServer.h>
#include <flash_hal.h>
#include <StreamString.h>
#include <LittleFS.h>

#ifdef ARDUINO_MOD_WIFI_ESP8266
#define LED_BUILTIN 1 //GPIO1=Olimex
#else
#define LED_BUILTIN 2 //GPIO2=ESP-12/WeMos(D4)
#endif

//#define DEBUG false

AsyncWebServer server(80);

int WIFI_PHY_MODE = 1; //WIFI_PHY_MODE_11B = 1, WIFI_PHY_MODE_11G = 2, WIFI_PHY_MODE_11N = 3
float WIFI_PHY_POWER = 20.5; //Max = 20.5dbm
int ACCESS_POINT_MODE = 0;
char ACCESS_POINT_SSID[] = "Dashboard";
char ACCESS_POINT_PASSWORD[] = "dashboard123";
int ACCESS_POINT_CHANNEL = 11;
int ACCESS_POINT_HIDE = 0;
int DATA_LOG = 0; //Enable data logger
int LOG_INTERVAL = 5; //seconds between data collection and write to LittleFS
int NETWORK_DHCP = 0;
char NETWORK_IP[] = "192.168.4.1";
char NETWORK_SUBNET[] = "255.255.255.0";
char NETWORK_GATEWAY[] = "192.168.4.1";
char NETWORK_DNS[] = "8.8.8.8";
int NOTIFY_ENABLED = 0;
char NOTIFY_EMAIL[] = "";
char NOTIFY_EMAIL_SMTP[] = "";
char NOTIFY_EMAIL_USERNAME[] = "";
char NOTIFY_EMAIL_PASSWORD[] = "";

File logFile;
uint32_t syncTime = 0;
const char text_html[] = "text/html";
const char text_plain[] = "text/plain";
const char text_json[] = "application/json";
static const char serverIndex[] PROGMEM =
  R"(<!DOCTYPE html>
<html lang='en'>
<head>
   <meta charset='utf-8'>
   <meta name='viewport' content='width=device-width,initial-scale=1'/>
</head>
<body>
<form method='POST' action='' enctype='multipart/form-data'>
   <input type='file' accept='.bin' name='firmware'>
   <input type='submit' value='Update Firmware'>
</form>
<br>
<form method='POST' action='' enctype='multipart/form-data'>
   <input type='file' accept='.bin' name='filesystem'>
   <input type='submit' value='Update Filesystem'>
</form>
</body>
</html>)";
bool restartRequired = false;  // Set this flag in the callbacks to restart ESP in the main loop

//====================
//CAN-Bus
//====================
/*
  https://github.com/coryjfowler/MCP_CAN_lib
  http://scottsnowden.co.uk/esp8266-mcp2515-can-bus-to-wifi-gateway/
  http://www.canhack.org/board/viewtopic.php?f=1&t=1041
  http://forum.arduino.cc/index.php?topic=152145.0
  https://github.com/Metaln00b/NodeMCU-BlackBox
*/
/*
   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
   The NodeMCU is not officially 5V tolerant.
   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
   Connect TJA1050-Chip separately to 5V of external power,
   because the TJA1050-Chip can not run with 3V3.
*/
#include <mcp_can.h>
#include <SPI.h>
/*
  #define MCP_8MHz_250kBPS_CFG1 (0x40)
  #define MCP_8MHz_250kBPS_CFG2 (0xF1)
  #define MCP_8MHz_250kBPS_CFG3 (0x85)
*/
/*
  MISO=D7(GPIO12),MOSI=D6(GPIO13),SCLK=D5(GPIO14),CS=D2(GPIO4),INT=D4(GPIO2)
  https://arduino-esp8266.readthedocs.io/en/2.4.0-rc1/libraries.html#spi

  There’s an extended mode where you can swap the normal pins to the SPI0 hardware pins.
  This is enabled by calling SPI.pins(6, 7, 8, 0) before the call to SPI.begin().

  The pins would change to: MOSI=SD1,MISO=SD0,SCLK=CLK,HWCS=GPIO0
*/

/* WeMos D1 Pins for MCP2515: CS=D2, INT=D4, SCK=D5, SO=D6, SI=D7 */

#define DEBUG_MODE 0
MCP_CAN CAN0(4);     // Set CS to pin GPIO4 (D2) or GPIO15 (D8)

int CAN_ID_FILTERS[10];

/*
  Standard CANId: 0x800 (max = 2048)
  Extended CANId: 0xPPPXXXXSS
  P = priority;  low value = higher priority;
      0x00=0
      0x0F=15
      0x10=16
      0x1C=20
      0x20=32
      0x90=144
      0xFF=255;
  XXXX = PNG, parameter group number, 4 chars / 8 bytes long
  SS = source address,
*/
long unsigned int CANmsgId;
unsigned char CANmsg[8];
//=============================

AESLib aesLib;

char cleartext[256];
char ciphertext[512];

// AES Encryption Key
byte aes_key[] = { 0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C };

// Initialization Vector
byte aes_iv[N_BLOCK] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Generate IV (once)
void aes_init() {
  aesLib.gen_iv(aes_iv);
}

String encrypt(char * msg, byte iv[]) {
  int msgLen = strlen(msg);
  char encrypted[2 * msgLen];
  aesLib.encrypt64(msg, msgLen, encrypted, aes_key, sizeof(aes_key), iv);
  return String(encrypted);
}

String decrypt(char * msg, byte iv[]) {
  int msgLen = strlen(msg);
  char decrypted[msgLen]; // half may be enough
  aesLib.decrypt64(msg, msgLen, decrypted, aes_key, sizeof(aes_key), iv);
  return String(decrypted);
}

void setup()
{
  Serial.begin(115200, SERIAL_8N1);
  //Serial.setDebugOutput(false);
  //Serial.setTimeout(1000);

  //===========
  //File system
  //===========
  LittleFS.begin();

  //======================
  //NVRAM type of Settings
  //======================
  EEPROM.begin(1024);
  uint8_t e = EEPROM.read(0);
  String nvram = "";

  if (e != 48 && e != 49) {
    //aes_init(); //Generate random IV
    NVRAM_Erase();
    NVRAM_Write(0, String(ACCESS_POINT_MODE));
    NVRAM_Write(1, String(ACCESS_POINT_HIDE));
    NVRAM_Write(2, String(ACCESS_POINT_CHANNEL));
    NVRAM_Write(3, ACCESS_POINT_SSID);
    NVRAM_Write(4, ACCESS_POINT_PASSWORD);
    NVRAM_Write(5, String(DATA_LOG));
    NVRAM_Write(6, String(LOG_INTERVAL));
    //==========
    NVRAM_Write(7, String(NETWORK_DHCP));
    NVRAM_Write(8, NETWORK_IP);
    NVRAM_Write(9, NETWORK_SUBNET);
    NVRAM_Write(10, NETWORK_GATEWAY);
    NVRAM_Write(11, NETWORK_DNS);
    //==========
    NVRAM_Write(12, String(NOTIFY_ENABLED));
    NVRAM_Write(13, NOTIFY_EMAIL);
    NVRAM_Write(14, NOTIFY_EMAIL_SMTP);
    NVRAM_Write(15, NOTIFY_EMAIL_USERNAME);
    nvram = encrypt(NOTIFY_EMAIL_PASSWORD, aes_iv); //Serial.println(nvram); //DEBUG
    NVRAM_Write(16, nvram);

    LittleFS.format();
  } else {
    ACCESS_POINT_MODE = NVRAM_Read(0).toInt();
    ACCESS_POINT_HIDE = NVRAM_Read(1).toInt();
    ACCESS_POINT_CHANNEL = NVRAM_Read(2).toInt();
    nvram = NVRAM_Read(3);
    nvram.toCharArray(ACCESS_POINT_SSID, nvram.length() + 1);
    nvram = NVRAM_Read(4);
    nvram.toCharArray(ACCESS_POINT_PASSWORD, nvram.length() + 1);
    DATA_LOG = NVRAM_Read(5).toInt();
    LOG_INTERVAL = NVRAM_Read(6).toInt();
    //==========
    NETWORK_DHCP = NVRAM_Read(7).toInt();
    nvram = NVRAM_Read(8);
    nvram.toCharArray(NETWORK_IP, nvram.length() + 1);
    nvram = NVRAM_Read(9);
    nvram.toCharArray(NETWORK_SUBNET, nvram.length() + 1);
    nvram = NVRAM_Read(10);
    nvram.toCharArray(NETWORK_GATEWAY, nvram.length() + 1);
    nvram = NVRAM_Read(11);
    nvram.toCharArray(NETWORK_DNS, nvram.length() + 1);
    //==========
    NOTIFY_ENABLED = NVRAM_Read(12).toInt();
    nvram = NVRAM_Read(13);
    nvram.toCharArray(NOTIFY_EMAIL, nvram.length() + 1);
    nvram = NVRAM_Read(14);
    nvram.toCharArray(NOTIFY_EMAIL_SMTP, nvram.length() + 1);
    nvram = NVRAM_Read(15);
    nvram.toCharArray(NOTIFY_EMAIL_USERNAME, nvram.length() + 1);
    nvram = NVRAM_Read(16);
    //nvram = decrypt(string2char(nvram), aes_iv); //Serial.println(p); //DEBUG
    nvram.toCharArray(NOTIFY_EMAIL_PASSWORD, nvram.length() + 1);
  }
  //EEPROM.end();

  WiFi.setPhyMode((WiFiPhyMode_t)WIFI_PHY_MODE);
  WiFi.setOutputPower(WIFI_PHY_POWER);

  IPAddress ip, gateway, subnet, dns;
  ip.fromString(NETWORK_IP);
  subnet.fromString(NETWORK_SUBNET);
  gateway.fromString(NETWORK_GATEWAY);
  dns.fromString(NETWORK_DNS);

  if (ACCESS_POINT_MODE == 0) {
    //=====================
    //WiFi Access Point Mode
    //=====================
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(ip, gateway, subnet);
    WiFi.softAP(ACCESS_POINT_SSID, ACCESS_POINT_PASSWORD, ACCESS_POINT_CHANNEL, ACCESS_POINT_HIDE);
    //Serial.println(WiFi.softAPIP());
  } else {
    //================
    //WiFi Client Mode
    //================
    WiFi.mode(WIFI_STA);
    if (NETWORK_DHCP == 0) {
      WiFi.config(ip, dns, gateway, subnet);
    }
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.begin(ACCESS_POINT_SSID, ACCESS_POINT_PASSWORD);  //Connect to the WiFi network
    //WiFi.enableAP(0);
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
#if DEBUG
      Serial.println("Connection Failed! Rebooting...");
#endif
      //If client mode fails ESP8266 will not be accessible
      //Set Emergency AP SSID for re-configuration
      NVRAM_Write(0, "0");
      NVRAM_Write(3, "_" + String(ACCESS_POINT_SSID));
      delay(5000);
      ESP.restart();
    }
#if DEBUG
    Serial.println(WiFi.localIP());
#endif
  }

  //===============
  //Data Logger
  //===============
  if (DATA_LOG == 1)
    logFile = LittleFS.open("/log.txt", "a");

  //===============
  //Async Web Server
  //===============

  server.on("/can/read",  HTTP_GET, [](AsyncWebServerRequest * request) {

    AsyncResponseStream *response = request->beginResponseStream(text_plain);
    response->addHeader("Cache-Control", "no-store");

    if (request->hasParam("sdo"))
    {
      //https://openinverter.org/wiki/CAN_communication#Setting_and_reading_parameters_via_SDO
      //CANId Receive Filter (0x581/1409)

      byte txBuf0[] = {0x22, 0x00, 0x20, 0x00, 0, 0, 0, 0}; //0x00 is node id (see: include/param_prj.h)

      String sz = request->getParam("sdo")->value();
      if (sz.indexOf(",") != -1)
      {
        char buf[sz.length() + 1];
        sz.toCharArray(buf, sizeof(buf));
        char *p = buf;
        char *str;

        while ((str = strtok_r(p, ",", &p)) != NULL) //split
        {
          txBuf0[3] = String(str).toInt();
          CAN0.sendMsgBuf(0x601, 0, 8, txBuf0);
          response->print(CANReceive());
        }
      } else {
        txBuf0[3] = sz.toInt();
        CAN0.sendMsgBuf(0x601, 0, 8, txBuf0);
      }
    }
    response->print(CANReceive());
    request->send(response);
  });
  server.on("/can/write",  HTTP_GET, [](AsyncWebServerRequest * request) {

    byte txBuf0[] = {0, 0, 0, 0, 0, 0, 0, 0};

    String id = request->getParam("id")->value();
    String sz = request->getParam("data")->value();
    if (sz.indexOf(",") != -1)
    {
      uint8_t i = 0;
      char buf[sz.length() + 1];
      sz.toCharArray(buf, sizeof(buf));
      char *p = buf;
      char *str;
      while ((str = strtok_r(p, ",", &p)) != NULL) //split
      {
        txBuf0[i] = String(str).toInt();
        i++;
      }
      CAN0.sendMsgBuf(id.toInt(), 0, 8, txBuf0);
    }
    request->send(200, text_plain, "CAN Message Sent");
  });
  server.on("/can/filter", HTTP_GET, [](AsyncWebServerRequest * request) {

    //Data bytes are ONLY checked when the MCP2515 is in 'MCP_STDEXT' mode via the begin
    //function, otherwise ('MCP_STD') only the ID is checked.

    //https://github.com/SeeedDocument/CAN_BUS_Shield/raw/master/resource/MCP2515.pdf
    //RXB0 has RXM0 (Mask 0), RXF0, and RXF1 (Filter 0 and Filter 1).
    //RXB1 has RXM1 (Mask 1), RXF2, RXF3, RXF4, and RXF5 (Filters 2, 3, 4, and 5).

    uint32_t RXM = 0x00000000;
    uint32_t RXF = 0x00000000;

    if (request->hasParam("id"))
    {
      String sz = request->getParam("id")->value();
      if (sz.indexOf(",") != -1)
      {
        uint8_t i = 0;
        char buf[sz.length() + 1];
        sz.toCharArray(buf, sizeof(buf));
        char *p = buf;
        char *str;

        memset(CAN_ID_FILTERS, 0, sizeof(CAN_ID_FILTERS));
        while ((str = strtok_r(p, ",", &p)) != NULL) //split
        {
          //TODO: Build Range from Filters

          RXM = String(str).toInt() >> 8;
          RXF = String(str).toInt() >> 8;
          CAN_ID_FILTERS[i] = RXF;
          i++;
        }
      } else {
        RXM = sz.toInt() >> 8;
        RXF = sz.toInt() >> 8;
        CAN_ID_FILTERS[0] = RXF;
      }
    }

    CAN0.init_Filt(0, 0, RXM); //Mask0 is for Filter0 and Filter1
    CAN0.init_Filt(0, 0, RXF); //0
    CAN0.init_Filt(1, 0, RXF); //1

    CAN0.init_Mask(1, 0, RXM); //Mask1 is only for Filter2, 3, 4, and 5
    CAN0.init_Filt(2, 0, RXF); //2
    CAN0.init_Filt(3, 0, RXF); //3
    CAN0.init_Filt(4, 0, RXF); //4
    CAN0.init_Filt(5, 0, RXF); //5

    /*
      CAN0.init_Mask(0,0,0x03A00000); //Mask0 is for Filter0 and Filter1
      CAN0.init_Filt(0,0,0x03A00000);
      CAN0.init_Filt(1,0,0x03A00000);

      CAN0.init_Mask(1,0,0x03A00000); //Mask1 is only for Filter2, 3, 4, and 5
      CAN0.init_Filt(2,0,0x03A00000);
      CAN0.init_Filt(3,0,0x03A00000);
      CAN0.init_Filt(4,0,0x03A00000);
      CAN0.init_Filt(5,0,0x03A00000);
    */

    /*
      https://forum.arduino.cc/index.php?topic=527028.0
    */
    //Set Mask 0 + Filter 0, Standard Addressing 11 bits
    //CAN0.init_Mask(0, 0, 0x07FF); //Mask0 is for Filter0 and Filter1
    //CAN0.init_Filt(0, 0, 0x740);

    //Set Mask 1 + Filter 2, Extended Addressing // 18 bits
    //CAN0.init_Mask(1, 1, (SA<<18) | EA); //Mask1 is only for Filter2, 3, 4, and 5
    //CAN0.init_Filt(2, 1, (SAMask<<18) | EAMask);

    /*
      http://www.savvysolutions.info/savvymicrocontrollersolutions/arduino.php?topic=arduino-seeedstudio-CAN-BUS-shield
    */
    //Generally, set the mask to 0xFFFFFFF and then apply filters
    //to each of the messages you want to allow to pass to the CAN bus shield.
    //
    //Mask 0xFFFFFFF & filter 0xFFFFFFF disables all messages
    //Mask 0xFFFFFFF & filter 0x0 disables all messages (mask disables filter)
    //Mask 0x0 & filter 0x0 allows all messages to pass
    //Mask 0x0 & filter 0xFFFFFFF allows msg 0xCF00400 to be received
    //Mask 0xFFFFFFF & filter 0xCF00400 allows msg 0xCF00400 to be received

    //=================================
    //After applying Filters reset Mode
    //=================================
    // SeeedStudio Library
    //CAN0.setMode(MODE_NORMAL);
    //CAN0.setMode(MODE_LISTENONLY);
    //-------------------
    // Coryjfowler Library
    CAN0.setMode(MCP_NORMAL);
    //CAN0.setMode(MCP_LISTENONLY);
    //=================================

    request->send(200, text_plain, "CAN Filter Set");
  });
  /*
    server.on("/can/sleep", [](AsyncWebServerRequest * request) {
    CAN0.sleep();  //MCP2515 will NOT wake up on incoming messages
    request->send(200, text_plain, "OK");
    });
    server.on("/can/wake", [](AsyncWebServerRequest * request){
    CAN0.wakeup(); //MCP2515 will wake up on incoming messages
    request->send(200, text_plain, "OK");
    });
  */
  server.on("/format", HTTP_GET, [](AsyncWebServerRequest * request) {
    String result = LittleFS.format() ? "OK" : "Error";
    FSInfo fs_info;
    LittleFS.info(fs_info);
    request->send(200, text_plain, "<b>Format " + result + "</b><br/>Total Flash Size: " + String(ESP.getFlashChipSize()) + "<br>LittleFS Size: " + String(fs_info.totalBytes) + "<br>LittleFS Used: " + String(fs_info.usedBytes));
  });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, text_plain, "...");
    restartRequired = true;
    //ESP.restart();
  });
  server.on("/aes", HTTP_GET, [](AsyncWebServerRequest * request) {

    aes_init(); //Generate random IV

    String out = "AES IV: ";
    for (int i = 0; i < sizeof(aes_iv); i++) {
      out += String(aes_iv[i], DEC);
      if ((i + 1) < sizeof(aes_iv)) {
        out += ",";
      }
    }
    request->send(200, text_plain, out);
  });
  server.on("/nvram", HTTP_GET, [](AsyncWebServerRequest * request) {

    String out = "{}";

    if (request->hasParam("network")) {
      out = NVRAM(7, 11, -1);
    } else if (request->hasParam("email")) {
      out = NVRAM(12, 15, -1);
    } else {
      out = NVRAM(0, 6, 4);
    }
    request->send(200, text_json, out);
  });
  server.on("/nvram", HTTP_POST, [](AsyncWebServerRequest * request) {

    String out = "<pre>";
    uint8_t c = 0, from = 0, to = 0;
    uint8_t skip = -1;

    if (request->hasParam("WiFiMode")) {
      //skip confirm password (5)
      from = 0, to = 7, skip = 5;
    } else if (request->hasParam("WiFiDHCP")) {
      from = 7, to = 11;
    } else if (request->hasParam("WiFiNotify")) {
      from = 12, to = 16;
    }

    for (uint8_t i = from; i <= to; i++) {

      String v = request->getParam(c)->value();

      //Catch and encrypt passwords
      if (request->getParam(c)->name() == "EmailPassword") {
        v = encrypt(string2char(v), aes_iv);
#if DEBUG
        Serial.println(v);
#endif
      }

      if (skip == -1 || i < skip) {
        out += request->getParam(c)->name() + ": ";
        NVRAM_Write(i, v);
        out += NVRAM_Read(i) + "\n";
      } else if (i > skip) {
        out += request->getParam(c)->name() + ": ";
        NVRAM_Write(i - 1, v);
        out += NVRAM_Read(i - 1) + "\n";
      }
      c++;
    }

    out += "\n...Rebooting";
    out += "</pre>";

    AsyncWebServerResponse *response = request->beginResponse(200,  text_html, out);

    if (request->hasParam("WiFiIP")) { //IP has changed
      response->addHeader("Refresh", "10; url=http://" + request->getParam("WiFiIP")->value() + "/index.html");
    } else {
      response->addHeader("Refresh", "8; url=/index.html");
    }
    request->send(response);

    restartRequired = true;
    //ESP.restart();
  });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, text_html, serverIndex);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (Update.hasError()) {
      StreamString str;
      Update.printError(str);
      request->send(200,  text_plain, String("Update error: ") + str.c_str());
    } else {
      AsyncWebServerResponse *response = request->beginResponse(200,  text_html, "Update Success! Rebooting...");
      response->addHeader("Refresh", "15; url=/");
      request->send(response);

      restartRequired = true;
      //ESP.restart();
    }
  }, WebUpload);

  server.on("/snapshot.php", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200,  text_plain, "OK");
  }, SnapshotUpload);

  /*
    server.onFileUpload([](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){
      if(!index) // if index == 0 then this is the first frame of data
        Serial.printf("UploadStart: %s\n", filename.c_str());
        Serial.printf("%s", (const char*)data);
      if(final) // if the final flag is set then this is the last frame of data
        Serial.printf("UploadEnd: %s (%u)\n", filename.c_str(), index+len);
    });
  */
  server.on("/serial.php", HTTP_GET, [](AsyncWebServerRequest * request) {

    //NOTE: AsyncWebServer library does not allow delay or yield, but Serial.readString(); uses yield();

    AsyncResponseStream *response = request->beginResponseStream(text_plain);
    response->addHeader("Cache-Control", "no-store");

    char b[255];
    size_t len = 0;

    if (request->hasParam("init")) {

      String com = "";
      /*
        // SeeedStudio Library
        #define CAN_5KBPS    1
        #define CAN_10KBPS   2
        #define CAN_20KBPS   3
        #define CAN_25KBPS   4
        #define CAN_31K25BPS 5
        #define CAN_33KBPS   6
        #define CAN_40KBPS   7
        #define CAN_50KBPS   8
        #define CAN_80KBPS   9
        #define CAN_83K3BPS  10
        #define CAN_95KBPS   11
        #define CAN_100KBPS  12
        #define CAN_125KBPS  13
        #define CAN_200KBPS  14
        #define CAN_250KBPS  15
        #define CAN_500KBPS  16
        #define CAN_666kbps  17
        #define CAN_1000KBPS 18

        // Coryjfowler Library
        #define CAN_4K096BPS 0
        #define CAN_5KBPS    1
        #define CAN_10KBPS   2
        #define CAN_20KBPS   3
        #define CAN_31K25BPS 4
        #define CAN_33K3BPS  5
        #define CAN_40KBPS   6
        #define CAN_50KBPS   7
        #define CAN_80KBPS   8
        #define CAN_100KBPS  9
        #define CAN_125KBPS  10
        #define CAN_200KBPS  11
        #define CAN_250KBPS  12
        #define CAN_500KBPS  13
        #define CAN_1000KBPS 14
      */
      int canbus_kbps_init = request->getParam("canbus")->value().toInt();
      //int canbus_kbps[] = {0, 5, 10, 20, 25, 31, 33, 40, 50, 80, 83, 95, 100, 125, 200, 250, 500, 666, 1000}; // SeeedStudio Library
      int canbus_kbps[] = {4, 5, 10, 20, 31, 33, 40, 50, 80, 100, 125, 200, 250, 500, 1000}; // Coryjfowler Library

      for (int i = 0; i < sizeof(canbus_kbps); i++) {
        if (canbus_kbps[i] == canbus_kbps_init) {
          canbus_kbps_init = i;
          break;
        }
      }

      //if (CAN0.begin(canbus_kbps_init) == CAN_OK) // SeeedStudio Library
      if (CAN0.begin(MCP_STDEXT, canbus_kbps_init, MCP_8MHZ) == CAN_OK) // Coryjfowler Library
      {
        // SeeedStudio Library
        //CAN0.setMode(MODE_NORMAL);
        //CAN0.setMode(MODE_LISTENONLY);
        //-------------------
        // Coryjfowler Library
        CAN0.setMode(MCP_NORMAL);
        //CAN0.setMode(MCP_LISTENONLY);

        com  += "CAN" + String(canbus_kbps_init);
      }
      if (Serial) {
        Serial.end();
        Serial.begin(request->getParam("serial")->value().toInt(), SERIAL_8N1);
        com  += "Serial";
      }
      response->print(com);
      //request->send(200, text_plain, com);

    } else if (request->hasParam("get")) {
      String sz = request->getParam("get")->value();
      String out;

      if (sz.indexOf(",") != -1 )
      {
        char buf[sz.length() + 1];
        sz.toCharArray(buf, sizeof(buf));
        char *p = buf;
        char *str;
        while ((str = strtok_r(p, ",", &p)) != NULL) //split
        {
          Serial.print("get " + String(str));
          Serial.print('\n');
          while (Serial.read() != '\n'); //consume echo
          do {
            memset(b, 0, sizeof(b));
            len = Serial.readBytes(b, sizeof(b) - 1);
            response->printf("%s", b);
          } while (len > 0);
        }
      } else {
        Serial.print("get " + sz);
        Serial.print('\n');
        while (Serial.read() != '\n'); //consume echo
        do {
          memset(b, 0, sizeof(b));
          len = Serial.readBytes(b, sizeof(b) - 1);
          response->printf("%s", b);
        } while (len > 0);
      }

    } else if (request->hasParam("command")) {

      serialStreamFlush(); //flush

      Serial.print(request->getParam("command")->value());
      Serial.print('\n');
      while (Serial.read() != '\n'); //consume echo

      AsyncWebServerResponse *response = request->beginChunkedResponse(text_plain, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        return Serial.readBytes(buffer, maxLen);
      });
      response->addHeader("Cache-Control", "no-store");
      request->send(response);

    } else if (request->hasParam("stream")) {

      uint16_t _loop = request->getParam("loop")->value().toInt();
      uint16_t _delay = request->getParam("delay")->value().toInt();

      serialStreamFlush(); //flush

      Serial.print("get " + request->getParam("stream")->value());
      Serial.print('\n');
      while (Serial.read() != '\n'); //consume echo
      while (Serial.read() != '\n'); //consume first read

      AsyncWebServerResponse *response = request->beginResponse(text_plain, _loop + 1, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        Serial.print('!');
        Serial.read(); //consume "!"
        return Serial.readBytes(buffer, maxLen);
      });
      response->addHeader("Cache-Control", "no-store");
      request->send(response);
    } else {
      //DEBUG
      //request->send(200, text_plain, "v:8,b:8,n:8,i:8,p:10,ah:10,kwh:10,t:30*");

      serialStreamFlush(); //flush
      //Serial.print('\n');
      
      while (char c = Serial.read() != '\n') {
        response->printf("%c", c);
      }
    }
    request->send(response);
  });
  server.on("/opendbc/index.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    String ext[] = {".dbc"};
    String out = indexJSON("/opendbc", ext);
    request->send(200, text_json, out);
  });
  server.on("/opendbc/delete", HTTP_GET, [](AsyncWebServerRequest * request) {
    LittleFS.remove("/opendbc/" + request->getParam("file")->value());

    AsyncWebServerResponse *response = request->beginResponse(200, text_html, request->getParam("file")->value() + " file deleted from LittleFS");
    response->addHeader("Refresh", "3; url=/index.html");
    request->send(response);
  });
  server.on("/views/index.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    String ext[] = {".json"};
    String out = indexJSON("/views", ext);
    request->send(200, text_json, out);
  });
  server.on("/views/bg/index.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    String ext[] = {".jpg", ".png"};
    String out = indexJSON("/views/bg", ext);
    request->send(200, text_json, out);
  });
  server.on("/views/save.php", HTTP_POST, [](AsyncWebServerRequest * request) {

    File file = LittleFS.open("/views/" + request->getParam("view")->value(), "w");
    file.print(request->getParam("json")->value());
    file.close();
    request->send(200, text_plain, "");
  });

  server.on("/", [](AsyncWebServerRequest * request) {
    if (LittleFS.exists("/index.html")) {
      request->redirect("/index.html");
    } else {
      AsyncWebServerResponse *response = request->beginResponse(200, text_html, "File System Not Found ...");
      response->addHeader("Refresh", "6; url=/update");
      request->send(response);
    }
  });

  //server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest * request) {
    //Serial.println((request->method() == HTTP_GET) ? "GET" : "POST");

    String file = request->url(); //Serial.println("Request:" + file);

    digitalWrite(LED_BUILTIN, HIGH);

    if (LittleFS.exists(file))
    {
      String contentType = getContentType(file);

      AsyncWebServerResponse *response = request->beginResponse(LittleFS, file, contentType, request->hasParam("download"));

      if (contentType != text_json) {
        response->addHeader("Content-Encoding", "gzip");
      }
      request->send(response);

    } else {
      request->send(404, text_plain, "404: Not Found");
    }

    digitalWrite(LED_BUILTIN, LOW);
  });
  server.begin();

  //ArduinoOTA.begin();

  pinMode(LED_BUILTIN, OUTPUT);

  //====================
  //CAN-Bus
  //====================
  /*
    CAN bus @ 250 kbps is limited to a sample rate of 100 Hz
    1000 ms = 1 sec = 1 Hz
    100 ms = 0.1 sec = 10 Hz
    10 ms = 0.01 sec = 100 Hz
  */

  SPI.begin();
  //SPI.setClockDivider(SPI_CLOCK_DIV2);         // Set SPI to run at 8MHz (16MHz / 2 = 8 MHz)

  //if (CAN0.begin(CAN_250KBPS) == CAN_OK) // SeeedStudio Library
  if (CAN0.begin(MCP_STDEXT, CAN_250KBPS, MCP_8MHZ) == CAN_OK) // Coryjfowler Library
  {
#if DEBUG
    Serial.println("MCP2515 Initialized Successfully!");
#endif
    // SeeedStudio Library
    //CAN0.setMode(MODE_NORMAL);
    //CAN0.setMode(MODE_LISTENONLY);
    //-------------------
    // Coryjfowler Library
    CAN0.setMode(MCP_NORMAL);
    //CAN0.setMode(MCP_LISTENONLY);
  } else {
#if DEBUG
    Serial.println("Error Initializing MCP2515...");
#endif
  }
  serialStreamFlush(); //flush

  //attachInterrupt(0, MCP2515_ISR, FALLING);
  //pinMode(CAN0_INT, INPUT);
}

void loop()
{
  if (restartRequired) {
#if DEBUG
    Serial.println("Restarting ESP");
#endif
    restartRequired = false;
    delay(1000);
    WiFi.disconnect(true);  //Erases SSID/password
    //ESP.eraseConfig();
    ESP.restart();
  }

  if (DATA_LOG == 0 || (millis() - syncTime) < LOG_INTERVAL) return;
  syncTime = millis();

  FSInfo fs_info;
  LittleFS.info(fs_info);

  if (fs_info.usedBytes < fs_info.totalBytes)
  {
    if (CAN0.checkReceive() == CAN_MSGAVAIL) {
      unsigned char len = 0;
      //CAN0.readMsgBuf(&len, CANmsg); // SeeedStudio Library
      CAN0.readMsgBuf(&CANmsgId, &len, CANmsg); // Coryjfowler Library
      logFile.printf("%s", CANmsg);
    }
    if (Serial.available()) {
      logFile.print(Serial.readStringUntil('\n'));
    }
  } else {
    logFile.close();
    LittleFS.remove("/log.txt");
  }

  //server.handleClient();
  //ArduinoOTA.handle();
  //yield();
}

char* string2char(String command) {
  if (command.length() != 0) {
    char *p = const_cast<char*>(command.c_str());
    return p;
  }
}

StreamString CANReceive()
{
  StreamString CANMessage;

  if (CAN0.checkReceive() == CAN_MSGAVAIL)
  {
    unsigned char len = 0;

    //===================
    // SeeedStudio Library
    //===================
    //CAN0.readMsgBuf(&len, CANmsg);
    //CANmsgId = CAN0.getCanId(); //Must be called AFTER CAN0.readMsgBuff, otherwise will get the last CAN ID, not the current value.

    //===================
    // Coryjfowler Library
    //===================
    CAN0.readMsgBuf(&CANmsgId, &len, CANmsg);

    //if (CAN0.isExtendedFrame()) // SeeedStudio Library
    if ((CANmsgId & 0x80000000) == 0x80000000) // Coryjfowler Library
    {
      CANMessage.printf("Extended ID:0x%.8lX  DLC:%1d  Data:", (CANmsgId & 0x1FFFFFFF), len);
    } else {
      CANMessage.printf("Standard ID:0x%.3lX  DLC:%1d  Data:", CANmsgId, len);
    }
    /*
      //if (CAN0.isRemoteRequest()) // SeeedStudio Library
      if ((CANmsgId & 0x40000000) == 0x40000000) // Coryjfowler Library
      {
      CANMessage.print(" REMOTE REQUEST FRAME");
      //CANMessage.print(CANReceive()); //Try next message
      }
    */
    for (byte i = 0; i < len; i++) {
      //CANMessage.printf("%d", CANmsg[i]);
      CANMessage.printf("0x%.2X ", CANmsg[i]);
    }
#ifdef DEBUG
    Serial.println(CANMessage);
#endif

    /*
      if (CANmsgId == 0) { // ignore.  Always receives msg with CANmsgID = 0 the first time.

      }else if((rxId == 0x0CF12507) && (rxBuf[2] > 0))
      {
          int msgOffset = 0;
          float msgFactor = 0.01;
          float fReturn = getFloatFromCanMsg(0, 8, msgOffset, msgFactor);
      }
    */

    /*
      int msgOffset = 0;
      float msgFactor = 0.01;
      updateCanMsgFromFloat(myFloatVal, 0, 16, msgOffset, msgFactor);
      CAN0.sendMsgBuf(CANmsgId, 1, 8, CANmsg);
    */
  } else {
    if (CAN0.checkError() == CAN_CTRLERROR) {
      CANMessage.println(CAN0.getError());
    }
    //CANMessage.println("No CAN Message Available");
  }
  return CANMessage;
}

/*
  http://www.savvysolutions.info/savvymicrocontrollersolutions/arduino.php?article=adafruit-ultimate-gps-shield-seeedstudio-can-bus-shield
*/
//****************************************************************

// These functions are written around the concept of using at
// least two bytes to represent a float or integer value.
// This allows you to send up to four separate float or integer
// values within a CAN message.

float getFloatFromCanMsg(int startBit, int msgLen, int offset, float factor) {
  // Read the data from msg[8] within CANmsg[]
  // convert it with the passed parameters, and return a float value.
  // Assumes msgLen = 16
  byte msb;
  byte lsb;
  switch (startBit) {
    case 0:
      msb = CANmsg[0];
      lsb = CANmsg[1];
      break;
    case 16:
      msb = CANmsg[2];
      lsb = CANmsg[3];
      break;
    case 32:
      msb = CANmsg[4];
      lsb = CANmsg[5];
      break;
    case 48:
      msb = CANmsg[6];
      lsb = CANmsg[7];
      break;
  }
  int myInt = (lsb << 8) | msb;
#if DEBUG
  Serial.print("getFloatFromCanMsg myInt = ");
  Serial.println(myInt);
#endif
  // float CANbusIntToFloat(unsigned int myInt, int offset, float factor) {
  float myFloat = CANbusIntToFloat(myInt, offset, factor);
  return myFloat;
}

unsigned int floatToIntCANbus(float myFloat, int offset, float factor) {
  // float myFloat = 2128.5;
  // unsigned int myInt = floatToIntCANbus(myFloat, 0, 0.125);
  //
  // Beginning with float of 2128.5, convert to CAN signal
  // values.
  // (int val) = ((float val) - offset) / factor;
  // (int val) = ((2128.5) - 0.0) / 0.125;
  // (int val) = 17028

  // Common offset & factor values for msgLen = 16 (two bytes):
  // Temperature in C:  offset=-273; factor=0.03125
  // Percent (0 to 100%):  offset=-125; factor=1
  // speed (0 to 5000 rpm):  offset=0; factor=0.125
  // torque (Nm):  offset=0; factor=1
  // mass flow (kg/h):  offset=0; factor=0.2
  // pressure (kPa):  offset=0; factor=4;
  // Boolean (0/1):  offset=0; factor=1;

  myFloat = myFloat - (float)offset;
  myFloat = myFloat / factor;
  unsigned int myInt = (unsigned int) myFloat;
  return myInt;
}

void updateCanMsgFromFloat(float floatVal, int startBit, int msgLen, int offset, float factor) {
  // Update msg[8] within CANmsg[] with the passed parameters
  // Assumes msgLen = 16;
  // unsigned int floatToIntCANbus(float myFloat, int offset, float factor) {
  unsigned int myInt = floatToIntCANbus(floatVal, offset, factor);
  byte msb = getMsbFromInt(myInt);
  byte lsb = getLsbFromInt(myInt);
  switch (startBit) {
    case 0:
      CANmsg[0] = msb;
      CANmsg[1] = lsb;
      break;
    case 16:
      CANmsg[2] = msb;
      CANmsg[3] = lsb;
      break;
    case 32:
      CANmsg[4] = msb;
      CANmsg[5] = lsb;
      break;
    case 48:
      CANmsg[6] = msb;
      CANmsg[7] = lsb;
      break;
  }
}

float CANbusIntToFloat(unsigned int myInt, int offset, float factor) {
  // value in decimal = (CAN DEC value) * factor + offset
  // 17500 * 0.125 + 0 = 2187.5 rpm

  float myFloat = (float) myInt * factor + (float) offset;
  return myFloat;
}

byte getMsbFromInt(int myInt) {
  // int myInt = 17028;
  // byte msb = getMsbFromInt(myInt);
  byte msb = myInt & 0xff;
  return msb;
}

byte getLsbFromInt(int myInt) {
  // int myInt = 17028;
  // byte lsb = getLsbFromInt(myInt);
  byte lsb = (myInt >> 8) & 0xff;
  return lsb;
}

//=============
// NVRAM CONFIG
//=============
String NVRAM(uint8_t from, uint8_t to, uint8_t skip)
{
  String out = "{\n";

  for (uint8_t i = from; i <= to; i++) {
    if (skip == -1 || i != skip) {
      out += "\t\"nvram" + String(i) + "\": \"" + NVRAM_Read(i) + "\",\n";
    }
  }

  out = out.substring(0, (out.length() - 2));
  out += "\n}";

  return out;
}

void NVRAM_Erase()
{
  for (uint16_t i = 0 ; i < EEPROM.length() ; i++) {
    EEPROM.write(i, 255);
  }
}

void NVRAM_Write(uint32_t address, String txt)
{
  char arrayToStore[32];
  memset(arrayToStore, 0, sizeof(arrayToStore));
  txt.toCharArray(arrayToStore, sizeof(arrayToStore)); // Convert string to array.

  EEPROM.put(address * sizeof(arrayToStore), arrayToStore);
  EEPROM.commit();
}

String NVRAM_Read(uint32_t address)
{
  char arrayToStore[32];
  EEPROM.get(address * sizeof(arrayToStore), arrayToStore);

  return String(arrayToStore);
}

String indexJSON(String dir, String ext[])
{
  String out = "{\n\t\"index\": [\n";

  Dir files = LittleFS.openDir(dir);
  while (files.next()) {
    for (int i = 0; i < sizeof(ext); i++) {
      if (files.fileName().endsWith(ext[i])) {
        out += "\t\t\"" + files.fileName() + "\",\n";
      }
    }
  }

  out = out.substring(0, (out.length() - 2));
  out += "\t]\n}";

  return out;
}

String getContentType(String filename)
{
  if (filename.endsWith(".htm")) return text_html;
  else if (filename.endsWith(".html")) return text_html;
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".json")) return text_json;
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".jpg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".svg")) return "image/svg+xml";
  else if (filename.endsWith(".csv")) return "text/csv";
  else if (filename.endsWith(".ttf")) return "font/ttf";
  else if (filename.endsWith(".woff")) return "font/woff";
  else if (filename.endsWith(".woff2")) return "font/woff2";
  return text_plain;
}

//===============
//Web OTA Updater
//===============
void WebUpload(AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  if (!index) {
    //Serial.print(request->params());

    if (filename == "flash-littlefs.bin") {
      //if (request->hasParam("filesystem",true)) {
      size_t fsSize = ((size_t) &_FS_end - (size_t) &_FS_start);
#if DEBUG
      Serial.printf("Free LittleFS Space: %u\n", fsSize);
      Serial.printf("LittleFS Flash Offset: %u\n", U_FS);
#endif
      close_all_fs();
      if (!Update.begin(fsSize, U_FS)) { //start with max available size
        Update.printError(Serial);
      }
    } else {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000; //calculate sketch space required for the update
#if DEBUG
      Serial.printf("Free Scketch Space: %u\n", maxSketchSpace);
#endif
      if (!Update.begin(maxSketchSpace, U_FLASH)) { //start with max available size
        Update.printError(Serial);
      }
    }
    Update.runAsync(true); // tell the updaterClass to run in async mode
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
  }

  if (final) {
    if (!Update.end(true)) {
      Update.printError(Serial);
    }
  }
}

void SnapshotUpload(AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  String uploadPath = filename;
  if (filename.endsWith(".json")) {
    uploadPath = "/views/" + filename;
  } if (filename.endsWith(".dbc")) {
    uploadPath = "/opendbc/" + filename;
  }

  if (!index) {
    LittleFS.remove(uploadPath);
  }

  File fsUpload = LittleFS.open(uploadPath, "a");
  fsUpload.write(data, len);
  fsUpload.close();
}

void serialStreamFlush()
{
  char b[255];
  size_t len = 0;
  uint8_t timeout = 10;

  do {
    memset(b, 0, sizeof(b));
    len = Serial.readBytes(b, sizeof(b) - 1);
    timeout--;
  } while (len > 0 && timeout > 0);
}
