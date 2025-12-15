#include "RoboClaw.h"
#include <Wire.h>
#include "HX711.h"


#define RX1PIN D4
#define TX1PIN D3
#define address 0x80

const int I2C_SLAVE_ADDRESS = 8; //I2C address (1-127)
String receivedCommand = "";

RoboClaw roboclaw0(&Serial0,10000);
RoboClaw roboclaw1(&Serial1,10000);

HX711 scale1;
HX711 scale2;
HX711 scale3;

//  adjust pins if needed
uint8_t dataPin1 = 11;
uint8_t clockPin1 = 12;
uint8_t dataPin2 = 9;
uint8_t clockPin2 = 10;
uint8_t dataPin3 = 7;
uint8_t clockPin3 = 8;


float M1tens,M2tens,M3tens;

// New global string to hold data for the I2C Master (Kinova)
String loadCellDataToSend = "";

String inputString = "";
String valueString = "";
int valueInt = 0;
int motor1pos = 0;
int motor2pos = 0;
int motor3pos = 0;
int incr = 2500;
int maxvalue = 150000;
int enc1,enc2,enc3;
int iterationCounter = 0;
bool UseTensionSensors = 0;
float Maxtension = 0;
float f,g,h;

// NEW: Function to zero all three load cells
void tareLoadCells(){
  Serial.println("TARE: Zeroing load cells...");
  scale1.tare();
  scale2.tare();
  scale3.tare();
}

void prepareLoadCellData(){
  if (scale1.is_ready()){
      // Reading with 5 averages for better stability
     M1tens = scale1.get_units(5);
  }
  if (scale2.is_ready()){
    M2tens = scale2.get_units(5);
  }
  if (scale3.is_ready()){
    M3tens = scale3.get_units(5);
  }

  // Format the data as a comma-separated string for easy parsing in Python: "M1:X.XX,M2:Y.YY,M3:Z.ZZ"
  loadCellDataToSend = "M1:" + String(M1tens, 2) + ",M2:" + String(M2tens, 2) + ",M3:" + String(M3tens, 2);
}

void setup() {
  Serial1.begin(9600, SERIAL_8N1, RX1PIN, TX1PIN);
  //Open Serial && roboclaw at 38400bps
  Serial.begin(57600);
  roboclaw0.begin(38400);
  roboclaw1.begin(9600);
  Serial.println("ESP32 Nano I2C Slave Receiver/Sender");
  Wire.begin(I2C_SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);
  
  // NEW: Register the function to call when the I2C Master requests data
  Wire.onRequest(requestEvent); 

  Serial.print("I2C Slave listening on address: ");
  Serial.println(I2C_SLAVE_ADDRESS);
  roboclaw1.SpeedAccelDeccelPositionM1(address,000,000,0000,motor3pos,1);
  roboclaw0.SpeedAccelDeccelPositionM1(address,0000,000,0,motor1pos,1);
  roboclaw0.SpeedAccelDeccelPositionM2(address,0000,000,0,motor2pos,1);
  
  // Load Cell Initialization and Calibration
  scale1.begin(dataPin1, clockPin1);
  scale2.begin(dataPin2, clockPin2);
  scale3.begin(dataPin3, clockPin3);
  scale1.set_offset(36513);
  scale1.set_scale(-19.260117);
  scale2.set_offset(-12193);
  scale2.set_scale(19.236208);
  scale3.set_offset(-12023);
  scale3.set_scale(-18.456814);
  scale1.tare();
  scale2.tare();
  scale3.tare();
}

void loop() {
  iterationCounter++;
  
  // Update the load cell reading string periodically so the data is always fresh when requested
  if (iterationCounter >= 10000){
    prepareLoadCellData();
    iterationCounter = 0;
  }
  
  if (receivedCommand != "") {
    Serial.print("Received Command ");
    Serial.println(receivedCommand);
    if (receivedCommand == "0x01"){
      if (motor1pos >= 0 && motor1pos <= maxvalue-incr ){
        motor1pos += incr;}
    }
    else if (receivedCommand == "0x02"){
       if (motor1pos >= incr && motor1pos <= maxvalue){
        motor1pos -= incr;}
    }
    else if (receivedCommand == "0x03"){
       if (motor2pos >= 0 && motor2pos <= maxvalue-incr){
      motor2pos += incr;}
   
    }
    else if (receivedCommand == "0x04"){
       if (motor2pos >= incr && motor2pos <= maxvalue){
      motor2pos -= incr;}
    }
    else if (receivedCommand == "0x05"){
       if (motor3pos >= 0 && motor3pos <= maxvalue-incr){
      motor3pos += incr;}
    }
    else if (receivedCommand == "0x06"){
       if (motor3pos >= incr && motor3pos <= maxvalue){
      motor3pos -= incr;}
 
    }
    else if (receivedCommand == "0x07"){
      if ((motor1pos >= 0 && motor1pos < maxvalue-incr) && (motor2pos >= 0 && motor2pos <= maxvalue-incr) && (motor3pos >= 0 && motor3pos <= maxvalue-incr)){
        motor1pos += incr;
        motor2pos += incr;
        motor3pos += incr;
      }
    }
    else if (receivedCommand == "0x08"){
      if ((motor1pos >= incr && motor1pos <= maxvalue) && (motor2pos >= incr && motor2pos <= maxvalue) && (motor3pos >= incr && motor3pos <= maxvalue)){
        motor1pos -= incr;
        motor2pos -= incr;
        motor3pos -= incr;
      }
    }
    else if (receivedCommand == "0x09"){
      stopMotors();
    }
    else if (receivedCommand == "0x10"){
      roboclaw0.SetEncM1(address, 0);
      roboclaw0.SetEncM2(address, 0);
      roboclaw1.SetEncM1(address, 0);
      motor1pos = 0;
      motor2pos = 0;
      motor3pos = 0;
    }
    else if (receivedCommand == "0x11"){
      roboclaw0.SetEncM1(address, maxvalue);
      roboclaw0.SetEncM2(address, maxvalue);
      roboclaw1.SetEncM1(address, maxvalue);
      motor1pos = maxvalue;
      motor2pos = maxvalue;
      motor3pos = maxvalue;
    }
    else if (receivedCommand == "0x12"){
      balanceMotorForces();
    }
    // NEW: Load Cell Tare/Zero command
    else if (receivedCommand == "0x13"){
      tareLoadCells();
    }
    Serial.print("motor 1 position : ");
    Serial.println(motor1pos);
    Serial.print("motor 2 position : ");
    Serial.println(motor2pos);
    Serial.print("motor 3 position : ");
    Serial.println(motor3pos);
    //bool SpeedAccelDeccelPositionM1(address, accel, speed, deccel, position, flag);
    roboclaw1.SpeedAccelDeccelPositionM1(address,000,000,0000,motor3pos,1);
    roboclaw0.SpeedAccelDeccelPositionM1(address,0000,000,0,motor1pos,1);
    roboclaw0.SpeedAccelDeccelPositionM2(address,0000,000,0,motor2pos,1);
  }
    
  receivedCommand = "";
}

void stopMotors(){
  enc1 = roboclaw0.ReadEncM1(address);
  enc2 = roboclaw0.ReadEncM2(address);
  enc3 = roboclaw1.ReadEncM1(address);
  roboclaw1.SpeedAccelDeccelPositionM1(address,000,000,0000,enc3,1);
  roboclaw0.SpeedAccelDeccelPositionM1(address,0000,000,0,enc1,1);
  roboclaw0.SpeedAccelDeccelPositionM2(address,0000,000,0,enc2,1);

}

void balanceMotorForces(){
  M1tens = abs(scale1.get_units(5));
  M2tens = abs(scale2.get_units(5));
  M3tens = abs(scale3.get_units(5));
  //Find the highest force
  Maxtension = max(M1tens,M2tens);
  Maxtension = max(Maxtension,M3tens);
  //Increment the other motors until all three tension sensors are within 10
  while(Maxtension > (M1tens +30)){
    motor1pos += incr/4;
    roboclaw0.SpeedAccelDeccelPositionM1(address,0000,000,0,motor1pos,1);
  }
  while(Maxtension > (M2tens +30)){
    motor2pos += incr/4;
    roboclaw0.SpeedAccelDeccelPositionM2(address,0000,000,0,motor2pos,1);
  }
  while(Maxtension > (M3tens +30)){
    motor3pos += incr/4;
    roboclaw1.SpeedAccelDeccelPositionM1(address,000,000,0000,motor3pos,1);
  }
}

void receiveEvent(int byteCount) {
  // Read all the bytes that were sent
  while (Wire.available()) {
    char c = Wire.read();
    // Read a byte as a character
    receivedCommand += c;
    // Append the character to the string
  }
  
}

// Function to execute when I2C Master (Kinova) requests data
void requestEvent() {
  // Wire.write() sends the data back to the master
  // We send the latest prepared string of load cell data
  Wire.write(loadCellDataToSend.c_str());
}