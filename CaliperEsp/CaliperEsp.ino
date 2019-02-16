/*
*	CaliperEsp.ino
*	Copyright (c) 2019 Bob Tidey
*
*	Hardware based on instructable by Jonathan Mackey
*   https://www.instructables.com/id/Caliper-Data-Interface/
*	
*	Collects data from calipers using the clock and data interface
*   This is a 24 bit data word sent every 250 mSec
*   A gap of > 50mSec is used to indicate start of next data word
*/

//set to 1 to generate caliper values for testing without calipers
#define TEST 0

#define ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include "FS.h"
#include <DNSServer.h>
#include <WiFiManager.h>

//put -1 s at end
int unusedPins[11] = {0,1,2,15,16,-1,-1,-1,-1,-1,-1};

#define CALIPER_CLOCK 13
#define CALIPER_DATA 14
#define CALIPER_POWER 4
#define GPIO_HOLD 5
#define GPIO_BUTTON 12
#define CLOCK_EDGE FALLING
#define DATA_INVERT 1

/*
 Manual Web set up
*/
#define AP_SSID "ssid"
#define AP_PASSWORD "password"
#define AP_MAX_WAIT 10
#define AP_PORT 80

//uncomment next line to use static ip address instead of dhcp
//#define AP_IP 192,168,0,200
#define AP_DNS 192,168,0,1
#define AP_GATEWAY 192,168,0,1
#define AP_SUBNET 255,255,255,0

String macAddr;

/*
Wifi Manager Web set up
If WM_NAME defined then use WebManager
*/
#define WM_NAME "caliperSetup"
#define WM_PASSWORD "password"
#ifdef WM_NAME
	WiFiManager wifiManager;
#endif

#define CONFIG_FILE "/caliperConfig.txt"

#define CALIPER_GAP 100
#define CALIPER_BITCOUNT 24
#define TIME_INTERVAL 500

#define BUTTON_SHORT 100
#define BUTTON_MEDIUM 2000
#define BUTTON_LONG 4000

#define dfltADC_CAL 0.976
float adcCal = dfltADC_CAL;
float battery_mult = 1220.0/220.0/1024;//resistor divider, vref, max count
float battery_volts;

#define BUTTON_IDLE 0
#define BUTTON_DOWN 1
#define BUTTON_UP 2
#define BUTTON_SHORT 100
#define BUTTON_MEDIUM 2000
#define BUTTON_LONG 4000

#define WIFI_CHECK_TIMEOUT 30000
int timeInterval = TIME_INTERVAL;
unsigned long elapsedTime;
unsigned long wifiCheckTime;
unsigned long caliperChangeTime;
unsigned long  caliperIntTime = 0;
unsigned long noChangeTimeout = 60000;
unsigned long buttonOnTime = 0;
unsigned long buttonOffTime = 0;
long buttonShort = BUTTON_SHORT;
long buttonMedium = BUTTON_MEDIUM;
long buttonLong = BUTTON_LONG;
int volatile buttonState = 0;
int measureCount = 0;
int volatile caliperCapture = 0;
int volatile caliperBitCount = 0;
int caliperMaxBitCount = CALIPER_BITCOUNT;
int volatile caliperAcc = 0;
int volatile caliperData = 0;
int caliperChangeData = 0;
int enableSleep = 0;

//Measurements
#define MEASURES_MAX 16
#define DFLT_MEASUREFILE "measuresDefault.txt"
#define DFLT_MEASUREFILE_PREFIX "measures"
String measureFilename = DFLT_MEASUREFILE;
String measureFilenamePrefix = DFLT_MEASUREFILE_PREFIX;
String measureNames[MEASURES_MAX];
int measureValues[MEASURES_MAX];
int measureIndex = 0;
int measuredIndex = 0;
int measuredValue = 0;

//For update service
String configNames[] = {"host","noChangeTimeout","enableSleep","timeInterval","adcCal","buttonShort","buttonMedium","buttonLong","measureFileName","measureFileNamePrefix"};
String host = "caliper";
const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "password";

ESP8266WebServer server(AP_PORT);
ESP8266HTTPUpdateServer httpUpdater;

//holds the current upload
File fsUploadFile;

HTTPClient cClient;

int wifiConnect(int check);
void initFS();
String getContentType(String filename);
bool handleFileRead(String path);
void handleFileUpload();
void handleFileDelete();
void handleFileCreate();
void handleFileList();

/*
  Caliper clock interrupt handler
*/
void ICACHE_RAM_ATTR caliperClockInterrupt() {
	unsigned long m;
	int b;
	m = millis();
	if(((m-caliperIntTime) > CALIPER_GAP)) {
		//reset data capture
		caliperCapture = 1;
		caliperBitCount = 0;
		caliperAcc = 0;
	}
	if(caliperCapture) {
		b = digitalRead(CALIPER_DATA) ^ DATA_INVERT;
		caliperAcc = caliperAcc >> 1 | (b ? 0x00800000 : 0);
		caliperBitCount++;
		if(caliperBitCount >= caliperMaxBitCount) {
			caliperData = caliperAcc;
			caliperCapture = 0;
		}
	}
	caliperIntTime = m;
}

/*
  Button Push interrupt handler
*/
void ICACHE_RAM_ATTR buttonInterrupt() {
	unsigned long m = millis();
	if(digitalRead(GPIO_BUTTON)) {
		buttonState = BUTTON_DOWN;
		buttonOnTime = m;
	} else {
		if(buttonState == BUTTON_DOWN) {
			buttonOffTime = m;
			buttonState = BUTTON_UP;
		}
	}
}

void ICACHE_RAM_ATTR  delaymSec(unsigned long mSec) {
	unsigned long ms = mSec;
	while(ms > 100) {
		delay(100);
		ms -= 100;
		ESP.wdtFeed();
	}
	delay(ms);
	ESP.wdtFeed();
	yield();
}

void ICACHE_RAM_ATTR  delayuSec(unsigned long uSec) {
	unsigned long us = uSec;
	while(us > 100000) {
		delay(100);
		us -= 100000;
		ESP.wdtFeed();
	}
	delayMicroseconds(us);
	ESP.wdtFeed();
	yield();
}

void unusedIO() {
	int i;
	
	for(i=0;i<11;i++) {
		if(unusedPins[i] < 0) {
			break;
		} else if(unusedPins[i] != 16) {
			pinMode(unusedPins[i],INPUT_PULLUP);
		} else {
			pinMode(16,INPUT_PULLDOWN_16);
		}
	}
}

/*
  Get config
*/
String getConfig() {
	String line = "";
	String strConfig;
	String strName;
	int config = 0;
	File f = SPIFFS.open(CONFIG_FILE, "r");
	if(f) {
		while(f.available()) {
			line =f.readStringUntil('\n');
			line.replace("\r","");
			if(line.length() > 0 && line.charAt(0) != '#') {
				switch(config) {
					case 0: host = line;break;
					case 1: noChangeTimeout = line.toInt();break;
					case 2: enableSleep = line.toInt();break;
					case 3: timeInterval = line.toInt();;break;
					case 4: adcCal = line.toFloat();;break;
					case 5: buttonShort = line.toInt();;break;
					case 6: buttonMedium = line.toInt();break;
					case 7: buttonLong = line.toInt();break;
					case 8: measureFilename = line;break;
					case 9: measureFilenamePrefix = line;
						Serial.println(F("Config loaded from file OK"));
						break;
				}
				strConfig += configNames[config] + ":" + line + "<BR>";
				config++;
			}
		}
		f.close();
		//enforce minimum no change of 60 seconds
		if(noChangeTimeout < 60000) noChangeTimeout = 60000;
		Serial.println("Config loaded");
		Serial.print(F("host:"));Serial.println(host);
		Serial.print(F("noChangeTimeout:"));Serial.println(noChangeTimeout);
		Serial.print(F("enableSleep:"));Serial.println(enableSleep);
		Serial.print(F("timeInterval:"));Serial.println(timeInterval);
		Serial.print(F("adcCal:"));Serial.println(adcCal);
		Serial.print(F("buttonShort:"));Serial.println(adcCal);
		Serial.print(F("buttonMedium:"));Serial.println(adcCal);
		Serial.print(F("buttonLong:"));Serial.println(adcCal);
		Serial.print(F("measureFileName:"));Serial.println(adcCal);
		Serial.print(F("measureFilenamePrefix:"));Serial.println(adcCal);
	} else {
		Serial.println(String(CONFIG_FILE) + " not found. Use default encoder");
	}
	return strConfig;
}



/*
  Connect to local wifi with retries
  If check is set then test the connection and re-establish if timed out
*/
int wifiConnect(int check) {
	if(check) {
		if((elapsedTime - wifiCheckTime) * timeInterval > WIFI_CHECK_TIMEOUT) {
			if(WiFi.status() != WL_CONNECTED) {
				Serial.println("Wifi connection timed out. Try to relink");
			} else {
				wifiCheckTime = elapsedTime;
				return 1;
			}
		} else {
			return 0;
		}
	}
	wifiCheckTime = elapsedTime;
#ifdef WM_NAME
	Serial.println(F("Set up managed IRBlaster Web"));
	wifiManager.setConfigPortalTimeout(180);
	#ifdef AP_IP
		wifiManager.setSTAStaticIPConfig(IPAddress(AP_IP), IPAddress(AP_GATEWAY), IPAddress(AP_SUBNET));
	#endif
	wifiManager.autoConnect(WM_NAME, WM_PASSWORD);
	WiFi.mode(WIFI_STA);
#else
	Serial.println(F("Set up manual IRBlaster Web"));
	int retries = 0;
	Serial.print(F("Connecting to AP"));
	#ifdef AP_IP
		IPAddress addr1(AP_IP);
		IPAddress addr2(AP_DNS);
		IPAddress addr3(AP_GATEWAY);
		IPAddress addr4(AP_SUBNET);
		WiFi.config(addr1, addr2, addr3, addr4);
	#endif
	WiFi.begin(AP_SSID, AP_PASSWORD);
	while (WiFi.status() != WL_CONNECTED && retries < AP_MAX_WAIT) {
		delaymSec(1000);
		Serial.print(F("."));
		retries++;
	}
	Serial.println("");
	if(retries < AP_MAX_WAIT) {
		Serial.print(F("WiFi connected ip "));
		Serial.print(WiFi.localIP());
		Serial.printf_P(PSTR(":%d mac %s\r\n"), AP_PORT, WiFi.macAddress().c_str());
		return 1;
	} else {
		Serial.println(F("WiFi connection attempt failed"));
		return 0;
	} 
#endif
}

void initFS() {
	if(!SPIFFS.begin()) {
		Serial.println(F("No SIFFS found. Format it"));
		if(SPIFFS.format()) {
			SPIFFS.begin();
		} else {
			Serial.println(F("No SIFFS found. Format it"));
		}
	} else {
		Serial.println(F("SPIFFS file list"));
		Dir dir = SPIFFS.openDir("/");
		while (dir.next()) {
			Serial.print(dir.fileName());
			Serial.print(F(" - "));
			Serial.println(dir.fileSize());
		}
	}
}

String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path){
  Serial.printf_P(PSTR("handleFileRead: %s\r\n"), path.c_str());
  if(path.endsWith("/")) path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload(){
  if(server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    Serial.printf_P(PSTR("handleFileUpload Name: %s\r\n"), filename.c_str());
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    Serial.printf_P(PSTR("handleFileUpload Data: %d\r\n"), upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    Serial.printf_P(PSTR("handleFileUpload Size: %d\r\n"), upload.totalSize);
  }
}

void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.printf_P(PSTR("handleFileDelete: %s\r\n"),path.c_str());
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.printf_P(PSTR("handleFileCreate: %s\r\n"),path.c_str());
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");
  Serial.printf_P(PSTR("handleFileList: %s\r\n"),path.c_str());
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  output += "]";
  server.send(200, "text/json", output);
}

void handleMinimalUpload() {
  char temp[700];

  snprintf ( temp, 700,
    "<!DOCTYPE html>\
    <html>\
      <head>\
        <title>ESP8266 Upload</title>\
        <meta charset=\"utf-8\">\
        <meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">\
        <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
      </head>\
      <body>\
        <form action=\"/edit\" method=\"post\" enctype=\"multipart/form-data\">\
          <input type=\"file\" name=\"data\">\
          <input type=\"text\" name=\"path\" value=\"/\">\
          <button>Upload</button>\
         </form>\
      </body>\
    </html>"
  );
  server.send ( 200, "text/html", temp );
}

void handleSpiffsFormat() {
	SPIFFS.format();
	server.send(200, "text/plain", "format complete");
}

//Convert value to string, include units if required
String getCaliperString(int data, int units) {
	float fVal;
	String strVal;
	String unit;
	fVal = data & 0xfffff;
	if(data & 0x100000) fVal = -fVal;
	if(data > 8388607) {
		strVal = String(fVal/2000);
		unit = " in";
	} else {
		strVal = String(fVal/100);
		unit = " mm";
	}
	if(units)
		strVal += unit;
	return strVal;
}

//Convert string to value using units
int getCaliperValue(String str) {
	int index;
	int data = 0;
	float fVal;
	int unit = 0; //0=mm, 1=in
	
	index = str.indexOf(' ');
	if(index > 1) {
		fVal = str.substring(0,index).toFloat();
		if(str.substring(index+1) == "in") {
			unit = 1;
		}
		if(unit) {
			data = fVal * 2000;
		} else {
			data =int(fVal * 100);
		}
		if(data < 0) data = 0x100000 - data;
		data = data + unit * 8388608;
	}
	return data;
}

//load measures file
String loadMeasures(String filename) {
	String line = "";
	String measureName;
	String strMeasures;
	int index;
	int ix=0;

	if(filename.length()) {
		File f = SPIFFS.open("/" + filename, "r");
		if(f) {
			while(f.available()) {
				line = f.readStringUntil('\n');
				line.replace("\r","");
				if(line.length() > 0 && line.charAt(0) != '#' && ix < MEASURES_MAX) {
					index = line.indexOf(':');
					if(index > 0) {
						measureName = line.substring(0, index);
						measureName.trim();
						if(measureName.length() == 0) measureName = "measure" + String(ix+1);
						measureNames[ix] = measureName;
						measureValues[ix] = getCaliperValue(line.substring(index+1));
						strMeasures += measureNames[ix] + ":" + line.substring(index+1) + "<BR>";
						ix++;
					}
				}
			}
			f.close();
			measureFilename = filename;
		} else {
			Serial.println(filename + " not found.");
		}
	}

	for(;ix < MEASURES_MAX;ix++) {
		strMeasures += measureNames[ix] + ":" + getCaliperString(measureValues[ix], 1) + "<BR>";
	}
	if(filename.length()) measureIndex = 0;
	return strMeasures;
}

//save measures file
//mode 0 use existing values
//mode 1 import new measures and clear out any undefined values
//mode 2 import new measures and leave any undefined values 
void saveMeasures(String filename, String strMeasures, int mode) {
	File f = SPIFFS.open("/" + filename, "w");
	int ix;
	int index0 = 0, index1 = 0, index2;
	String measure;

	if(f) {
		for(ix = 0; ix < MEASURES_MAX; ix++) {
			index1 = strMeasures.indexOf("<BR>", index0);
			if(mode && index1 > 0) {
				measure = strMeasures.substring(index0, index1);
				index2 = measure.indexOf(":");
				if(index2 > 0) {
					measureNames[ix] = measure.substring(0, index2);
					measureNames[ix].trim();
					if(measureNames[ix].length() == 0) measureNames[ix] = "measure" + String(ix+1);
					measureValues[ix] = getCaliperValue(measure.substring(index2+1));
					f.print(measureNames[ix] + ":" + measure.substring(index2+1) + "\n");
				}
				index0 = index1 + 4;
			} else {
				if(mode == 1) {
					measureNames[ix] = "measure" + String(ix+1);
					measureValues[ix] = 0;
				}
				f.print(measureNames[ix] + ":" + getCaliperString(measureValues[ix],1) + "\n");
			}
		}
		f.close();
	}
}

//switch module into deep sleep
void powerOff() {
	Serial.println("Action power off");
	if(enableSleep) {
		//save current values
		saveMeasures(measureFilename, "", 0);
		WiFi.mode(WIFI_OFF);
		delaymSec(10);
		WiFi.forceSleepBegin();
		delaymSec(1000);
		pinMode(GPIO_HOLD, INPUT);
		ESP.deepSleep(0);
	}
}

//power off then on the calipe runit
void handleZero() {
	digitalWrite(CALIPER_POWER, 0);
	delaymSec(1000);
	digitalWrite(CALIPER_POWER, 1);
	server.send(200, "text/plain", "caliper zeroed");
}

//action request to reload config
void handleLoadConfig() {
	server.send(200, "text/html", getConfig());
}

//action request to save config
void handleSaveConfig() {
	File f;
	String config = server.arg("config");
	config.replace("<BR>","\n");
	f = SPIFFS.open(CONFIG_FILE, "w");
	if(f) {
		f.print(config);
		f.close();
		getConfig();
		server.send(200, "text/plain", "config saved");
	} else {
		server.send(200, "text/plain", "error saving config");
	}
}

//action request to load measures file and return results
void handleLoadMeasures() {
	String filename = server.arg("filename");
	String measures;
	server.send(200, "text/html", loadMeasures(filename));
}

//action request to save measures file
void handleSaveMeasures() {
	String filename = server.arg("filename");
	String measures  = server.arg("measures");
	if(filename.length() > 0) {
		measureFilename = filename;
	} else {
		measureFilename = DFLT_MEASUREFILE;
	}
	saveMeasures(filename, measures, 1);
	server.send(200, "text/plain", "measures saved");
}

//action setting measureindex
void handleSetMeasureIndex() {
	int index = server.arg("index").toInt();
	if(index < 0) index = 0;
	if(index >= MEASURES_MAX) index = MEASURES_MAX - 1;
	measureIndex = index;
	server.send(200, "text/plain", "measureIndex set");
}

//action setting measureindex
void handleGetMeasureFiles() {
	String fileList;
	String filename;
	Dir dir = SPIFFS.openDir("/");
	while (dir.next()) {
		filename = dir.fileName();
		if(filename.indexOf(measureFilenamePrefix) == 1) {
			fileList += filename.substring(1) + "<BR>";
		}
	}
	server.send(200, "text/html", fileList);
}

//send response to status request
void handleStatus() {
	String status = "MeasureFilePrefix:" + String(measureFilenamePrefix) +"<BR>";
	status += "Raw:" + String(caliperData) +"<BR>";
	status += "CurrentValue:" + getCaliperString(caliperData, 1) +"<BR>";
	status += "MeasuredIndex:" + String(measuredIndex) +"<BR>";
	status += "MeasuredValue:" + getCaliperString(measuredValue, 1) +"<BR>";
	battery_volts = battery_mult * adcCal * analogRead(A0);
	status += "BatteryVolts:" +String(battery_volts) + "<BR>";
	status += "MeasureIndex:" + String(measureIndex) +"<BR>";
	status += "MeasureFile:" + measureFilename +"<BR>";
	status += "MeasureCount:" + String(measureCount) +"<BR>";
	server.send(200, "text/html", status);
}

//action power request
void handlePower() {
	int pwr;
	pwr = server.arg("onOff").toInt();
	digitalWrite(CALIPER_POWER, pwr);
	server.send(200, "text/plain", "power onOff processed");
}

void measureStep() {
	if(TEST) caliperData = elapsedTime & 0x3fff;
	measureValues[measureIndex] = caliperData;
	measuredValue = caliperData;
	measuredIndex = measureIndex;
	measureIndex++;
	measureCount++;
	Serial.println("measure step ix:" + String(measureIndex) +" v:" + String(measuredValue));
	if(measureIndex >= MEASURES_MAX) measureIndex = 0;
}

void checkButton() {
	long diff = buttonOffTime - buttonOnTime;
	if(buttonState == BUTTON_UP) {
		buttonState = BUTTON_IDLE;
		if(diff > buttonLong) {
			Serial.println("button long");
			powerOff();
		} else if(diff > buttonMedium) {
			Serial.println("button medium");
			measureIndex--;
			if(measureIndex < 0) measureIndex = MEASURES_MAX - 1;
		}  else if(buttonOffTime - buttonOnTime > buttonShort) {
			Serial.println("button short");
			//do take a measurement and step on
			measureStep();
		}
		caliperChangeTime = elapsedTime;
	}
}

void checkForChange() {
	int diff = caliperData - caliperChangeData;
	if(diff > 20 || diff <  -20) {
		caliperChangeData = caliperData;
		caliperChangeTime = elapsedTime;
	} else if((elapsedTime - caliperChangeTime) * timeInterval > noChangeTimeout) {
		caliperChangeTime = elapsedTime;
		powerOff();
	}
}

/*
 Initialise wifi, message handlers and ir sender
*/
void setup() {
	unusedIO();
	Serial.begin(115200);
	pinMode(CALIPER_CLOCK, INPUT_PULLUP);
	pinMode(CALIPER_DATA, INPUT_PULLUP);
	pinMode(CALIPER_POWER, OUTPUT);
	pinMode(GPIO_HOLD, OUTPUT);
	pinMode(GPIO_BUTTON, INPUT);
	digitalWrite(GPIO_HOLD, 1);
	digitalWrite(CALIPER_POWER,1);
	Serial.println(F("Set up filing system"));
	initFS();
	getConfig();
	wifiConnect(0);
	macAddr = WiFi.macAddress();
	macAddr.replace(":","");
	Serial.println(macAddr);
	//Update service
	Serial.println(F("Set up Web update service"));
	MDNS.begin(host.c_str());
	httpUpdater.setup(&server, update_path, update_username, update_password);
	MDNS.addService("http", "tcp", AP_PORT);

	Serial.println(F("Set up Web command handlers"));
	//Simple upload
	server.on("/upload", handleMinimalUpload);
	//SPIFFS format
	server.on("/format", handleSpiffsFormat);
	server.on("/list", HTTP_GET, handleFileList);
	//load editor
	server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");});
	//create file
	server.on("/edit", HTTP_PUT, handleFileCreate);
	//delete file
	server.on("/edit", HTTP_DELETE, handleFileDelete);
	//first callback is called after the request has ended with all parsed arguments
	//second callback handles file uploads at that location
	server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload);
	server.on("/status",  handleStatus);
	server.on("/power",  handlePower);
	server.on("/zero",  handleZero);
	server.on("/loadconfig", handleLoadConfig);
	server.on("/saveconfig", handleSaveConfig);
	server.on("/loadmeasures", handleLoadMeasures);
	server.on("/savemeasures", handleSaveMeasures);
	server.on("/setmeasureindex", handleSetMeasureIndex);
	server.on("/getmeasurefiles", handleGetMeasureFiles);

	//called when the url is not defined here
	//use it to load content from SPIFFS
	server.onNotFound([](){if(!handleFileRead(server.uri())) server.send(404, "text/plain", "FileNotFound");});
	server.begin();
	attachInterrupt(CALIPER_CLOCK, caliperClockInterrupt, CLOCK_EDGE);
	attachInterrupt(GPIO_BUTTON, buttonInterrupt, CHANGE);
}

void loop() {
	server.handleClient();
	checkForChange();
	checkButton();
	delaymSec(timeInterval);
	elapsedTime++;
	wifiConnect(1);
}
