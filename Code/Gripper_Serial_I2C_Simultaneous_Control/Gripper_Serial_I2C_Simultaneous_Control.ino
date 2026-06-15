/*
 * SERIAL COMMAND FORMAT
 * Multi-motor absolute position:   M1 10000; M2 11000; M3 -1000;
 * Relative move (++ or --):        M1 ++500; M3 --1000;
 * Single commands:                 STOP | TARE | TARE 1|2|3 | STATUS
 *
 * I2C PACKET FORMAT (host → Arduino, binary) - 9 BYTES
 * [0xBB][CMD][ARG1][ARG2][ARG3][ARG4][ARG5][ARG6][XOR_CHECKSUM]
 * CMD bytes defined in CMD_* constants below.
 *
 * STATUS BROADCAST (Arduino → host)
 * Binary packet sent over I2C on request, and printed to Serial every STATUS_INTERVAL_MS.
 * [0xAA][STATUS][LC1_i32 x4][LC2_i32 x4][LC3_i32 x4][ENC1_i32 x4][ENC2_i32 x4][ENC3_i32 x4][XOR]  = 27 bytes
 * LC values in units of grams × 10 (one decimal place, integer packed).
 * STATUS byte: bit0=ready
 */

#include "RoboClaw.h"
#include <Wire.h>
#include "HX711.h"
#include <Preferences.h>

// ─── Pin / address constants ──────────────────────────────────────────────────
#define RX0PIN  D0
#define TX0PIN  D1
#define RX1PIN  D4
#define TX1PIN  D3

#define ADDR0   0x80   // RoboClaw0 — M1, M2
#define ADDR1   0x81   // RoboClaw1 — M3

#define I2C_SLAVE_ADDR  8

// ─── Motion constants ─────────────────────────────────────────────────────────
#define HOME     0
#define ACCEL    10000
#define SPEED    10000
#define DECEL    10000
#define MIN_POS  -18000
#define MAX_POS  18000

// ─── State / Busy Variables ──────────────────────────────────────────────────
#define BUSY_DELAY_MS 1450  // Time (ms) the gripper ignores commands after moving

unsigned long busyUntil = 0;

bool isReady() { 
  return millis() >= busyUntil; 
}

// ─── I2C binary command bytes ─────────────────────────────────────────────────
#define I2C_CMD_START     0xBB
#define I2C_STATUS_START  0xAA

#define CMD_SET_POS       0x01   // set absolute motor position (single)
#define CMD_STOP_ALL      0x02   // emergency stop all - only stops after command is executed, doesnt stop during command
#define CMD_TARE          0x03   // tare all LCs
#define CMD_TARE_ONE      0x04   // tare single LC (MOTOR_ID = 1/2/3)
#define CMD_SET_POS_ALL   0x05   // set all 3 motors simultaneously
#define CMD_HEARTBEAT     0xFF   // no-op keepalive

// ─── Timing ───────────────────────────────────────────────────────────────────
#define STATUS_INTERVAL_MS  50    // 20 Hz serial status broadcast

// ─── Hardware objects ─────────────────────────────────────────────────────────
RoboClaw roboclaw0(&Serial1, 10000);
RoboClaw roboclaw1(&Serial2, 10000);

HX711 scale1, scale2, scale3;
const uint8_t dataPin1 = 11, clockPin1 = 12;
const uint8_t dataPin2 =  9, clockPin2 = 10;
const uint8_t dataPin3 =  7, clockPin3 =  8;
Preferences prefs;

// ─── Motor state ──────────────────────────────────────────────────────────────
int motorPos[3] = {HOME, HOME, HOME};

// ─── Load cell state ──────────────────────────────────────────────────────────
float   lcValue[3]  = {0, 0, 0}; 
float   lcAccum[3]  = {0, 0, 0};
uint8_t lcCount[3]  = {0, 0, 0};
uint8_t lcRRIndex   = 0;         
const uint8_t LC_SAMPLES = 3;

// ─── I2C receive buffer ───────────────────────────────────────────────────────
#define I2C_PKT_LEN 9   
#define I2C_BUF_PKTS 8
volatile uint8_t i2cRawBuf[I2C_BUF_PKTS * I2C_PKT_LEN];
volatile uint8_t i2cBufHead = 0;   
volatile uint8_t i2cBufTail = 0;   

// ─── Status packet ────────────────────────────────────────────────────────────
volatile uint8_t statusPkt[27];

unsigned long lastStatusPrint = 0;


// ═════════════════════════════════════════════════════════════════════════════
//  UTILITY
// ═════════════════════════════════════════════════════════════════════════════
inline int clampPos(int v) {
  return v < MIN_POS ? MIN_POS : (v > MAX_POS ? MAX_POS : v);
}

uint8_t xorChecksum(const uint8_t* buf, uint8_t len) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < len; i++) x ^= buf[i];
  return x;
}

void setBusy() {
  busyUntil = millis() + BUSY_DELAY_MS;
  buildStatusPacket(); 
}


// ═════════════════════════════════════════════════════════════════════════════
//  MOTION
// ═════════════════════════════════════════════════════════════════════════════
void sendPos(int idx, int pos) {
  switch (idx) {
    case 0: roboclaw0.SpeedAccelDeccelPositionM1(ADDR0, ACCEL, SPEED, DECEL, pos, 0); break;
    case 1: roboclaw0.SpeedAccelDeccelPositionM2(ADDR0, ACCEL, SPEED, DECEL, pos, 0); break;
    case 2: roboclaw1.SpeedAccelDeccelPositionM1(ADDR1, ACCEL, SPEED, DECEL, pos, 0); break;
  }
}

void moveMotor(int idx, int newPos) {
  newPos = clampPos(newPos);
  motorPos[idx] = newPos;
  sendPos(idx, newPos);
}

void stopMotors() {
  bool v0 = false, v1 = false, v2 = false;

  int32_t e0 = roboclaw0.ReadEncM1(ADDR0, nullptr, &v0);
  int32_t e1 = roboclaw0.ReadEncM2(ADDR0, nullptr, &v1);
  int32_t e2 = roboclaw1.ReadEncM1(ADDR1, nullptr, &v2);
  
  if (v0) motorPos[0] = (int)e0;
  if (v1) motorPos[1] = (int)e1;
  if (v2) motorPos[2] = (int)e2;
  
  sendPos(0, motorPos[0]);
  sendPos(1, motorPos[1]);
  sendPos(2, motorPos[2]);
  
  busyUntil = 0; 
  Serial.printf("STOP — held M1=%d M2=%d M3=%d\n", motorPos[0], motorPos[1], motorPos[2]);
}


// ═════════════════════════════════════════════════════════════════════════════
//  TARE / NVS
// ═════════════════════════════════════════════════════════════════════════════
void saveTareOffsets() {
  prefs.begin("tare", false);
  prefs.putLong("o1", scale1.get_offset());
  prefs.putLong("o2", scale2.get_offset());
  prefs.putLong("o3", scale3.get_offset());
  prefs.end();
}

void loadTareOffsets() {
  prefs.begin("tare", true);
  long o1 = prefs.getLong("o1", LONG_MIN);
  long o2 = prefs.getLong("o2", LONG_MIN);
  long o3 = prefs.getLong("o3", LONG_MIN);
  prefs.end();
  
  if (o1 != LONG_MIN) scale1.set_offset(o1);
  if (o2 != LONG_MIN) scale2.set_offset(o2);
  if (o3 != LONG_MIN) scale3.set_offset(o3);
}

void tareSingle(int idx) {
  if (idx == 0) scale1.tare(10);
  else if (idx == 1) scale2.tare(10);
  else if (idx == 2) scale3.tare(10);
  
  lcAccum[idx] = 0; lcCount[idx] = 0; lcValue[idx] = 0.0f;
  saveTareOffsets();
  Serial.printf("TARE: LC%d zeroed\n", idx + 1);
}

void tareAll() {
  Serial.println("TARE: zeroing all...");
  scale1.tare(10); scale2.tare(10); scale3.tare(10);
  for (int i = 0; i < 3; i++) { 
    lcAccum[i] = 0; lcCount[i] = 0; lcValue[i] = 0.0f;
  }
  saveTareOffsets();
  Serial.println("TARE: done");
}


// ═════════════════════════════════════════════════════════════════════════════
//  STATUS PACKET
// ═════════════════════════════════════════════════════════════════════════════
void buildStatusPacket() {
  uint8_t status = (isReady() ? 0x01 : 0x00);  // bit0: ready (0 = busy)

  statusPkt[0] = I2C_STATUS_START;
  statusPkt[1] = status;

  // Pack each LC value as int32 (grams × 10) big-endian
  for (int i = 0; i < 3; i++) {
    int32_t val = (int32_t)(lcValue[i] * 10.0f);
    statusPkt[2 + i*4 + 0] = (val >> 24) & 0xFF;
    statusPkt[2 + i*4 + 1] = (val >> 16) & 0xFF;
    statusPkt[2 + i*4 + 2] = (val >>  8) & 0xFF;
    statusPkt[2 + i*4 + 3] = (val      ) & 0xFF;
  }

  // Pack each motor position as int32 big-endian
  for (int i = 0; i < 3; i++) {
    int32_t enc = motorPos[i];
    statusPkt[14 + i*4 + 0] = (enc >> 24) & 0xFF;
    statusPkt[14 + i*4 + 1] = (enc >> 16) & 0xFF;
    statusPkt[14 + i*4 + 2] = (enc >>  8) & 0xFF;
    statusPkt[14 + i*4 + 3] = (enc      ) & 0xFF;
  }

  // XOR Checksum over the 26 data bytes
  statusPkt[26] = xorChecksum((const uint8_t*)statusPkt, 26);
}

// ═════════════════════════════════════════════════════════════════════════════
//  NON-BLOCKING LOAD CELL UPDATE
// ═════════════════════════════════════════════════════════════════════════════
void updateLC() {
  HX711* sensors[3] = {&scale1, &scale2, &scale3};
  HX711* s = sensors[lcRRIndex];
  if (!s->is_ready()) return;

  lcAccum[lcRRIndex] += s->get_units(1);
  lcCount[lcRRIndex]++;
  
  if (lcCount[lcRRIndex] >= LC_SAMPLES) {
    lcValue[lcRRIndex] = lcAccum[lcRRIndex] / LC_SAMPLES;
    lcAccum[lcRRIndex] = 0;
    lcCount[lcRRIndex] = 0;
    lcRRIndex = (lcRRIndex + 1) % 3;
    if (lcRRIndex == 0) buildStatusPacket(); 
  } else {
    lcRRIndex = (lcRRIndex + 1) % 3;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SERIAL STATUS PRINT  (20 Hz)
// ═════════════════════════════════════════════════════════════════════════════
void printStatus() {
  if (millis() - lastStatusPrint < STATUS_INTERVAL_MS) return;
  lastStatusPrint = millis();
  
  Serial.printf("%d,%.2f,%.2f,%.2f,%d,%d,%d\n",
    isReady() ? 1 : 0,
    lcValue[2], lcValue[0], lcValue[1],
    motorPos[0], motorPos[1], motorPos[2]);
}


// ═════════════════════════════════════════════════════════════════════════════
//  SERIAL COMMAND PARSER
// ═════════════════════════════════════════════════════════════════════════════
void parseSerial(String& line) {
  line.trim();
  if (line.length() == 0) return;
  
  if (line.equalsIgnoreCase("STOP")) { stopMotors(); return; }
  if (line.equalsIgnoreCase("TARE")) { tareAll(); return; }
  if (line.equalsIgnoreCase("STATUS")) {
    Serial.printf("LC: %.2f, %.2f, %.2f  |  ENC: %d, %d, %d  | READY: %d\n",
      lcValue[0], lcValue[1], lcValue[2], motorPos[0], motorPos[1], motorPos[2], isReady() ? 1 : 0);
    return;
  }

  int  newPos[3]    = {motorPos[0], motorPos[1], motorPos[2]};
  bool moveFlag[3]  = {false, false, false};

  int start = 0;
  while (start <= (int)line.length()) {
    int end = line.indexOf(';', start);
    if (end == -1) end = line.length();
    
    String tok = line.substring(start, end);
    tok.trim();
    start = end + 1;

    if (tok.length() == 0) continue;
    
    int sp = tok.indexOf(' ');
    if (sp == -1) continue;

    String cmd = tok.substring(0, sp);
    String valStr = tok.substring(sp + 1);
    valStr.trim();
    
    if (cmd.equalsIgnoreCase("TARE")) {
      int idx = valStr.toInt() - 1;
      if (idx >= 0 && idx < 3) tareSingle(idx);
      continue;
    }

    if (cmd.length() == 2 && (cmd[0] == 'M' || cmd[0] == 'm')) {
      int mIdx = cmd[1] - '1'; 
      if (mIdx < 0 || mIdx > 2) continue;

      if (!isReady()) {
        Serial.printf("Busy: ignoring command for M%d\n", mIdx + 1);
        continue;
      }

      bool isRelative = false;
      long val = 0;

      if (valStr.startsWith("++")) {
        isRelative = true;
        val = strtol(valStr.substring(2).c_str(), nullptr, 10);
      } else if (valStr.startsWith("--")) {
        isRelative = true;
        val = -strtol(valStr.substring(2).c_str(), nullptr, 10);
      } else {
        isRelative = false;
        val = strtol(valStr.c_str(), nullptr, 10);
      }

      if (isRelative) {
        newPos[mIdx] = motorPos[mIdx] + (int)val;
      } else {
        newPos[mIdx] = HOME + (int)val; 
      }
      
      moveFlag[mIdx] = true;
      continue;
    }
  }

  bool didMove = false;
  for (int i = 0; i < 3; i++) {
    if (moveFlag[i]) {
      Serial.printf("M%d → %d (offset %+d)\n", i+1, clampPos(newPos[i]), clampPos(newPos[i]) - HOME);
      moveMotor(i, newPos[i]);
      didMove = true;
    }
  }
  
  if (didMove) setBusy(); 
}


// ═════════════════════════════════════════════════════════════════════════════
//  I2C BINARY COMMAND DISPATCH
// ═════════════════════════════════════════════════════════════════════════════
void dispatchI2CPacket(const uint8_t* pkt) {
  // pkt = [0xBB][CMD][ARG1][ARG2][ARG3][ARG4][ARG5][ARG6][XOR]
  if (pkt[0] != I2C_CMD_START) return;
  
  // XOR Checksum is now calculated over the first 8 bytes and checked against the 9th
  if (xorChecksum(pkt, 8) != pkt[8]) {
    Serial.println("I2C: bad checksum, discarding");
    return;
  }

  uint8_t cmd = pkt[1];

  switch (cmd) {
    case CMD_SET_POS: {
      if (!isReady()) {
        Serial.println("I2C: Busy, ignoring SET_POS");
        break;
      }
      uint8_t motorId = pkt[2];                           
      int     val     = (uint16_t)((pkt[3] << 8) | pkt[4]); 
      int mIdx = motorId - 1;
      
      if (mIdx >= 0 && mIdx < 3) {
        moveMotor(mIdx, val);
        setBusy();
        Serial.printf("I2C SET_POS M%d → %d\n", motorId, val);
      }
      break;
    }

    // NEW Multi-Motor Set Command
    case CMD_SET_POS_ALL: {
      if (!isReady()) {
        Serial.println("I2C: Busy, ignoring SET_POS_ALL");
        break;
      }
      // Unpack 3 uint16_t variables
      uint16_t m1 = (pkt[2] << 8) | pkt[3];
      uint16_t m2 = (pkt[4] << 8) | pkt[5];
      uint16_t m3 = (pkt[6] << 8) | pkt[7];

      // 0xFFFF is used as an "ignore this motor" flag
      if (m1 != 0xFFFF) moveMotor(0, m1);
      if (m2 != 0xFFFF) moveMotor(1, m2);
      if (m3 != 0xFFFF) moveMotor(2, m3);

      setBusy();
      Serial.printf("I2C SET_POS_ALL -> M1:%d  M2:%d  M3:%d\n", 
                     (m1==0xFFFF?-1:m1), (m2==0xFFFF?-1:m2), (m3==0xFFFF?-1:m3));
      break;
    }
      
    case CMD_STOP_ALL:
      stopMotors();
      break;

    case CMD_TARE:
      tareAll();
      break;
      
    case CMD_TARE_ONE: {
      int mIdx = pkt[2] - 1;
      if (mIdx >= 0 && mIdx < 3) tareSingle(mIdx);
      break;
    }
      
    case CMD_HEARTBEAT:
      break;   // no-op keepalive

    default:
      Serial.printf("I2C: unknown CMD 0x%02X\n", cmd);
      break;
  }
}


// ═════════════════════════════════════════════════════════════════════════════
//  I2C ISRs
// ═════════════════════════════════════════════════════════════════════════════
void receiveEvent(int numBytes) {
  static uint8_t rxBuf[I2C_PKT_LEN];
  static uint8_t rxFill = 0;

  while (Wire.available()) {
    uint8_t b = Wire.read();

    // Heartbeat: single 0xFF byte — discard any partial frame and ignore
    if (b == CMD_HEARTBEAT && rxFill == 0) continue;

    // Wait for frame start byte
    if (b == I2C_CMD_START && rxFill == 0) {
      rxBuf[rxFill++] = b;
      continue;
    }

    // Mid-packet: accumulate bytes
    if (rxFill > 0) {
      rxBuf[rxFill++] = b;
    }

    // Complete packet received — push to ring buffer
    if (rxFill == I2C_PKT_LEN) {
      uint8_t nextHead = (i2cBufHead + 1) % I2C_BUF_PKTS;
      if (nextHead != i2cBufTail) {
        memcpy((void*)&i2cRawBuf[i2cBufHead * I2C_PKT_LEN], rxBuf, I2C_PKT_LEN);
        i2cBufHead = nextHead;
      }
      rxFill = 0;
    }
  }
}

void requestEvent() {
  // Respond with 27-byte status packet
  Wire.write((const uint8_t*)statusPkt, 27);
}


// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  Serial1.begin(38400, SERIAL_8N1, RX0PIN, TX0PIN);
  Serial2.begin(9600,  SERIAL_8N1, RX1PIN, TX1PIN);
  roboclaw0.begin(38400);
  roboclaw1.begin(9600);
  
  roboclaw0.SetEncM1(ADDR0, HOME);
  roboclaw0.SetEncM2(ADDR0, HOME);
  roboclaw1.SetEncM1(ADDR1, HOME);
  motorPos[0] = motorPos[1] = motorPos[2] = HOME;

  Wire.begin(I2C_SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  
  scale1.begin(dataPin1, clockPin1);
  scale2.begin(dataPin2, clockPin2);
  scale3.begin(dataPin3, clockPin3);
  scale1.set_scale(65.722504f);
  scale2.set_scale(61.299000f);
  scale3.set_scale(67.263794f);

  loadTareOffsets();
  prefs.begin("tare", true);
  bool hasSaved = prefs.isKey("o1");
  prefs.end();
  
  if (!hasSaved) {
    Serial.println("First boot: initial tare...");
    tareAll();
  }

  buildStatusPacket();

  Serial.println("=== GripperControl v6 ready ===");
  Serial.printf("HOME=%d  MIN=%d  MAX=%d  I2C=0x%02X\n", HOME, MIN_POS, MAX_POS, I2C_SLAVE_ADDR);
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  static bool lastReadyState = true;
  bool currentReady = isReady();
  if (currentReady != lastReadyState) {
    buildStatusPacket();
    lastReadyState = currentReady;
  }

  updateLC();
  
  while (i2cBufTail != i2cBufHead) {
    uint8_t pkt[I2C_PKT_LEN];
    memcpy(pkt, (const void*)&i2cRawBuf[i2cBufTail * I2C_PKT_LEN], I2C_PKT_LEN);
    i2cBufTail = (i2cBufTail + 1) % I2C_BUF_PKTS;
    dispatchI2CPacket(pkt);
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    parseSerial(line);
  }

  printStatus();
}