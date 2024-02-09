//------------------------------------------------------------------
// ESP32 IoT Hub
//
// January 2024, Deangi
//
// Set up an ESP32 with attached SD card to be an IoT hub to
// allow collection of data from sensors and storing it and
// forwarding it from the IoT "edge" to a remote server.
//
// Iot data can be received by the hub from several input channels:
// 1) via an FTP server (allowing IoT device with WiFi connection to upload it's data)
// 2) via an HTTP post (allowing IoT device with WiFi connection to upload it's data)
// 3) Future: via additional sources such as serial port, IIC, SPI, LoRa, Bluetooth
//
// Example use cases (could be a combination of these)
//
// Use case 1: Tiny IoT relay 
//   - In this case there is an existing WiFi signal to connect with.
//     - The IotHub server connects to this as a WiFi Station
//     - IOT devices can use the Iot Hub server to upload and store results
//     - Basically similar to having a raspberry PI FTP server, but this
//       will use about 1/2 watt of power.
//   - External servers can download IoT data via FTP when convenient
// Use case 2: Tiny WiFi Hub - AP with FTP server
//   - In this case there is no existing WiFi signal to connect with.
//     - The Iot Hub server initializes as a WiFi AP for other devices
//       to connect with
//     - IOT devices can connect directly to this AP and upload results
//     - Useful in very power limited situations to store and forward
//       IOT data
//     - this FTP server uses 1 watt or less.
//   - External servers can download IoT data via FTP when convenient
// Future - LoRa, BTLE, serial support - receive data from these sources
//          and send to the IoT Hub log file(s)
//
// Scenarios: Remote monitoring - limited power scenarios
//   Sensors periodically take measurements and record them.
//   On some periodic basis (once per day, every hour, ... depending
//   on power availability) the sensors connect to WiFi and either
//   FTP/Append results or HTTP/POST results to this server.
//
//   These results are stored on the SD card.   Then the results
//   can later be retrieved by some server via FTP or directly from
//   the SD card.
//
//   For example the IoT Hub can collect data at some remote location
//   over a period of weeks or months.   Then the results can be
//   retrieved via a period site visit - say a truck, 'copter, or 
//   even a drone visit monthly.
//
// Follow on project ideas:
// 1) Bluetooth, Bluetooth LE gateway that would gather data
//    from BT connected sensors and forward it to the IoT HUB
// 2) LoRa gateway that would gather data from LoRa connected
//    sensors and forward it to the IoT HUB
//
//------------------------------------------------------------------
// Board, software, and IDE information
//
// Credits: 
//   FTP Server code -> https://github.com/dplasa/FTPClientServer/
//
// Uses an inexpensive ESP32 card with an attached SD card reader
// to allow FTP clients to connect and upload/download files on
// the SD card.
//
// Board: Search for Aideepen ESP32-CAM on Amazon for details
//        This board comes with a camera, but it's not used
//        in this application - it can be physically removed.
//
// Uses: Arduino IDE with ESP32 Express IF board support package
//
// Board selection: M5Stack-Timer-Cam from M5Stack-Arduino
// Partition selection: Default  (3MB no OTA/1MB SPIFFS)
//
// ESP32 resource usage:
// 1) CPU - full speed 240 MHz - assume we're not on battery power
// 2) ESP32 I/O - SPI connection to SD card
//        - documentation says up to 4GB supported, but I've used Samsung 32GB Ulta A1 HC
//
// -- GPIO allocation from the schematic I saw:
//
// Camera: 34,35,32,25,26,27,23,22,21,19,18,5,0
// Led   : 4 (Shared with SD card for some reason)
// SD    : 14,12,4,2,15,13
// GPIO 16 - spare
//
// Configuration file note:
//
//SSID=WiFiSSID
//PASSWORD=WiFiPassword
//FTPUSER=ftpuser
//FTPPASSWORD=ftppwd
//MODE=AP
//MODE=STA
//------------------------------------------------------------------------
// V1.1 - support AP mode as well as STA mode, add simple web server
// 
// TODO:
// - In STATION mode, if WiFi gets disconnected, try to reconnect
// - Come up with some standard regarding logging format to identify sensor and other meta data


#include "Arduino.h"
#include "driver/rtc_io.h"      // RTC interface
#include <ESP32Time.h>          // RTC time functions
#include <SPI.h>                // SPI driver code
#include "SD_MMC.h"             // SD_MMC card library
#include <WiFi.h>               // WiFi driver code
#include <WiFiUdp.h>            // UDP code for NTPClient support
#include <NTPClient.h>          // Network Time Protocol (NTP) client library

#define DEBUG_ESP_PORT Serial
#include "FTPServer1.h"

//-----------------------------------------------------------------
// real time clock (software based, not backed up for power failures
// Subject to some drift!!!
ESP32Time rtc(-8*3600);  // -8 from GMT by default

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
int needNtp = true;

// file system
fs::FS & fileSystem = SD_MMC; // file system
FTPServer ftpSrv(SD_MMC);

//-------------------------------------------
// File names 
// --- configuration file ---
#define CONFIGFN "/config.ini"
// --- log file of IoTHub activities ---
#define LOGFN "/iothub.log"
// --- IoT data from HTTP POST gets put here
#define IOTFN "/iotdata.log"

// sign on message - for serial port
#define SIGNON "\nIoT HUB V1.1\n"

// configuration file data
char ssid[64];
char password[64];
char ftpuser[64];
char ftppwd[64];
int isApMode; // true means AP mode, false means STATION mode

int wifiWaitCounter;
int wifiIsConnected = false;
String ipAddress = String("unknown");
WiFiServer server(80);

// Error Codes
#define ERR_NOCARD (1)
#define ERR_NOWIFI (2)

//-------------------------------------------------------------
// blink an error code
//-------------------------------------------------------------
#define SIGNALLED (4)
#define LEDON pinMode(SIGNALLED,OUTPUT); digitalWrite(SIGNALLED,HIGH)
#define LEDOFF pinMode(SIGNALLED,OUTPUT); digitalWrite(SIGNALLED,LOW)

void PRINTLN(char* s) { Serial.println(s); }
void PRINTLN(String s) { Serial.println(s); }
void PRINT(char* s) { Serial.print(s); }
void PRINT(String s) { Serial.print(s); }

void blink(int code)
{
  PRINT("Error ");
  Serial.println(code);
  
  if (code < 0) code = 0;
  if (code > 10) code = 10;
  
  int dly=100; // ms
  for (;;) // fatal error code, never return from this
  {
    LEDON; delay(dly);
    LEDOFF; delay(3*dly);
    LEDON; delay(3*dly);
    LEDOFF; delay(3*dly);
    LEDON; delay(dly);
    LEDOFF; delay(6*dly);
    for (int i = 0; i < code; i++)
    {
      LEDON; delay(2*dly);
      LEDOFF; delay(2*dly);    
    }
    delay(10*dly);
  }
}

//--------------------------------------------------
// Append a line to the log file
void appendToLogFile(String fn, String msg)
{
  PRINTLN(msg);

  // append to log file
  File fout = fileSystem.open(fn, FILE_APPEND);
  if (!fout)
  {
    PRINTLN("Unable to append to log file");
  }
  else
  {
    fout.println(msg);
    fout.close();
  }
}

//----------------------------------------------------------------------
// Compose a log line with a leading time-tag
void addLogLine(String fn, char* msg)
{
  char buf[128];
  String ttag = rtc.getTime("%Y/%m/%d,%H:%M:%S"); // "2022/11/16,18:22:01"
  sprintf(buf,"%s,%s",ttag.c_str(), msg);
  appendToLogFile(fn, buf); 
}


//----------------------------------------------------------------------
void getNtpTime()
{
  if (wifiIsConnected==0) return; // can't do it if no wifi connected.
  // NTP Client
  timeClient.begin();
  timeClient.setTimeOffset(0);
  int waitingForNtp = 10;
  while (waitingForNtp--)
  {
    if (!timeClient.update())
    {
      timeClient.forceUpdate();
      delay(500);
    }
    else
      break;
  }

  unsigned long epochTime = timeClient.getEpochTime();
  PRINTLN("NTP Time: ");
  PRINTLN(timeClient.getFormattedDate());
  rtc.setTime(epochTime);
  addLogLine(LOGFN, "NTP");
}


//----------------------------------------------------------
// read line from input text file
int readln(File finp, uint8_t* buf, int maxlen)
{
  // return true on successful read, false on EOF
  // 10 or 13 (LF, CR) or both are EOL indicators
  int len=0;
  int eof=false;

  buf[0]=0;
  while (len<(maxlen-1))
  {
    if (!finp.available())
    {
      eof=true;
      break;
    }
    char c = finp.read();
    if (c < 0) 
    {
      eof=true;
      break; // EOF
    }
    if (c==13) continue; // ignore CR
    if (c==10) break; // end-of-line
    buf[len++]=c;
  }
  buf[len]=0; // null terminate
  return !eof;
}

//----------------------------------------------------------
// retrieve a value for a key in the config file
void readKey(char* configFn, char* key, char* outbuf, int maxlen)
{
  outbuf[0] = 0; // returning null string on error 
  //
  // Config file is key=value format
  // SSID=mywifi
  // PASSWORD=mypassword
  // TIMEZONE=-8
  // OFFSET=123590 
  //
  // pass in key with trailing = sign!!! 
  // readKey("/test.cfg","MYKEY=", outbuf, 127);

  File finp = fileSystem.open(CONFIGFN, FILE_READ);
  if (!finp)
  {
    PRINTLN("Unable to read config file");
    return;
  }
  // scan file and look for key
  char buf[128];
  int n = strlen(key);
  while (readln(finp, (uint8_t*) buf, 127))
  {
    if (strncmp(buf,key,n) == 0) // found
    { 
      PRINTLN(buf);
      strncpy(outbuf,&buf[n],maxlen);
      break;
    }
  }
  finp.close();
}




//------------------------------------------------------------------------
// convert IP address to string
String IpAddress2String(const IPAddress& ipAddress)
{
  return String(ipAddress[0]) + String(".") +\
  String(ipAddress[1]) + String(".") +\
  String(ipAddress[2]) + String(".") +\
  String(ipAddress[3])  ; 
}

//----------------------------------------------------------------------
// Connect to some WiFi access point as a station
int connectToWiFi()
{
 // Connect to Wi-Fi, 1=connected, 0=not connected
  int maxWaitTimeToConnect = 50; // seconds
  WiFi.begin(ssid, password);
  wifiWaitCounter=0;
  wifiIsConnected=0;
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(1000);
    PRINTLN("Connecting to WiFi..");
    if (++wifiWaitCounter >= maxWaitTimeToConnect)
      break;
  }

  // Print ESP32 Local IP Address
  if (wifiWaitCounter >= maxWaitTimeToConnect)
  {
    addLogLine(LOGFN, "WiFi connect failed");  
  }
  else
  {
    ipAddress = IpAddress2String(WiFi.localIP());
    String tmp = "WIFICONNECT,"+ipAddress;
    addLogLine(LOGFN, (char*)tmp.c_str());
    wifiIsConnected=1;
  }
  return wifiIsConnected;
}


//-----------------------------------------------------
// Setup - called once on boot-up
void setup() 
{
  char buf[32];
  
  Serial.begin(115200);
  Serial.println(SIGNON);

  pinMode(SIGNALLED,OUTPUT);
  
  // Mount SD card file system
  if(!SD_MMC.begin()) 
  {
    PRINTLN("SD_MMC Card Mount Failed");
    blink(ERR_NOCARD);
  }
  else
  {
    uint8_t cardType = SD_MMC.cardType();
  
    if(cardType == CARD_NONE) 
    {
      PRINTLN("No SD card attached");
      blink(ERR_NOCARD);
    }
    else
    {
      PRINT("Mounted SD_MMC card successfully, type is ");

      if(cardType == CARD_MMC){
        PRINTLN("MMC");
      } else if(cardType == CARD_SD){
        PRINTLN("SDSC");
      } else if(cardType == CARD_SDHC){
        PRINTLN("SDHC");
      } else {
        PRINTLN("UNKNOWN CARD TYPE");
      }
    }
  }

  // read configuration file
  readKey(CONFIGFN, "SSID=", ssid, 63);
  readKey(CONFIGFN, "PASSWORD=", password, 63);  
  readKey(CONFIGFN, "FTPUSER=", ftpuser, 63);
  readKey(CONFIGFN, "FTPPASSWORD=", ftppwd, 63);
  readKey(CONFIGFN, "MODE=", buf, 31); // MODE=AP or MODE=STA, station mode by default
  isApMode = (strcmp(buf,"AP") == 0);
  Serial.println(isApMode ? "AP MODE" : "STATION MODE");
  
  // initialize the RTC 
  rtc.setTime(12,0,0,1,1,2024); // default time 12:00:00 1/1/2024

  // see what mode we're operating in and initialize WiFi accordingly
  if (isApMode)
  {
    // Access point mode - 192.168.4.1 - will DHCP up to 4 or 5 clients at once
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("Access Point IP address: ");
    Serial.println(IP);
    ipAddress = IpAddress2String(IP);
  }
  else
  {
    // WiFi station mode - connect to an access point
    // on first boot, try to connect to WiFi and get time/date from NTP
    connectToWiFi();
    if (!connectToWiFi) blink(ERR_NOWIFI); // no point in continuing here
    
    needNtp = true;
    getNtpTime(); // get NTP time if possible to set date/time properly
  }

  PRINTLN("Starting Web service");
  server.begin(); // start web server
  
  PRINT("Initializing FTP server - user "); 
  PRINT(ftpuser);
  PRINT(" password ");
  PRINTLN(ftppwd);
  ftpSrv.begin(ftpuser,ftppwd); // start FTP server
}


WiFiClient client;
int webClientConnected = false;

//-----------------------------------------------------
// output home page to a connected client
void replyWithHomePage(WiFiClient client)
{
  // HTTP/1.1 header sent back to client
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println("Connection: close");
  client.println();
  // Display the HTML web page
  client.println("<!DOCTYPE html><html>");
  client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<link rel=\"icon\" href=\"data:,\">");
  
  // Web Page Heading
  client.println("<body><h1>Iot Hub edge server</h1>");

  uint64_t cardSize = SD_MMC.cardSize();
  int cardSizeInMB = cardSize/(1024 * 1024);
  client.printf("<p>Card size  : %d MB</p>", cardSizeInMB);

  uint64_t bytesAvailable = SD_MMC.totalBytes(); 
  int bytesAvailableInMB = bytesAvailable/(1024 * 1024);
  client.printf("<p>available: %d MB</p>", bytesAvailableInMB);

  uint64_t bytesUsed = SD_MMC.usedBytes();
  int bytesUsedInMB = bytesUsed/(1024 * 1024);
  client.printf("<p>used: %d MB</p>", bytesUsedInMB);
  
  client.println(rtc.getTime("<p>At %Y/%m/%d,%H:%M:%S</p>"));
  
  client.print("<p>"); 
  client.print(SIGNON); 
  client.print(" on "); 
  client.print(ipAddress); 
  client.println("</p>");
  
  client.println("</body></html>");
  
  // The HTTP response ends with another blank line
  client.println();
  // Break out of the while loop
}

//-----------------------------------------------------
// Convert hex digit to binary
int fromHex(char c)
{
  if ((c >= '0') && (c <= '9')) return (int)(c - '0');
  if ((c >= 'A') && (c <= 'F')) return (int)(c-'A')+10;
  if ((c >= 'a') && (c <= 'f')) return (int)(c-'a')+10;
  return 0;
}

//-----------------------------------------------------
// deHex - remove hex notation from http request
//    URLs have non-alpha-numerics encoded in a hex
//    'escape' sequence that is a % character and then
//    2 hex digits.   In this routine we input such
//    a URL encoded string and un-escape it.
void deHex(char* p)
{
  // p="123%20ABC on input, on output p="123 ABC"
  char* outp = p;
  int safectr = 255;
  char c;
  while ((c = *p++) && (safectr-- > 0))
  {
    if (c == '%')
    {
       // %xx in hex
       c = *p++; safectr--;
       if (!c) break;
       int vc = fromHex(c) << 4;
       c = *p++; safectr--;
       if (!c) break;
       vc = (vc + fromHex(c)) & 0x7f;
       // must be a printable char to be logged
       if ((vc >= ' ') && (vc <= '~')) *outp++ = (char)vc;
    }
    else
    {
      if ((c >= ' ') && (c <= '~')) *outp++ = c;
    }
  }
  *outp = '\0';
}

//-----------------------------------------------------
// process a received http header
//
// The header looks something like this:
// GET /?LOG=123%20ABC HTTP/1.1
void processHeader(char* hdr)
{
  // GET /?LOG=123%20ABC HTTP/1.1
  // Expect one parameter, LOG with all printable ASCII characters 0x20 .. 0x7e
  // if there is a LOG= parameter, extract it and either append to the iotdata.log
  // file or process it if it's a time and date set command (starts with a ~)
  //Serial.println("Got header");
  //Serial.println(hdr);
  char * p = strstr(hdr,"LOG=");
  if (p)
  {
    char *s = strstr(p," ");
    if (s != NULL)
    {
      *s='\0'; // null terminate, p="LOG=123%20ABC"
      p+=4; // p="123%20ABC"
      deHex(p);
      //Serial.println(p);
      addLogLine(IOTFN,p);
      if (*p == '~')  // set local time ~20240112201101, leading ~ is mandatory ~yyyymmddhhmmss, UTC manditory
      {
        p++;
        // 01234567890123
        // yyyymmddhhmmss
        int dsec = atoi(&p[12]); p[12]='\0';
        int dmin = atoi(&p[10]); p[10]='\0';
        int dhr  = atoi(&p[ 8]); p[ 8] ='\0';
        int dday = atoi(&p[ 6]); p[ 6] ='\0';
        int dmon = atoi(&p[ 4]); p[ 4] ='\0';
        int dyr  = atoi(p);
        // sanity check
        if (
          ((dyr >= 2024) && (dyr <= 2099)) &&
          ((dmon >=  1) && (dmon <= 12)) &&
          ((dday >=  1) && (dday <= 31)) &&
          ((dhr  >=  0) && (dhr  <= 23)) &&
          ((dmin >=  0) && (dmin <= 59)) &&
          ((dsec >=  0) && (dsec <= 59))
          )
          {
            rtc.setTime(dsec,dmin,dhr,dday,dmon,dyr);
            Serial.println(rtc.getDateTime());
            addLogLine(LOGFN,"Set time");
          }
          else
          {
            Serial.printf("Set time failed %d %d %d %d %d %d\n",dyr,dmon,dday,dhr,dmin,dsec);
          }
      }
    }
  }
}

// If an HTTP client connects, we can't just wait forever
// for the request to come in - so we periodically poll to
// see if characters have come in and store them until we
// have a full line.   When a full line is received, process
// it. 
#define HTTPHEADERMAXLEN (256)
char httpHeader[HTTPHEADERMAXLEN];
int httpHeaderPtr = 0;
long httpScanCounter = 0; // to time-out a connection
#define MAXHTTPSCANCOUNTER (200000) /* about 20000 per second */

//-----------------------------------------------------
// loop() - called forever after setup() is called
void loop() 
{
  for (;;)
  {
    // sort of a crude round-robin scheduler
    ftpSrv.handleFTP();

    if (webClientConnected)
    {
      // a client has connected, and we're collecting
      // received characters until we get end-of-line
      // then process the received line
      
      if (httpScanCounter++ >= MAXHTTPSCANCOUNTER)
      {
        webClientConnected = false;
        if (client)
        {
          client.stop();
        }
        Serial.println("\nForced client disconnect");
      }
      // a client is connected, try to read the request line
      // request line will be something like this:
      // GET /?var1=val1 HTTP/1.1
      //    GET /?LOG=123%20ABC HTTP/1.1
      //   ?var1=val1&var2=val2 ,,,
      //   values have embedded hex for some characters like  "1%202" means "1 2"
      //   basically any char that isn't alpha/numeric is %xx
      //
      else if (!client)
      {
        webClientConnected = false;
        client.stop(); // client unexpectedly disconnected
        //Serial.println("\nunexpected client disconnect");
      }
      else if (client.available())
      {
        // here we poll to see if a character has come
        // in from the web client
        
        char c = client.read();
        //Serial.write(c); // echo for debugging
        if (c == '\n')
        {
          //Serial.println();
          processHeader(httpHeader);
          replyWithHomePage(client);
          client.flush();
          client.stop();
          webClientConnected = false;
          //Serial.println("Web client disco");
        }
        else if (httpHeaderPtr < (HTTPHEADERMAXLEN-1))
        {
          httpHeader[httpHeaderPtr++] = c;
          httpHeader[httpHeaderPtr] = '\0';
        }
      }
    }
    else
    {
      // at the present time no client is connected,
      // see if one is knocking on our door
      client = server.available();
      if (client) 
      {
        webClientConnected = true;
        httpHeader[0] = '\0';
        httpHeaderPtr = 0;
        httpScanCounter = 0;
        Serial.println("Web client connected");
      }
    }
  }
}
