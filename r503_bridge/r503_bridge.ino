#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

const SoftwareSerial fpSerial(2, 3);
const Adafruit_Fingerprint finger(&fpSerial);

const uint8_t SECURITY_LEVEL = FINGERPRINT_SECURITY_LEVEL_3;
const uint8_t MAX_CMD_LEN = 32;
const unsigned long TIMEOUT_MS = 10000;

char inputBuffer[MAX_CMD_LEN + 1];
uint8_t inputBufferIndex = 0;
unsigned long ledHoldUntil = 0;
bool isBusy = false;
volatile bool canceled = false;

void setup() {
  Serial.begin(9600);
  while (!Serial)
    ;

  Serial.println(F("BOOTING"));

  finger.begin(57600);

  if (!finger.verifyPassword()) {
    replyErr(F("ERR:SENSOR_NOT_FOUND"));
    while (1)
      ;
  }

  if (finger.getParameters() != FINGERPRINT_OK || finger.capacity <= 0) {
    replyErr(F("ERR:GET_PARAMS_FAILED"));
    while (1)
      ;
  }

  finger.setSecurityLevel(SECURITY_LEVEL);

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
      if (inputBufferIndex > 0) {
        inputBuffer[inputBufferIndex] = '\0';
        inputBufferIndex = 0;
        processCmd(inputBuffer);
      }
    } else if (inputBufferIndex < MAX_CMD_LEN) {
      inputBuffer[inputBufferIndex++] = c;
    }
  }
}

void processCmd(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "CANCEL") {
    canceled = true;
    return;
  }

  if (isBusy) return;

  flushSensorBuffer();

  if (cmd == "PING") {
    Serial.println(F("OK"));
  } else if (cmd == "INFO") {
    handleInfo();
  } else if (cmd == "ENROLL") {
    handleOp(handleEnroll);
  } else if (cmd == "VERIFY") {
    handleOp(handleVerify);
  } else if (cmd == "LIST") {
    handleList();
  } else if (cmd.startsWith("DELETE:")) {
    handleDelete(cmd);
  } else if (cmd == "CLEAR") {
    handleClear();
  } else {
    replyErr(F("ERR:UNKNOWN_COMMAND"));
  }
}

void handleOp(void (*op)()) {
  isBusy = true;
  canceled = false;
  op();
  isBusy = false;
}

void handleInfo() {
  uint8_t r1 = finger.getParameters();
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

  replyErr(F("ERR:SENSOR_STATE_INVALID"));
}

void handleEnroll() {
  uint8_t id = getFirstEmptyId();
  if (id == -1) {
    replyErr(F("ERR:LIBRARY_FULL"));
    return;
  }

  ledPurpleBreathing();

  if (!scanFinger(1, id)) return;

  Serial.println(F("OK:REMOVE_FINGER"));
  purgeSensorAck();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }
  delay(500);

  if (!scanFinger(2, id)) return;

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
  ledPurpleBreathing();
  Serial.println(F("OK:PLACE_FINGER"));

  if (!waitForFinger()) {
    if (canceled) {
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

  uint8_t result = finger.fingerSearch();
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

void handleList() {
  Serial.print(F("OK:LIST"));

  for (uint8_t id = 1; id <= finger.capacity; id++) {
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

void handleDelete(String cmd) {
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
  if (finger.emptyDatabase() == FINGERPRINT_OK) {
    finger.getTemplateCount();
    Serial.println(F("OK:CLEARED"));
    ledBlue();
  } else {
    replyErr(F("ERR:CLEAR_FAILED"));
  }
}

bool scanFinger(uint8_t step, uint8_t id) {
  Serial.print(F("OK:PLACE_FINGER,STEP,"));
  Serial.print(step);
  Serial.print(F(",ID,"));
  Serial.println(id);

  if (!waitForFinger()) {
    if (canceled) {
      replyErr(F("ERR:CANCELLED"));
    } else {
      replyErr(F("ERR:TIMEOUT"));
    }
    return false;
  }

  if (finger.image2Tz(step) != FINGERPRINT_OK) {
    replyErr(F("ERR:CONVERT_FAILED"));
    return false;
  }

  return true;
}

bool waitForFinger() {
  unsigned long start = millis();

  purgeSensorAck();

  while (finger.getImage() != FINGERPRINT_OK) {
    if (millis() - start > TIMEOUT_MS || canceled) return false;

    delay(50);
    readCmd();
  }

  return true;
}

uint8_t getFirstEmptyId() {
  flushSensorBuffer();

  uint8_t result = finger.getTemplateCount();
  if (result == FINGERPRINT_OK && finger.templateCount == 0) {
    return 1;
  }

  for (uint8_t id = 1; id <= finger.capacity; id++) {
    flushSensorBuffer();
    result = finger.loadModel(id);
    if (result != FINGERPRINT_OK) {
      return id;
    }
  }

  return -1;
}

bool isValidMatch() {
  return finger.fingerID > 0 && finger.fingerID <= finger.capacity && finger.confidence > 0;
}

void flushSensorBuffer() {
  for (uint8_t i = 0; i < 5; i++) {
    while (fpSerial.available()) {
      fpSerial.read();
    }

    delay(30);

    if (!fpSerial.available()) break;
  }
}

void purgeSensorAck() {
  flushSensorBuffer();
  finger.getImage();
  delay(50);
  flushSensorBuffer();
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
