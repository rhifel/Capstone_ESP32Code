#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include "payload_struct.h"
#include <Wire.h>
#include <Adafruit_SH110X.h>
#include <TinyGPS++.h>

// oled object and variables
constexpr uint8_t SCREEN_WIDTH  = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;
constexpr uint8_t OLED_RESET    = -1;
constexpr uint8_t OLED_ADDR     = 0x3C;
Adafruit_SH1106G oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// gps obejct, uart, and variable
HardwareSerial GPSSerial(2);  
TinyGPSPlus gps;
constexpr uint8_t RX_PIN = 16;
constexpr uint8_t TX_PIN = 17;

// nrf24 object, device address, variables, and pins
constexpr uint8_t CE_PIN  = 4;
constexpr uint8_t CSN_PIN = 5;
RF24 radio(CE_PIN, CSN_PIN);
const uint8_t HND_ADDR[6] = "HND01";
const uint8_t BSE_ADDR[6] = "BSE01";
constexpr uint8_t H_ID = 1;

// buttons and led pins
constexpr uint8_t BUTTON_R = 25;
constexpr uint8_t BUTTON_B = 26;
constexpr uint8_t BUTTON_G = 27;
constexpr uint8_t BUTTON_SC = 32;
constexpr uint8_t LED_G    = 33;

// button and status arrays
const uint8_t buttons[]  = {BUTTON_R, BUTTON_G, BUTTON_B};
const uint8_t statuses[] = {STATUS_RED, STATUS_GREEN, STATUS_BLUE};
const size_t numButtons  = sizeof(buttons)/sizeof(buttons[0]);

// variable helpers
static uint16_t msg_counter = 1; 
const uint16_t DEBOUNCE_MS = 200;
uint8_t lastScButtonState = HIGH; //screen button state
uint8_t lastStButtonState[numButtons] = {HIGH,HIGH,HIGH}; //status button state

// separate debounce times for the buttons
static uint32_t lastPressTime[numButtons] = {0,0,0}; // status buttons
static uint32_t lastScreenPressTime = 0;

// reponse from base for LAST RECV and checkForResponse
bool recvResponse = false;
uint16_t lastRecvMsgID = 0;
uint8_t lastRecvResponse = 0;

// last sent status for LAST SENT and sendSOS
bool sentPkt = false;
uint16_t lastSentMsgID = 0;
uint8_t lastSentStatus = 0;

// time snapshots for LAST SENT
bool lastSentTimeValid = false;
uint8_t lastSentHour, lastSentMinute, lastSentSecond;
uint8_t lastSentDay, lastSentMonth;
uint16_t lastSentYear;

// time snapshots for LAST RECV
bool lastRecvTimeValid = false;
uint8_t lastRecvHour, lastRecvMinute, lastRecvSecond;
uint8_t lastRecvDay, lastRecvMonth;
uint16_t lastRecvYear;

// popup handling when receiving response
bool showRecvPopup = false;
uint32_t recvPopupStart = 0;
const uint32_t POPUP_MS = 3000;

// button confirmation
bool isWaitingConfirm = false;
uint8_t pendingStatus = 0;
uint32_t confirmTimeout = 0;
const uint16_t CONFIRM_WINDOW_MS = 5000; // 5 seconds to confirm button send

// state lock
bool waitingForResponse = false;
uint8_t currentStatus = 0;
unsigned long sendTime = 0;
const unsigned long LOCK_TIMEOUT = 10000; // 10 secs

// TDC update
uint32_t lastTDCUpdate = 0;
const uint16_t TDC_REFRESH_MS = 1000; // 1 sec
// tempscreen; for internal UI
bool tempScreenActive = false;
uint32_t tempScreenStart = 0;
uint16_t tempScreenDuration = 0;

// non blocking for LED and buzzer 
struct BlinkState {
    bool active = false;
    uint8_t pin;
    uint8_t togglesLeft;
    uint32_t lastTime;
    uint16_t interval;
    bool state;
} blink;
uint8_t ledMode = 0;

void startBlink(uint8_t pin, uint8_t times, uint16_t interval){
    pinMode(pin, OUTPUT);

    blink.active = true;
    blink.pin = pin;
    blink.togglesLeft = times * 2;
    blink.interval = interval;
    blink.lastTime = millis();
    blink.state = false;
}

void updateBlink(){
    if(!blink.active) return;

    uint32_t now = millis();
    if(now - blink.lastTime >= blink.interval){
        blink.lastTime = now;

        blink.state = !blink.state;
        digitalWrite(blink.pin, blink.state);

        blink.togglesLeft--;

        if(blink.togglesLeft == 0){
            blink.active = false;
            digitalWrite(blink.pin, LOW);
        }
    }
}

void setLEDMode(uint8_t mode) {
    blink.active = false;
    digitalWrite(LED_G, LOW);

    ledMode = mode;

    switch (mode) {
        case 0: break;
        case 1: startBlink(LED_G, 999, 500); break;
        case 2: startBlink(LED_G, 1, 400); break;
        case 3: startBlink(LED_G, 999, 200); break;
    }
}

// priority function 
bool isHigherPriority(uint8_t newStatus, uint8_t currentStatus){
    if(currentStatus == 0) return true;
    return newStatus < currentStatus;
}

// OLED helper functions
void oledPrintAt(const char* msg, int x, int y, int textSize = 1, bool clear = false){
    if(clear) oled.clearDisplay();
    oled.setTextSize(textSize);
    oled.setCursor(x, y);
    oled.print(msg);
}

void oledPrintCenter(const char* msg, int textSize = 1, bool clear = false){
    int cX = SCREEN_WIDTH / 2;
    int cY = SCREEN_HEIGHT / 2;

    int textWidth  = strlen(msg) * 6 * textSize;
    int textHeight = 8 * textSize;

    int posX = cX - textWidth / 2;
    int posY = cY - textHeight / 2;

    oledPrintAt(msg, posX, posY, textSize, clear);
}

void oledPrintCenterY(const char* msg, int y, int textSize = 1){
    int textWidth = strlen(msg) * 6 * textSize;
    int x = (SCREEN_WIDTH - textWidth) / 2;

    oled.setTextSize(textSize);
    oled.setCursor(x, y);
    oled.print(msg);
}
// function for the internal UI; non-blocking
void showTempMessage(const char* line1, const char* line2, uint16_t duration){
    if(duration == 0) duration = 1000;

    oled.clearDisplay();
    oledPrintCenterY(line1, 20, 2);
    if(line2) oledPrintCenterY(line2, 40, 1);
    oled.display();

    tempScreenActive = true;
    tempScreenStart = millis();
    tempScreenDuration = duration;
}

const char* statusToStr(uint8_t s) {
    switch (s) {
        case 1: return "ALERT";
        case 2: return "AID";
        case 3: return "SAFE";
        default: return "UNKNOWN";
    }
}

const char* statusToMsgResp(uint8_t r){
    switch(r){
        case 1: return "RECEIVED";
        case 2: return "ON THE WAY";
        case 3: return "WAIT";
        case 4: return "BUSY";
        default: return "UNKNOWN";
    }
}

// enums for statemachine homescreen
// TDC is Time, Date, and Coordinates
// LS and LR are Last Sent and Received
// GD is GUIDE for ALERT | AID | SAFE
enum HomeScreenState { 
    SCREEN_TDC, 
    SCREEN_LR, 
    SCREEN_LS,
    SCREEN_GD}; 
HomeScreenState homeState = SCREEN_TDC; 

const uint8_t daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

bool isLeapYear(uint16_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

void applyTimeOffset(uint8_t &hour, uint8_t &day, uint8_t &month, uint16_t &year, uint8_t offset) {
    hour += offset;
    while(hour >= 24){
        hour -= 24;
        day++;
        
        uint8_t maxDay = daysInMonth[month - 1];
        if (month == 2 && isLeapYear(year)) maxDay = 29;

        if (day > maxDay) {
            day = 1;
            month++;
            if (month > 12) {
                month = 1;
                year++;
            }
        }
    }
}

// 1st screen
void oledShowTDC() {
    oled.clearDisplay();

    if (!gps.time.isValid() || !gps.date.isValid() || !gps.location.isValid()) {
        oledPrintCenterY("ACQUIRING", 28, 2);
        oled.display();
        return;
    }

    uint8_t hour   = gps.time.hour();
    uint8_t minute = gps.time.minute();
    uint8_t day    = gps.date.day();
    uint8_t month  = gps.date.month();
    uint16_t year  = gps.date.year();

    // UTC+8
    applyTimeOffset(hour, day, month, year, 8);

    // Convert to 12hr format
    const char* ampm = (hour >= 12) ? "PM" : "AM";
    uint8_t hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;

    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d %s", hour12, minute, ampm);

    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d", month, day, year);

    char coordBuf[24];
    snprintf(coordBuf, sizeof(coordBuf), "%.5f, %.5f", gps.location.lat(), gps.location.lng());

    oledPrintCenterY(dateBuf, 4, 1);
    oledPrintCenterY(timeBuf, 16, 2);
    oledPrintCenterY("LAT , LON", 36, 1);
    oledPrintCenterY(coordBuf, 44, 1);

    oled.display();
}

// 2nd
void oledShowLS() {
    oled.clearDisplay();
    char timeBuf[16];
    char idBuf[8];
    char dateBuf[16];
    
    oledPrintAt("SENT:", 0, 0, 1);

    if (sentPkt && lastSentTimeValid) {
        uint8_t hour   = lastSentHour;
        uint8_t minute = lastSentMinute;
        uint8_t second = lastSentSecond;
        uint8_t day    = lastSentDay;
        uint8_t month  = lastSentMonth;
        uint16_t year  = lastSentYear;

        applyTimeOffset(hour, day, month, year, 8);

        const char* ampm = (hour >= 12) ? "PM" : "AM";
        uint8_t hour12 = hour % 12;
        if (hour12 == 0) hour12 = 12;

        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d%s", hour12, minute, second, ampm);
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d", month, day, year);

        int timeWidth = strlen(timeBuf) * 6;
        int dateWidth = strlen(dateBuf) * 6;
        int xTime = (SCREEN_WIDTH - timeWidth - dateWidth - 4) / 2;
        int xDate = xTime + timeWidth + 4;

        oledPrintAt(timeBuf, xTime, 10, 1);
        oledPrintAt(dateBuf, xDate, 10, 1);

        // status
        oledPrintCenterY(statusToStr(lastSentStatus), 30, 2);

        // id label and value
        snprintf(idBuf, sizeof(idBuf), "ID:%d", lastSentMsgID);
        oledPrintCenterY(idBuf, 50, 1);
    } else {
        oled.clearDisplay();
        oledPrintCenterY("AWAITING", 20, 2);
        oledPrintCenterY("INPUT", 40, 2);
    }
    
    oled.display();
}

//  3rd
void oledShowLR() {
    oled.clearDisplay();
    char timeBuf[16];
    char idBuf[8];
    char dateBuf[16];

    oledPrintAt("RECEIVED:", 0, 0, 1);

    if (recvResponse && lastRecvTimeValid) {
        uint8_t hour   = lastRecvHour;
        uint8_t minute = lastRecvMinute;
        uint8_t second = lastRecvSecond;
        uint8_t day    = lastRecvDay;
        uint8_t month  = lastRecvMonth;
        uint16_t year  = lastRecvYear;

        applyTimeOffset(hour, day, month, year, 8);

        const char* ampm = (hour >= 12) ? "PM" : "AM";
        uint8_t hour12 = hour % 12;
        if (hour12 == 0) hour12 = 12;

        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d%s", hour12, minute, second, ampm);
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d", month, day, year);

        int timeWidth = strlen(timeBuf) * 6;
        int dateWidth = strlen(dateBuf) * 6;
        int xTime = (SCREEN_WIDTH - timeWidth - dateWidth - 4) / 2;
        int xDate = xTime + timeWidth + 4;

        oledPrintAt(timeBuf, xTime, 10, 1);
        oledPrintAt(dateBuf, xDate, 10, 1);

        // response message
        oledPrintCenterY(statusToMsgResp(lastRecvResponse), 25, 2);

        // id
        snprintf(idBuf, sizeof(idBuf), "ID:%d", lastRecvMsgID);
        oledPrintCenterY(idBuf, 50, 1);
    } else {
        oled.clearDisplay();
        oledPrintCenterY("AWAITING", 20, 2);
        oledPrintCenterY("MESSAGE", 40, 2);
    }

    oled.display();
}

// 4th
void oledShowGD(){
    oled.clearDisplay();

    oledPrintCenterY("GUIDE", 0, 1);

    oledPrintAt("ALERT", 5, 18, 1);
    oledPrintAt("AID",   5, 32, 1);
    oledPrintAt("SAFE",  5, 46, 1);

    oledPrintAt("- EMERGENCY", 55, 18, 1);
    oledPrintAt("- ASSIST",    55, 32, 1);
    oledPrintAt("- CLEAR",     55, 46, 1);

    oled.display();
}
// helper for switching screens
void refreshCurrentScreen() {
    switch (homeState) {
        case SCREEN_TDC:
            oledShowTDC();  break;
        case SCREEN_LS:
            oledShowLS();   break;
        case SCREEN_LR:
            oledShowLR();   break;
        case SCREEN_GD:
            oledShowGD();   break;
    }
}

void handleScreenButton(uint32_t now) {
    uint8_t currentButtonState = digitalRead(BUTTON_SC);

    if (currentButtonState == LOW && lastScButtonState == HIGH && (now - lastScreenPressTime > DEBOUNCE_MS)) {

        lastScreenPressTime = now;

        homeState = (HomeScreenState)((homeState + 1) % 4);
        refreshCurrentScreen();
    }
    
    lastScButtonState = currentButtonState;
}

// Check for incoming response from base
void checkForResponse(){
    uint8_t pipe;
    if(radio.available(&pipe) && pipe == 1){
        uint8_t payloadSize = radio.getDynamicPayloadSize(); 
        // trash invalic packet size
        if(payloadSize != sizeof(payload_t)){
            radio.flush_rx();
            radio.stopListening();
            radio.startListening();
            return;
        }

        payload_t resp; //response 
        radio.read(&resp, sizeof(resp));

        // Validate packet
        if (resp.type != PKT_RESPONSE) return;
        if (resp.handheld_id != H_ID) return;
        if (resp.msg_id != lastSentMsgID) return;

        lastRecvMsgID = resp.msg_id;
        lastRecvResponse = resp.response_code;  
        
        // receivedTime snapshot
        if(gps.time.isValid() && gps.date.isValid()){
        lastRecvHour   = gps.time.hour();
        lastRecvMinute = gps.time.minute();
        lastRecvSecond = gps.time.second();

        lastRecvDay    = gps.date.day();
        lastRecvMonth  = gps.date.month();
        lastRecvYear   = gps.date.year();

        lastRecvTimeValid = true;
        } else{
            lastRecvTimeValid = false;
        }

        recvResponse = true;
        waitingForResponse = false;

        setLEDMode(2);
        showRecvPopup = true;
        recvPopupStart = millis();
        tempScreenActive = false;
    }
}

// recv popup UI
void drawResponsePopup(uint16_t id) {
    char idBuf[10];

    oled.clearDisplay();

    oledPrintCenterY("NEW", 10, 2);
    oledPrintCenterY("MESSAGE", 30, 2);

    snprintf(idBuf, sizeof(idBuf), "ID:%d", id);
    oledPrintCenterY(idBuf, 50, 1);

    oled.display();
}

bool sendSOS(uint8_t status){
    if(!gps.location.isValid() || !gps.time.isValid() || !gps.date.isValid()){
        return false;
    } 

    payload_t pkt = {0};

    pkt.year       = gps.date.year();
    pkt.month      = gps.date.month();
    pkt.day        = gps.date.day();
    pkt.daySeconds = (uint32_t)gps.time.hour() * 3600 + 
                     (uint32_t)gps.time.minute() * 60 + 
                     (uint32_t)gps.time.second();

    pkt.type            = PKT_STATUS;
    pkt.handheld_id     = H_ID;
    pkt.latitude        = (int32_t)(gps.location.lat()*1e7);
    pkt.longitude       = (int32_t)(gps.location.lng()*1e7);
    pkt.status          = status;
    pkt.msg_id          = msg_counter;
    pkt.response_code   = 0;   // added for the PKT_RESPONSE

    radio.stopListening();
    bool ack = radio.write(&pkt, sizeof(pkt));
    radio.startListening();

    
    msg_counter++;
    lastSentStatus  = status;   // ALERT / SAFE / AID
    sentPkt         = true;
    lastSentMsgID   = pkt.msg_id;

    // sentTime snapshot
    lastSentHour   = (uint8_t)(pkt.daySeconds / 3600);
    lastSentMinute = (uint8_t)((pkt.daySeconds / 60) % 60);
    lastSentSecond = (uint8_t)(pkt.daySeconds % 60);
    lastSentDay    = pkt.day;
    lastSentMonth  = pkt.month;
    lastSentYear   = pkt.year;
    lastSentTimeValid = (pkt.year != 0);

    if(ack){
        setLEDMode(2);  // blink if there is autoack 
    } 
    return true;
}

void handleStatusButtons(uint32_t now) {
    for (size_t i = 0; i < numButtons; i++) {
        uint8_t currentButtonState = digitalRead(buttons[i]);
        // check for press + debounce
        if (currentButtonState == LOW && lastStButtonState[i] == HIGH && (now - lastPressTime[i] > DEBOUNCE_MS)) {
            lastPressTime[i] = now;

            // the 2nd press 
            if(isWaitingConfirm && pendingStatus != 0){
                if(statuses[i] == pendingStatus){ 
                    // match, turn off LED, send packet
                    setLEDMode(0);
                    //sendSOS(pendingStatus);
                    if(waitingForResponse){
                        if(!isHigherPriority(pendingStatus, currentStatus)){
                            int remaining = (LOCK_TIMEOUT - (now - sendTime)) / 1000;
                            if(remaining < 0) remaining = 0;
                            char retryBuf[20];

                            snprintf(retryBuf, sizeof(retryBuf), "%ds", remaining);
                            showTempMessage("RETRY IN", retryBuf, 800);

                            isWaitingConfirm = false;
                            return;
                        }
                    }
                    
                    bool success = sendSOS(pendingStatus);
                    isWaitingConfirm = false; // reset the confirmation state
    
                    if(success){
                        waitingForResponse = true;
                        currentStatus = pendingStatus;
                        sendTime = now;
                        setLEDMode(0);

                        showTempMessage("SENDING...", statusToStr(pendingStatus), 1000);
                    }else{
                        setLEDMode(0);
                        showTempMessage("FAILED", "TO SEND", 800);
                    }

                } else{ // user presses a different button
                    isWaitingConfirm = false;
                    setLEDMode(0);
                    showTempMessage("CANCELLED", nullptr, 800);
                }
            }else{ // first press
                isWaitingConfirm = true;
                pendingStatus = statuses[i];
                confirmTimeout = now;
                setLEDMode(3); // confirm warning

                oled.clearDisplay();
                oledPrintCenterY("CONFIRM SEND?", 5, 1);
                oledPrintCenterY(statusToStr(pendingStatus), 20, 2);
                oledPrintCenterY("Press the same button", 45, 1);
                oledPrintCenterY("to send...", 55, 1);
                oled.display();
            }
        }
        lastStButtonState[i] = currentButtonState;
    }
    if(isWaitingConfirm && (now - confirmTimeout > CONFIRM_WINDOW_MS)){
        isWaitingConfirm = false;
        pendingStatus = 0;
        confirmTimeout = 0;
        setLEDMode(0);
        refreshCurrentScreen();
    }
}


// setup
void setup() {
    Serial.begin(115200);
    Wire.begin();
    GPSSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
    oled.begin(OLED_ADDR,true);


    //Wire.setClock(400000);
    oled.clearDisplay();
    oled.setTextColor(SH110X_WHITE);

    pinMode(BUTTON_R, INPUT_PULLUP);
    pinMode(BUTTON_G, INPUT_PULLUP);
    pinMode(BUTTON_B, INPUT_PULLUP);
    pinMode(BUTTON_SC, INPUT_PULLUP);
    pinMode(LED_G, OUTPUT);

    oled.clearDisplay();
    oledPrintCenter("BOOTING...", 2, true);
    oled.display();
    delay(700);
    
    oled.clearDisplay();
    
    if(!radio.begin()){
        oledPrintCenter("NRF FAIL", 2, true); 
        oled.display();
        while(1);
    } else{
        oledPrintCenter("NRF OK", 2, true); 
        oled.display();
        delay(700);
    }

    setLEDMode(2); // startup blink

    for (size_t i = 0; i < numButtons; i++){
        lastStButtonState[i] = digitalRead(buttons[i]);
        }

    lastScButtonState = digitalRead(BUTTON_SC);

    radio.enableDynamicPayloads();
    radio.setChannel(82);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_MAX);
    radio.setAutoAck(true);
    radio.setCRCLength(RF24_CRC_16);
    radio.setRetries(15,15);
    radio.openWritingPipe(BSE_ADDR);
    radio.openReadingPipe(1, HND_ADDR);

    radio.startListening();

    oled.clearDisplay();
    oledPrintCenter("ACTIVE", 2, true);
    oled.display();
    delay(700);

    // Show initial screen
    oledShowTDC();
}

// Main Loop 
void loop() {
    uint32_t now = millis();

    // always running 
    while(GPSSerial.available()) gps.encode(GPSSerial.read());
    
    checkForResponse(); // check responses from base
    updateBlink(); // non-blocking LED&Buzzer 

    // state lock
    if(waitingForResponse && (now - sendTime > LOCK_TIMEOUT)){
        waitingForResponse = false;
    }

    if(tempScreenActive){
        if(millis() - tempScreenStart > tempScreenDuration){
            tempScreenActive = false;
            refreshCurrentScreen();
        }
        return;
    }

    // popup transitions when receiving
    if (showRecvPopup) {
        static bool drawn = false;

        if (!drawn) {
            drawResponsePopup(lastRecvMsgID);
            drawn = true;
        }

        if (now - recvPopupStart > POPUP_MS) {  // If 3 seconds have passed, 
            showRecvPopup = false;              // close the popup and return to main screen
            drawn = false;
            refreshCurrentScreen();                                    
        }
        return;
    }

    // check for button presses
    handleScreenButton(now); 
    handleStatusButtons(now);

    if(homeState == SCREEN_TDC && !tempScreenActive && !showRecvPopup){
        if(now - lastTDCUpdate > TDC_REFRESH_MS){
            lastTDCUpdate = now;
            oledShowTDC();
        }
    }

    static HomeScreenState lastState = SCREEN_TDC;
    if(homeState != lastState){
        lastState = homeState;
        refreshCurrentScreen();
    }
}
