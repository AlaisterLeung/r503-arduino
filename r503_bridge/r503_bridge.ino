#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

SoftwareSerial fpSerial(2, 3);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);

String inputBuffer = "";
unsigned long ledHoldUntil = 0;
volatile bool cancelRequested = false;
bool isBusy = false;

void setup() {
  Serial.begin(9600);
  while (!Serial);

  Serial.println(F("BOOTING"));

  bool sensorOK = false;

  // Try factory default 57600
  finger.begin(57600);
  delay(100);
  if (finger.verifyPassword()) {
    // Sensor found at 57600; downgrade to 38400 for SoftwareSerial reliability
    uint8_t result = finger.setBaudRate(FINGERPRINT_BAUDRATE_38400);
    if (result == FINGERPRINT_OK) {
      delay(100);
      finger.begin(38400);
      delay(100);
      sensorOK = finger.verifyPassword();
    }
  } else {
    // Fallback to 38400 from a prior setup
    delay(100);
    finger.begin(38400);
    delay(100);
    sensorOK = finger.verifyPassword();
  }

  if (!sensorOK) {
    replyErr(F("ERR:SENSOR_NOT_FOUND"));
    while (1);
  }

  uint8_t result = FINGERPRINT_PACKETRECIEVEERR;
  for (int i = 0; i < 3; i++) {
    flushSensorBuffer();
    result = finger.getParameters();
    if (result == FINGERPRINT_OK && finger.capacity > 0) break;
    delay(100);
  }

  if (result != FINGERPRINT_OK || finger.capacity == 0) {
    replyErr(F("ERR:GET_PARAMS_FAILED"));
    while (1);
  }

  finger.setSecurityLevel(FINGERPRINT_SECURITY_LEVEL_2);

  Serial.println(F("READY"));
  ledBlue();
}

void loop() {
  if (ledHoldUntil != 0 && millis() >= ledHoldUntil) {
    ledOff();
  }

  readCmd();
}

void readCmd() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        processCmd(inputBuffer);
        inputBuffer = "";
      }
    } else {
      inputBuffer += c;
    }
  }
}

void processCmd(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (isBusy) {
    if (cmd == "CANCEL") {
      cancelRequested = true;
    }
    return;
  }

  if (cmd == "PING") {
    replyOk();
  } else if (cmd == "INFO") {
    handleInfo();
  } else if (cmd == "ENROLL") {
    handleOp(handleEnroll);
  } else if (cmd == "VERIFY") {
    handleOp(handleVerify);
  } else if (cmd.startsWith("DELETE:")) {
    handleDelete(cmd);
  } else if (cmd == "LIST") {
    handleList();
  } else if (cmd == "CLEAR") {
    handleClear();
  } else if (cmd == "CANCEL") {
    return;
  } else {
    replyErr(F("ERR:UNKNOWN_COMMAND"));
  }
}

void handleOp(void (*op)()) {
  isBusy = true;
  cancelRequested = false;
  op();
  isBusy = false;
}

void handleInfo() {
  for (int attempt = 0; attempt < 2; attempt++) {
    flushSensorBuffer();
    uint8_t r1 = finger.getParameters();
    delay(50);
    uint8_t r2 = finger.getTemplateCount();

    if (r1 == FINGERPRINT_OK && r2 == FINGERPRINT_OK) {
      uint16_t capacity = finger.capacity;
      uint16_t used = finger.templateCount;

      if (capacity > 0 && used <= capacity) {
        Serial.print(F("OK:CAPACITY,"));
        Serial.print(capacity);
        Serial.print(F(",USED,"));
        Serial.print(used);
        Serial.print(F(",SEC_LEVEL,"));
        Serial.print(finger.security_level);
        Serial.print(F(",BAUD_RATE,"));
        Serial.println(finger.baud_rate);
        ledBlue();
        return;
      }
    }

    flushSensorBuffer();
    delay(50);
  }

  replyErr(F("ERR:SENSOR_STATE_INVALID"));
}

void handleEnroll() {
  flushSensorBuffer();

  int id = getFirstEmptyId();
  if (id == -1) {
    replyErr(F("ERR:LIBRARY_FULL"));
    return;
  }

  ledPurpleBreathing();
  delay(50);

  if (!scanFinger(1, 1, id)) return;

  Serial.println(F("OK:REMOVE_FINGER"));
  purgeSensorAck();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (cancelRequested) {
      cancelRequested = false;
      replyErr(F("ERR:CANCELLED"));
      return;
    }
    delay(50);
    readCmd();
  }
  
  delay(1000);

  if (!scanFinger(2, 2, id)) return;

  if (finger.createModel() != FINGERPRINT_OK) {
    replyErr(F("ERR:MODEL_MISMATCH"));
    return;
  }

  if (finger.storeModel(id) == FINGERPRINT_OK) {
    Serial.print(F("OK:ENROLLED,ID,"));
    Serial.println(id);
    ledBlue();
  } else {
    replyErr(F("ERR:STORE_FAILED"));
  }
}

void handleVerify() {
  flushSensorBuffer();

  ledPurpleBreathing();
  delay(50);
  Serial.println(F("OK:PLACE_FINGER"));

  if (!waitForFinger(5000)) {
    if (cancelRequested) {
      cancelRequested = false;
      replyErr(F("ERR:CANCELLED"));
    } else {
      replyErr(F("ERR:TIMEOUT"));
    }
    return;
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    replyErr(F("ERR:CONVERT_FAILED"));
    return;
  }

  int result = finger.fingerSearch();
  if (result == FINGERPRINT_OK && isValidMatch()) {
    Serial.print(F("OK:VERIFIED,ID,"));
    Serial.print(finger.fingerID);
    Serial.print(F(",CONFIDENCE,"));
    Serial.println(finger.confidence);
    ledBlue();
  } else {
    replyErr(F("ERR:NO_MATCH"));
  }
}

void handleDelete(String cmd) {
  flushSensorBuffer();

  int sep = cmd.indexOf(':');
  if (sep == -1) {
    replyErr(F("ERR:INVALID_ID"));
    return;
  }

  int id = cmd.substring(sep + 1).toInt();
  if (id <= 0 || id > finger.capacity) {
    replyErr(F("ERR:INVALID_ID"));
    return;
  }

  if (finger.deleteModel(id) == FINGERPRINT_OK) {
    Serial.print(F("OK:DELETED,ID,"));
    Serial.println(id);
    ledBlue();
  } else {
    replyErr(F("ERR:DELETE_FAILED"));
  }
}

void handleClear() {
  flushSensorBuffer();

  if (finger.emptyDatabase() == FINGERPRINT_OK) {
    finger.getTemplateCount();
    Serial.println(F("OK:CLEARED"));
    ledBlue();
  } else {
    replyErr(F("ERR:CLEAR_FAILED"));
  }
}

void handleList() {
  flushSensorBuffer();

  Serial.print(F("OK:LIST"));

  for (int id = 1; id <= finger.capacity; id++) {
    flushSensorBuffer();
    uint8_t result = finger.loadModel(id);
    if (result == FINGERPRINT_OK) {
      Serial.print(F(","));
      Serial.print(id);
    }
  }

  Serial.println();
  ledBlue();
}

void flushSensorBuffer() {
  for (int i = 0; i < 5; i++) {
    while (fpSerial.available()) {
      fpSerial.read();
    }

    delay(30);

    if (!fpSerial.available()) break;
  }
}

int getFirstEmptyId() {
  flushSensorBuffer();

  uint8_t result = finger.getTemplateCount();
  if (result == FINGERPRINT_OK && finger.templateCount == 0) {
    return 1;
  }

  for (int id = 1; id <= finger.capacity; id++) {
    flushSensorBuffer();
    result = finger.loadModel(id);
    if (result != FINGERPRINT_OK) {
      return id;
    }
  }

  return -1;
}

bool scanFinger(uint8_t slot, uint8_t step, int id) {
  Serial.print(F("OK:PLACE_FINGER,STEP,"));
  Serial.print(step);
  Serial.print(F(",ID,"));
  Serial.println(id);

  if (!waitForFinger(0)) {
    if (cancelRequested) {
      cancelRequested = false;
      replyErr(F("ERR:CANCELLED"));
    } else {
      replyErr(F("ERR:TIMEOUT"));
    }
    return false;
  }

  if (finger.image2Tz(slot) != FINGERPRINT_OK) {
    replyErr(F("ERR:CONVERT_FAILED"));
    return false;
  }

  return true;
}

void purgeSensorAck() {
  flushSensorBuffer();
  finger.getImage();
  delay(50);
  flushSensorBuffer();
}

bool waitForFinger(unsigned long timeoutMs) {
  unsigned long start = millis();

  purgeSensorAck();

  while (finger.getImage() != FINGERPRINT_OK) {
    if (cancelRequested) return false;
    if (timeoutMs > 0 && (millis() - start) > timeoutMs) {
      return false;
    }

    delay(50);
    readCmd();
  }

  return true;
}

bool isValidMatch() {
  return finger.fingerID > 0 &&
         finger.fingerID <= finger.capacity &&
         finger.confidence > 0;
}

void replyOk() {
  Serial.println(F("OK"));
  ledBlue();
}

void replyErr(const __FlashStringHelper* msg) {
  Serial.println(msg);
  ledRed();
}

void ledOff() {
  finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
  ledHoldUntil = 0;
}

void ledBlue() {
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE, 0);
  ledHoldUntil = millis() + 2000;
}

void ledRed() {
  finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
  ledHoldUntil = millis() + 2000;
}

void ledPurpleBreathing() {
  finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_PURPLE, 0);
  ledHoldUntil = 0;
}
