/*
 * transmitter for ewinch
 * sends current state (pull value)
 * receives acknowlegement with current parameters
 * communication is locked to a specific transmitter for 5 seconds after his last message
 * admin ID 0 can always take over communication
 * ...adding servo control for line-cutter
 */

static int myID = 8;    // set to your desired transmitter id, "0" is for admin 1 - 15 is for additional transmitters [unique number from 1 - 15]
static int myMaxPull = 85;  // 0 - 127 [kg], must be scaled with VESC ppm settings

#include <Pangodream_18650_CL.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include <rom/rtc.h>
#include "Arduino.h"
#include <Button2.h>
#include "common.h"

// Initialize the OLED display using Wire library
SSD1306Wire display(0x3c, SDA, SCL);   // ADDRESS, SDA, SCL - SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
// End stuff taken from SS1306DrawingDemo.io

#define SCK     5    // GPIO5  -- SX1278's SCK
#define MISO    19   // GPIO19 -- SX1278's MISnO
#define MOSI    27   // GPIO27 -- SX1278's MOSI
#define SS      18   // GPIO18 -- SX1278's CS
#define RST     14   // GPIO14 -- SX1278's RESET
#define DI0     26   // GPIO26 -- SX1278's IRQ(Interrupt Request)
#define BAND  868E6  //frequency in Hz (433E6, 868E6, 915E6) 

// SSD1306 display(0x3c, 21, 22); //replaced with the above from SSD1306DrawingDemo.ino
int rssi = 0;
float snr = 0;
String packSize = "--";
String packet ;

// battery measurement
//#define CONV_FACTOR 1.7
//#define READS 20
Pangodream_18650_CL BL(35); // pin 34 old / 35 new v2.1 hw

// Buttons for state machine control
#define BUTTON_UP  15 // up
#define BUTTON_DOWN  12 // down

Button2 btnUp = Button2(BUTTON_UP);
Button2 btnDown = Button2(BUTTON_DOWN);

static int loopStep = 0;
bool toogleSlow = true;
int8_t targetPull = 0;   // pull value range from -127 to 127
int currentPull = 0;          // current active pull on vesc
bool stateChanged = false;
int currentState = -1;   // status to start with: -2 = hard brake, -1 = soft brake, 0 = no pull/no brake, 1 = default pull (~3kg), 2 = pre pull, 3 = take off pull, 4 = full pull, 5 = extra strong pull
int hardBrake = -20;  //in kg - status -2 this status is 20kg brake - 
int softBrake = -7;  //in kg - status -1 this status is 7kg brake - activated with a long stop press on "buttonDown"
int defaultPull = 7;  //in kg - status 1 this will always be activated when the "brake-button" (buttonDown) is pressed (unless a long press, which activates the "soft-break" status)
int prePullScale = 18;      //in %, calculated in code below: "targetPull = myMaxPull * prePullScale / 100;" - status 2 this is to help you launch your glider up into the air, approx 13kg pull value
int takeOffPullScale = 55;  //in % - status 3 this is to gently and safely take off with a slight pull
int fullPullScale = 80;     //in % - status 4 once you have a safety margin of 15-30m heigt, switch to this status for full pull
int strongPullScale = 100;  //in % - status 5 - extra strong pull - this is equal your value in kg as defined in "myMaxPull" above. should be equal your bodyweight
unsigned long lastStateSwitchMillis = 0;

uint8_t vescBattery = 0;
uint8_t vescTempMotor = 0;

struct LoraTxMessage loraTxMessage;
struct LoraRxMessage loraRxMessage;

unsigned long lastTxLoraMessageMillis = 0;    //last message send
unsigned long lastRxLoraMessageMillis = 0;    //last message received
unsigned long previousRxLoraMessageMillis = 0;

unsigned int loraErrorCount = 0;
unsigned long loraErrorMillis = 0;


void setup() {
  Serial.begin(115200);
  
 // //OLED display // replaced with stuff from SSD1306DrawingDemo.ino
 // pinMode(16,OUTPUT);
 // pinMode(2,OUTPUT);
 // digitalWrite(16, LOW);    // set GPIO16 low to reset OLED
 // delay(50); 
 // digitalWrite(16, HIGH); // while OLED is running, must set GPIO16 in high

  //lora init
  SPI.begin(SCK,MISO,MOSI,SS);
  LoRa.setPins(SS,RST,DI0);
  if (!LoRa.begin(868E6)) {
    Serial.println("Starting LoRa failed!");
    while (1);
  }
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  //LoRa.setSpreadingFactor(10);   // default is 7, 6 - 12
  LoRa.enableCrc();
  //LoRa.setSignalBandwidth(500E3);   //signalBandwidth - signal bandwidth in Hz, defaults to 125E3. Supported values are 7.8E3, 10.4E3, 15.6E3, 20.8E3, 31.25E3, 41.7E3, 62.5E3, 125E3, 250E3, and 500E3.

  // display init
  display.init();
  //display.flipScreenVertically();  

  //Serial.println(" Longpress Time: " + String(btnUp.getLongClickTime()) + "ms");
  //Serial.println(" DoubleClick Time: " + String(btnUp.getDoubleClickTime()) + "ms");
  btnUp.setPressedHandler(btnPressed);
  btnDown.setPressedHandler(btnPressed);
  btnDown.setLongClickTime(500);
  btnDown.setLongClickDetectedHandler(btnDownLongClickDetected);
  btnDown.setDoubleClickTime(400);
  btnDown.setDoubleClickHandler(btnDownDoubleClick);

  // Etienne: Let me try if I can code :-) trying to add "triple Click" Function for triggering a servo, that will pull a pin for a line-cutter to be deployd on the winch
  btnDown.setTripleClickTime(400);      
  btnDown.setTripleClickHandler(btnDownTripleClick);
  // Etienne: TripleClick Test End
    
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  Serial.printf("Starting Transmitter \n");
  display.drawString(0, 0, "Starting Transmitter");

  // admin --> scan for existing transmitter for a few seconds --> start up with his current pull state
  if (myID == 0 ) {
      display.clear();
      display.setTextAlignment(TEXT_ALIGN_LEFT);
      display.setFont(ArialMT_Plain_16);
      display.drawString(0, 0, "Searching 4s for");
      display.drawString(0, 14, "existing transmitter...");
      display.display();
      lastTxLoraMessageMillis = millis();
      while (millis() < lastTxLoraMessageMillis + 4000) {
          // packet from transmitter
          if (LoRa.parsePacket() >= sizeof(loraTxMessage) ) {
            LoRa.readBytes((uint8_t *)&loraTxMessage, sizeof(loraTxMessage));
            if (loraTxMessage.pullValue == loraTxMessage.pullValueBackup) {
                //found --> read state and exit
                currentState = loraTxMessage.currentState;
                targetPull = loraTxMessage.pullValue;
                Serial.printf("Found existing transmitter, starting up with state: %d: %d \n", currentState, targetPull);
                //exit search loop
                lastTxLoraMessageMillis = millis() - 4000;
            }
          } 
          delay(10);
       }
   }

   // reset to my transmitter id
   loraTxMessage.id = myID;
  
}


void loop() {

    loopStep++;
  
    // screen
    if (loopStep % 100 == 0) {
      toogleSlow = !toogleSlow;
    }
    if (loopStep % 10 == 0) {
      display.clear();
      display.setTextAlignment(TEXT_ALIGN_LEFT);
      display.setFont(ArialMT_Plain_16);  //10, 16, 24
      if (toogleSlow) {
          display.drawString(0, 0, loraTxMessage.id + String("-B: ") + vescBattery + "%, T: " + vescTempMotor + " C");        
      } else {
          display.drawString(0, 0, loraTxMessage.id + String("-T: ") + BL.getBatteryChargeLevel() + "%, " + rssi + "dBm, " + snr + ")");        
      }
      display.setFont(ArialMT_Plain_24);  //10, 16, 24
      display.drawString(0, 14, String(currentState) + String(" (") + targetPull + "/" + currentPull + String("kg)"));
      display.drawString(0, 36, String(loraRxMessage.tachometer * 10) + "m| " + String(loraRxMessage.dutyCycleNow) + "%" );
      display.display();
    }
    
    // LoRa data available?
    //==acknowledgement from receiver?
    if (LoRa.parsePacket() == sizeof(loraRxMessage) ) {
        LoRa.readBytes((uint8_t *)&loraRxMessage, sizeof(loraRxMessage));
        currentPull = loraRxMessage.pullValue;
        // vescBatteryPercentage and vescTempMotor are alternated on lora link to reduce packet size
          if (loraRxMessage.vescBatteryOrTempMotor == 1){
            vescBattery = loraRxMessage.vescBatteryOrTempMotorValue;
          } else {
            vescTempMotor = loraRxMessage.vescBatteryOrTempMotorValue;
          }
        previousRxLoraMessageMillis = lastRxLoraMessageMillis;  // remember time of previous paket
        lastRxLoraMessageMillis = millis();
        rssi = LoRa.packetRssi();
        snr = LoRa.packetSnr();
        Serial.printf("Value received: %d, RSSI: %d: , SNR: %d \n", loraRxMessage.pullValue, rssi, snr);
        Serial.printf("tacho: %d, dutty: %d: \n", loraRxMessage.tachometer * 10, loraRxMessage.dutyCycleNow);
   }

  // if no lora message for more then 1,5s --> show error on screen + acustic
  if (millis() > lastRxLoraMessageMillis + 1500 ) {
        //TODO acustic information
        //TODO  red disply
        display.clear();
        display.display();
        // log connection error
       if (millis() > loraErrorMillis + 5000) {
            loraErrorMillis = millis();
            loraErrorCount = loraErrorCount + 1;
       }
  }
  
        // state machine
        // -2 = hard brake -1 = soft brake, 0 = no pull / no brake, 1 = default pull (2kg), 2 = pre pull, 3 = take off pull, 4 = full pull, 5 = extra strong pull
        switch(currentState) {
            case -2:
              targetPull = hardBrake; // -> hard brake
              break;
            case -1:
              targetPull = softBrake; // -> soft brake
              break;
            case 0:
              targetPull = 0; // -> neutral, no pull / no brake
              break;
            case 1: 
              targetPull = defaultPull;   //independent of max pull
              break;
            case 2: 
              targetPull = myMaxPull * prePullScale / 100;
              break;
            case 3:
              targetPull = myMaxPull * takeOffPullScale / 100;
              break;
            case 4:
              targetPull = myMaxPull * fullPullScale / 100;
              break;
            case 5:
              targetPull = myMaxPull * strongPullScale / 100;
              break;
            default: 
              targetPull = softBrake;
              Serial.println("no valid state");
              break;
          }

        delay(10);

        // send Lora message every 400ms  --> three lost packages lead to failsafe on receiver (>1,5s)
        // send immediatly if state has changed
        if (millis() > lastTxLoraMessageMillis + 400 || stateChanged) {
            stateChanged = false;
            loraTxMessage.currentState = currentState;
            loraTxMessage.pullValue = targetPull;
            loraTxMessage.pullValueBackup = targetPull;
            if (LoRa.beginPacket()) {
                LoRa.write((uint8_t*)&loraTxMessage, sizeof(loraTxMessage));
                LoRa.endPacket();
                Serial.printf("sending value %d: \n", targetPull);
                lastTxLoraMessageMillis = millis();  
            } else {
                Serial.println("Lora send busy");
            }
        }

        btnUp.loop();
        btnDown.loop();
        delay(10);
}

void btnPressed(Button2& btn) {
    if (btn == btnUp) {
        Serial.println("btnUP pressed");
        //do not switch up to fast
        if (millis() > lastStateSwitchMillis + 1000 && currentState < 5) {
          currentState = currentState + 1;
          // skip neutral state to prevent line mess up
          if (currentState == 0 ) {
              currentState = currentState + 1;
          }
          lastStateSwitchMillis = millis();
          stateChanged = true;
        }
    } else if (btn == btnDown) {
        Serial.println("btnDown pressed");
        if (currentState > 1) {
          currentState = 1;   //default pull
          lastStateSwitchMillis = millis();
          stateChanged = true;
        } else if (currentState > -2 && currentState < 1){
          currentState = currentState - 1;
          lastStateSwitchMillis = millis();
          stateChanged = true;
        }
    }
}

void btnDownLongClickDetected(Button2& btn) {
    currentState = -1;    //brake
    lastStateSwitchMillis = millis();
    stateChanged = true;
}
void btnDownDoubleClick(Button2& btn) {
  // only get to neutral state from brake
  if (currentState <= -1) {
    currentState = 0;    // neutral
    lastStateSwitchMillis = millis();
    stateChanged = true;
  }
// Etienne: Let's try the Triple Click Function of "Button Down" to trigger the Servo-LineCutter
void btnDownTripleClick(Button2& btn) {
    currentState = -1;    //brake
    lastStateSwitchMillis = millis();
    stateChanged = true;
    somethingneededhere to trigger servo
    }
}
