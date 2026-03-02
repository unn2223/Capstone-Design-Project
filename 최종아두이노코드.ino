/*
 * 무선전력 충전 시스템 (XY Stage Auto-Align & Charging)
 * 최종 통합본 (Final Version + FB/LR Auto Align + S5 Home Align by d1/d4)
 */

#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ==========================================
// 1. 하드웨어 설정
// ==========================================
// 듀얼 LCD (상단/하단 분리 표시)
LiquidCrystal_I2C lcd1(0x27, 16, 2); // 상단 LCD
LiquidCrystal_I2C lcd2(0x26, 16, 2); // 하단 LCD

// ★ LED 핀은 Mega 여유 핀으로 이동함 (서보/초음파와 충돌 피하기 위해)
//   - 기존: 11, 12, 13  →  변경: 22, 23, 24
const int PIN_LED_R = 22;
const int PIN_LED_O = 23;
const int PIN_LED_G = 24;

// 서보 (앞뒤 / 좌우)
// 두 번째 코드 요구대로 유지
const int PIN_SERVO_FB = 11;  // 앞뒤 축 (연속회전 서보)
const int PIN_SERVO_LR = 9;   // 좌우 축 (연속회전 서보)

// 초음파 핀 (두 번째 코드 기준 그대로)
// 앞뒤용 (diff)
const int TRIG1 = 13;
const int ECHO1 = 12;

const int TRIG2 = 30;
const int ECHO2 = 31;

// 좌우용 (sum)
const int TRIG3 = 2;
const int ECHO3 = 3;

const int TRIG4 = 6;
const int ECHO4 = 7;

// 전류 센서
const int PIN_CURRENT = A0;

// 부저 (미스매치 알림)
const int PIN_BUZZER = A8;

// 서보 객체
Servo servoFB;   // 앞뒤 (Y축에 해당)
Servo servoLR;   // 좌우 (X축에 해당)

// 번호판 문자열
String plateText = "";

// ==========================================
// 2. 캘리브레이션 / 정렬 파라미터
// ==========================================

// (참고) 기존 X/Y 선형 보정용 변수 (지금은 사용하지 않지만 보존)
// 공식: 각도 = (AX * 거리) + BX
float AX = 1.0;
float BX = 0.0;
float AY = 1.0;
float BY = 0.0;

// 연속회전 서보 "홈" (정지) 값
const int LR_HOME = 90;
const int FB_HOME = 90;
const int T_SERVO_WAIT = 1000; // 복귀 후 대기 시간

// ----- 앞뒤 diff 기준 -----
const float diff_LOW  = 115.3;  // mm
const float diff_HIGH = 128.5;  // mm

// 이동평균 창 크기
const int WINDOW = 5;

// d1, d2 이동평균용 버퍼
float d1Buf[WINDOW];
float d2Buf[WINDOW];
int d1Index = 0, d2Index = 0;
int d1Count = 0, d2Count = 0;

// ----- 좌우 sum 기준 -----
const float SUM_LOW  = 82.3;  // mm
const float SUM_HIGH = 88.7;  // mm

// ==========================================
// 3. 시스템 변수 정의
// ==========================================
enum State {
  S00_WAIT_SIGNAL, 
  S01_WAIT_INPUT,  
  S1_APPROVING,
  S2_APPROVED,
  S3_ALIGNING,
  S4_CHARGING,
  S5_COMPLETED
};
State currentState = S00_WAIT_SIGNAL;
bool isStateFirstRun = true; // 상태 진입 플래그

// 충전 변수
float  Q_target_mAh = 0.0;
double Q_accum_As   = 0.0;
double Q_accum_mAh  = 0.0;
unsigned long lastCurrentMeasureTime = 0;

// UI 변수
unsigned long stateStartTime   = 0;
unsigned long lastScreenSwitch = 0;
int screenMode = 1;
int lastAnimSec = -1;

// ==========================================
// 4. 공통 문자열/plate 처리 함수
// ==========================================
String plateTailDigits();

String normalizedPlate() {
  String plate = plateText;
  plate.trim();
  if (plate.length() == 0) plate = "UNKNOWN";
  if (plate.length() > 10) plate = plate.substring(0, 10);
  return plate;
}

// 번호판에서 뒤쪽 숫자 4자리만 추출 (없으면 UNKNOWN)
String plateTailDigits() {
  String digits = "";
  for (int i = plateText.length() - 1; i >= 0 && digits.length() < 4; i--) {
    if (isDigit(plateText[i])) {
      digits = plateText[i] + digits;
    }
  }
  if (digits.length() == 0) {
    return "UNKNOWN";
  }
  return digits;
}

bool hasDigit(const String& text) {
  for (unsigned int i = 0; i < text.length(); i++) {
    if (isDigit(text[i])) return true;
  }
  return false;
}

// 시리얼에서 한 줄 받아 번호판으로 인식 가능한지 확인
bool trySetPlateFromLine(const String& line) {
  String msg = line;
  msg.trim();
  if (msg.length() < 3) return false;

  String upper = msg;
  upper.toUpperCase();

  if (upper.startsWith("PLATE:")) {
    msg = msg.substring(6);
    msg.trim();
  }

  // 4자리 숫자만 보내는 경우도 지원
  bool hasDigitOnly = true;
  for (unsigned int i = 0; i < msg.length(); i++) {
    if (!isDigit(msg[i]) && msg[i] != ' ') {
      hasDigitOnly = false;
      break;
    }
  }
  if (!hasDigitOnly && !upper.startsWith("PLATE:")) {
    return false;
  }

  plateText = msg;
  plateText.trim();
  Serial.print(">> Plate received: ");
  Serial.println(plateText);
  Serial.print(">> Plate tail: ");
  Serial.println(plateTailDigits());
  return true;
}

void showPlateStatus(const char* statusLine) {
  lcd1.clear();
  lcd2.clear();
  lcd1.setCursor(0, 0);
  lcd1.print("HELLO ");
  lcd1.print(plateTailDigits());
  lcd2.setCursor(0, 0);
  lcd2.print(statusLine);
}

// 시리얼 공통 처리 (어느 상태에서든)
void handleSerialCommands() {
  if (Serial.available() <= 0) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (trySetPlateFromLine(line)) {
    if (currentState == S00_WAIT_SIGNAL) {
      changeState(S01_WAIT_INPUT);
    }
    return;
  }

  String upper = line;
  upper.toUpperCase();
  if (upper == "START") {
    changeState(S01_WAIT_INPUT);
    return;
  }
  if (upper == "BUZZER" || upper == "ALERT" || upper == "MATCHFAIL") {
    buzzAlert();
  }
}

// ==========================================
// 5. 초음파 / 전류 센서 함수
// ==========================================

// 공통 초음파 측정 함수 (단위: mm)
float readUltrasonicMM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 20000);  // 타임아웃 20ms

  if (duration == 0) {
    return 9999.0;  // 측정 실패 시 큰 값
  }

  // 속도 ~343 m/s → 0.343 mm/µs
  // 편도라서 /2 → 0.1715 mm/µs
  float distance_mm = duration * 0.1715;
  return distance_mm;
}

float readCurrentSensor() {
  int raw = analogRead(PIN_CURRENT);
  float voltage = (raw / 1023.0) * 5.0;
  // 센서 영점(2.5V) 미세 조정 필요 시 2.5를 변경
  float amps = (voltage - 2.5) / 0.185;

  amps = abs(amps);
  // 아주 작은 전류는 노이즈로 간주 (예: 50mA 미만)
  if (amps < 0.05) {
    amps = 0.0;
  }
  return amps;
}

// ==========================================
// 6-1. 부저 제어
// ==========================================
void buzzAlert() {
  // 능동 부저: 0.5초 ON / 0.5초 OFF 3회 울림
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(500);
    digitalWrite(PIN_BUZZER, LOW);
    delay(500);
  }
}

// ==========================================
// 6. 상태 제어 공통 함수
// ==========================================
void changeState(State newState) {
  currentState = newState;
  stateStartTime = millis();
  isStateFirstRun = true;
  lastAnimSec = -1;
  lastScreenSwitch = 0;
  screenMode = 1;
  lcd1.clear();
  lcd2.clear();
}

void resetSystem() {
  digitalWrite(PIN_LED_R, LOW);
  digitalWrite(PIN_LED_O, LOW);
  digitalWrite(PIN_LED_G, LOW);

  // 서보 원위치 (정지 위치)
  servoLR.write(LR_HOME);
  delay(800);
  servoFB.write(FB_HOME);
  delay(800);

  Q_accum_As = 0;
  Q_accum_mAh = 0;
  Q_target_mAh = 0;
  changeState(S00_WAIT_SIGNAL);
}

// ==========================================
// 7. FB/LR 자동 정렬 루틴 (두 번째 코드 통합)
// ==========================================

void runAutoAlignFB_LR(unsigned long maxDurationMs) {
  unsigned long start = millis();
  int stableCount = 0;
  // 진행 상황 표시
  lcd1.clear();
  lcd2.clear();
  lcd1.setCursor(0, 0); lcd1.print("HELLO ");
  lcd1.print(plateTailDigits());
  lcd2.setCursor(0, 0); lcd2.print("ALIGN FB/LR...");

  // ★ 이동평균 버퍼/인덱스 리셋
  d1Index = d2Index = 0;
  d1Count = d2Count = 0;
  for (int i = 0; i < WINDOW; i++) {
    d1Buf[i] = 0;
    d2Buf[i] = 0;
  }

  while (millis() - start < maxDurationMs) {
    // =========================
    // 1. 앞뒤 축 제어 (diff + 이동평균)
    // =========================
    float d1_raw = readUltrasonicMM(TRIG1, ECHO1);
    d1Buf[d1Index] = d1_raw;
    d1Index = (d1Index + 1) % WINDOW;
    if (d1Count < WINDOW) d1Count++;

    float d1 = 0.0;
    for (int i = 0; i < d1Count; i++) {
      d1 += d1Buf[i];
    }
    d1 /= d1Count;

    float d2_raw = readUltrasonicMM(TRIG2, ECHO2);
    d2Buf[d2Index] = d2_raw;
    d2Index = (d2Index + 1) % WINDOW;
    if (d2Count < WINDOW) d2Count++;

    float d2 = 0.0;
    for (int i = 0; i < d2Count; i++) {
      d2 += d2Buf[i];
    }
    d2 /= d2Count;

    float diff = d1 - d2;

    Serial.print("[FB] d1(avg): ");
    Serial.print(d1);
    Serial.print("  d2(avg): ");
    Serial.print(d2);
    Serial.print("  diff: ");
    Serial.println(diff);

    if (diff > diff_HIGH) {
      // 너무 멀다 → 한쪽 방향으로 조금 이동
      servoFB.write(83);
      delay(300);
      servoFB.write(FB_HOME);  // 정지
      Serial.println("sexfp");
    } else if (diff < diff_LOW) {
      // 너무 가깝다 → 반대 방향으로 조금 이동
      servoFB.write(100);
      delay(300);
      servoFB.write(FB_HOME);
      Serial.println("pornfp");
    } else {
      servoFB.write(FB_HOME);
      Serial.println("ssibalfp");
      delay(200);
    }

    // =========================
    // 2. 좌우 축 제어 (sum)
    // =========================
    float d3 = readUltrasonicMM(TRIG3, ECHO3);
    float d4 = readUltrasonicMM(TRIG4, ECHO4);
    float sum = d3 + d4;

    Serial.print("[LR] d3: ");
    Serial.print(d3);
    Serial.print("  d4: ");
    Serial.print(d4);
    Serial.print("  sum: ");
    Serial.println(sum);

    if (sum > SUM_HIGH) {
      // 너무 멀다 → 한쪽 방향으로 조금 이동
      servoLR.write(100);
      delay(300);
      servoLR.write(LR_HOME);
      Serial.println("sexlr");
    } else if (sum < SUM_LOW) {
      // 너무 가깝다 → 반대 방향으로 조금 이동
      servoLR.write(80);
      delay(300);
      servoLR.write(LR_HOME + 1);  // 네가 쓰던 91 유지
      Serial.println("pornlr");
    } else {
      servoLR.write(LR_HOME);
      Serial.println("ssiballr");
      delay(200);
    }

    // =========================
    // 3. 정렬 완료 판정 (몇 번 연속으로 범위 안이면 종료)
    // =========================
    if (diff >= diff_LOW && diff <= diff_HIGH &&
        sum  >= SUM_LOW  && sum  <= SUM_HIGH) {
      stableCount++;
    } else {
      stableCount = 0;
    }

    if (stableCount >= 3) {  // 3번 연속 OK면 정렬 완료로 판단
      Serial.println(">> FB/LR align stable.");
      lcd1.clear(); lcd2.clear();
      lcd1.setCursor(0, 0); lcd1.print("HELLO ");
      lcd1.print(plateTailDigits());
      lcd2.setCursor(0, 0); lcd2.print("ALIGN STABLE");
      break;
    }

    // 전체 여유
    delay(200);
  }

  // 안전하게 정지
  servoFB.write(FB_HOME);
  servoLR.write(LR_HOME);
}

// ==========================================
// S5용 정지 위치(Final Home) 정렬 루틴 (d1, d4 기준)
//  - d1(TRIG1/ECHO1): 88~93 mm
//  - d4(TRIG4/ECHO4): 49~53 mm
// ==========================================
void runReturnToHomeByD1D4(unsigned long maxDurationMs) {
  const float D1_LOW  = 88.0;
  const float D1_HIGH = 93.0;
  const float D4_LOW  = 49.0;
  const float D4_HIGH = 53.0;

  unsigned long start = millis();
  int stableCount = 0;

  lcd1.clear(); lcd2.clear();
  lcd1.setCursor(0, 0); lcd1.print("HELLO ");
  lcd1.print(plateTailDigits());
  lcd2.setCursor(0, 0); lcd2.print("HOME ALIGN...");

  while (millis() - start < maxDurationMs) {
    float d1 = readUltrasonicMM(TRIG1, ECHO1);
    float d4 = readUltrasonicMM(TRIG4, ECHO4);

    Serial.print("[HOME] d1: ");
    Serial.print(d1);
    Serial.print("  d4: ");
    Serial.println(d4);

    bool d1_ok = (d1 >= D1_LOW && d1 <= D1_HIGH);
    bool d4_ok = (d4 >= D4_LOW && d4 <= D4_HIGH);

    // ---------- 앞뒤(FB)축: d1 기준 ----------
    if (!d1_ok) {
      if (d1 > D1_HIGH) {
        // 너무 멀다 → 기존 FB에서 diff_HIGH 쪽에 쓰던 값
        servoFB.write(83);
        delay(300);
        servoFB.write(FB_HOME);
      } else if (d1 < D1_LOW) {
        // 너무 가깝다 → 반대 방향
        servoFB.write(100);
        delay(300);
        servoFB.write(FB_HOME);
      }
    } else {
      servoFB.write(FB_HOME);
    }

    // ---------- 좌우(LR)축: d4 기준 ----------
    if (!d4_ok) {
      if (d4 > D4_HIGH) {
        // 너무 멀다 → 기존 sum>HIGH 때 쓰던 방향
        servoLR.write(100);
        delay(300);
        servoLR.write(LR_HOME);
      } else if (d4 < D4_LOW) {
        // 너무 가깝다 → 반대 방향
        servoLR.write(80);
        delay(300);
        servoLR.write(LR_HOME + 1);  // 기존 91 유지
      }
    } else {
      servoLR.write(LR_HOME);
    }

    // ---------- 안정 판정 ----------
    if (d1_ok && d4_ok) {
      stableCount++;
    } else {
      stableCount = 0;
    }

    if (stableCount >= 3) {
      Serial.println(">> HOME 위치(d1,d4) 안정.");
      lcd1.clear(); lcd2.clear();
      lcd1.setCursor(0, 0); lcd1.print("HELLO ");
      lcd1.print(plateTailDigits());
      lcd2.setCursor(0, 0); lcd2.print("HOME STABLE");
      break;
    }

    delay(250);  // 한 번 움직이고 잠깐 대기
  }

  // 최종 정지
  servoFB.write(FB_HOME);
  servoLR.write(LR_HOME);
}

// ==========================================
// 8. 상태별 동작 선언
// ==========================================
void handle_S00_WaitSignal();
void handle_S01_WaitInput();
void handle_S1_Approving();
void handle_S2_Approved();
void handle_S3_Aligning();
void handle_S4_Charging();
void handle_S5_Completed();

// ==========================================
// 9. 초기화 (Setup)
// ==========================================
void setup() {
  Serial.begin(9600);         // 기존 인터페이스 유지
  Serial.setTimeout(50);

  lcd1.init();
  lcd1.backlight();
  lcd2.init();
  lcd2.backlight();

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_O, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // 초음파 핀 설정
  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);

  pinMode(TRIG3, OUTPUT);
  pinMode(ECHO3, INPUT);
  pinMode(TRIG4, OUTPUT);
  pinMode(ECHO4, INPUT);

  // 서보 초기화
  servoFB.attach(PIN_SERVO_FB);
  servoLR.attach(PIN_SERVO_LR);
  servoFB.write(FB_HOME);
  servoLR.write(LR_HOME);

  lcd1.setCursor(0, 0);
  lcd1.print("SYSTEM BOOT...");
  lcd2.setCursor(0, 0);
  lcd2.print("INIT...");
  delay(500);
  resetSystem();
}

// ==========================================
// 10. 메인 루프
// ==========================================
void loop() {
  // 상태별 로직 전에 언제나 시리얼 명령을 확인
  handleSerialCommands();

  switch (currentState) {
    case S00_WAIT_SIGNAL: handle_S00_WaitSignal(); break;
    case S01_WAIT_INPUT:  handle_S01_WaitInput();  break;
    case S1_APPROVING:    handle_S1_Approving();   break;
    case S2_APPROVED:     handle_S2_Approved();    break;
    case S3_ALIGNING:     handle_S3_Aligning();    break;
    case S4_CHARGING:     handle_S4_Charging();    break;
    case S5_COMPLETED:    handle_S5_Completed();   break;
  }
}

// ==========================================
// 11. 상태별 상세 동작
// ==========================================

// S00: 차량 감지 대기
void handle_S00_WaitSignal() {
  if (isStateFirstRun) {
    lcd1.clear();
    lcd2.clear();
    lcd1.setCursor(0, 0); lcd1.print("WAITING SIGNAL..");
    lcd2.setCursor(0, 0); lcd2.print("Ready to Detect");
    Serial.println("\n[S00] 차량 감지 대기중... (아무 키나 누르세요)");
    isStateFirstRun = false;
  }
}

// S01: 목표 충전량 입력 대기
void handle_S01_WaitInput() {
  if (isStateFirstRun) {
    lcd1.clear();
    lcd2.clear();
    lcd1.setCursor(0, 0); lcd1.print("VEHICLE DETECTED");
    lcd2.setCursor(0, 0); lcd2.print("Input Target Q");
    Serial.println("\n[S01] 목표 충전량(mAh) 입력 (0=디버그):");
    isStateFirstRun = false;
  }

  if (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (trySetPlateFromLine(line)) return;

    String payload = line;
    String upper = payload;
    upper.toUpperCase();
    if (upper.startsWith("TARGET:")) {
      payload = payload.substring(7);
      payload.trim();
    }

    if (!hasDigit(payload)) {
      Serial.println(">> 입력에 숫자가 없어 무시.");
      return;
    }

    float inputVal = payload.toFloat();
    if (inputVal < 0) inputVal = 0;
    Q_target_mAh = inputVal;

    Serial.print(">> 설정된 목표치: ");
    Serial.print((int)Q_target_mAh);
    Serial.println(" mAh");
    changeState(S1_APPROVING);
  }
}

// S1: 승인 대기 (10초)
void handle_S1_Approving() {
  if (isStateFirstRun) {
    digitalWrite(PIN_LED_R, HIGH);
    Serial.println("\n[S1] 승인 중...");
    isStateFirstRun = false;
  }

  int elapsedSec = (millis() - stateStartTime) / 1000;
  if (elapsedSec >= 10) {
    digitalWrite(PIN_LED_R, LOW);
    changeState(S2_APPROVED);
    return;
  }

  if (elapsedSec != lastAnimSec) {
    lastAnimSec = elapsedSec;
    lcd1.clear();
    lcd2.clear();
    lcd1.setCursor(0, 0); lcd1.print("APPROVING");
    for (int i = 0; i < (elapsedSec % 4); i++) lcd1.print(".");
    lcd2.setCursor(0, 0);
    lcd2.print("Please Wait ");
    lcd2.print(10 - elapsedSec);
    lcd2.print("s");
  }
}

// S2: 승인 완료 (5초)
void handle_S2_Approved() {
  if (isStateFirstRun) {
    digitalWrite(PIN_LED_G, HIGH);
    lcd1.clear();
    lcd2.clear();
    lcd1.setCursor(0, 0); lcd1.print("HELLO ");
    lcd1.print(plateTailDigits());
    lcd2.setCursor(0, 0); lcd2.print("APPROVED!");
    lcd2.setCursor(0, 1); lcd2.print("Ready to Align");
    Serial.println("\n[S2] 승인 완료.");
    isStateFirstRun = false;
  }

  if (millis() - stateStartTime >= 5000) {
    digitalWrite(PIN_LED_G, LOW);
    changeState(S3_ALIGNING);
  }
}

// S3: 정렬 (FB/LR 폐루프 정렬 사용)
void handle_S3_Aligning() {
  if (isStateFirstRun) {
    Serial.println("\n[S3] 정렬 시작...");
    isStateFirstRun = false;

    showPlateStatus("ALIGNING...");
    // FB/LR 정렬 루틴 실행 (최대 20초, 그 안에 안정되면 조기 종료)
    runAutoAlignFB_LR(20000);

    showPlateStatus("ALIGN COMPLETE");
    Serial.println(">> 정렬 완료. 5초 후 충전.");
    delay(5000);

    lastCurrentMeasureTime = millis();
    changeState(S4_CHARGING);
  }
}

// S4: 충전
void handle_S4_Charging() {
  if (isStateFirstRun) {
    digitalWrite(PIN_LED_O, HIGH);
    Serial.println("\n[S4] 충전 시작.");
    isStateFirstRun = false;
  }

  unsigned long currentMillis = millis();
  double dt = (currentMillis - lastCurrentMeasureTime) / 1000.0;
  lastCurrentMeasureTime = currentMillis;

  float currentAmps = readCurrentSensor();
  Q_accum_As  += currentAmps * dt;
  Q_accum_mAh  = Q_accum_As / 3.6;

  if (currentMillis - lastScreenSwitch > 3000) {
    lastScreenSwitch = currentMillis;
    screenMode = !screenMode;
    lcd1.clear();
    lcd2.clear();
    if (screenMode == 0) {
      lcd1.setCursor(0, 0); lcd1.print("HELLO ");
      lcd1.print(plateTailDigits());
      lcd2.setCursor(0, 0); lcd2.print("CHARGING...");
      lcd2.setCursor(0, 1);
      if (Q_target_mAh == 0) lcd2.print("[DEBUG MODE]");
      else lcd2.print("Do Not Touch");
    } else {
      lcd1.setCursor(0, 0); lcd1.print("HELLO ");
      lcd1.print(plateTailDigits());
      lcd2.setCursor(0, 0); lcd2.print("Cur:");
      lcd2.print((int)Q_accum_mAh);
      lcd2.print("mAh");
      lcd2.setCursor(0, 1); lcd2.print("Tgt:");
      lcd2.print((int)Q_target_mAh);
      lcd2.print("mAh");
    }
    Serial.print("I: ");
    Serial.print(currentAmps);
    Serial.print(" A | Q: ");
    Serial.println(Q_accum_mAh);
  }

  bool isFinished = false;
  if (Q_target_mAh == 0) { // 디버그 모드
    if (millis() - stateStartTime >= 10000) isFinished = true;
  } else {
    if (Q_accum_mAh >= Q_target_mAh) isFinished = true;
    // 안전 장치: 목표가 있어도 15초 넘으면 종료해 다음 상태로 진행
    if (millis() - stateStartTime >= 15000) isFinished = true;
  }

  if (isFinished) {
    digitalWrite(PIN_LED_O, LOW);
    changeState(S5_COMPLETED);
  }
}

// S5: 완료 및 복귀 (d1,d4 기준 홈 포지션 정렬 포함)
void handle_S5_Completed() {
  if (isStateFirstRun) {
    digitalWrite(PIN_LED_G, HIGH);
    lcd1.clear();
    lcd2.clear();
    lcd1.setCursor(0, 0); lcd1.print("CHARGE COMPLETE!");
    lcd2.setCursor(0, 0); lcd2.print("BYE ");
    lcd2.print(plateTailDigits());
    Serial.println("\n[S5] 충전 완료. 복귀 중...");
    isStateFirstRun = false;
  }

  if (millis() - stateStartTime > 2000) {
    // d1,d4 기준으로 정지 위치 맞추기
    showPlateStatus("PARKING...");
    runReturnToHomeByD1D4(16000);   // 최대 16초 안에 맞추기 시도

    showPlateStatus("PARKED");
    delay(2000);

    resetSystem();
  }
}
