/*
 * ============================================================
 *  MPPT Solar Charge Controller
 *  Algorithm: Perturb & Observe (P&O)
 *  Author   : ELAZZAOUI Toufik 
 *  University: Elhadj Mossa Ag Akhamok - Tamanrasset
 *  Year     : 2023
 * ============================================================
 *  Hardware:
 *    - Arduino Nano (ATmega328)
 *    - PV Panel   : 30W, Vmp=18V, Imp=1.6A, Ncell=72
 *    - Voltage sensor (resistor divider R1=15kΩ, R2=10kΩ)
 *    - Current sensor : ACS712-5A  → A1
 *    - LCD 20x4 I2C   → SDA=A4, SCL=A5
 *    - MOSFET IRF1407 → PWM pin D9
 *    - Boost converter: L=66µH, C1=470µF, C2=100µF
 *    - Diode IN4007
 * ============================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ──────────────────────────────────────────
//  LCD  (adresse I2C 0x27 ou 0x3F)
// ──────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ──────────────────────────────────────────
//  Broches (Pins)
// ──────────────────────────────────────────
const int PIN_VOLTAGE = A0;   // Capteur de tension
const int PIN_CURRENT = A1;   // ACS712-5A
const int PIN_PWM     = 9;    // MOSFET gate (Timer1 - 10-bit PWM)

// ──────────────────────────────────────────
//  Paramètres du capteur de tension
//  Diviseur : R1=15kΩ, R2=10kΩ  →  ratio = (R1+R2)/R2
// ──────────────────────────────────────────
const float R1          = 15000.0;
const float R2          = 10000.0;
const float V_REF       = 5.0;       // Tension de référence Arduino
const float ADC_STEPS   = 1023.0;
const float VOLT_RATIO  = (R1 + R2) / R2;   // = 2.5

// ──────────────────────────────────────────
//  Paramètres ACS712-5A
//  Sensibilité = 185 mV/A, offset = 2.5V
// ──────────────────────────────────────────
const float ACS_SENSITIVITY = 0.185;  // V/A
const float ACS_OFFSET      = 2.5;    // V  (Vcc/2)

// ──────────────────────────────────────────
//  Paramètres P&O
// ──────────────────────────────────────────
const float DELTA_DUTY = 2.0;       // Pas de perturbation (%)
const float DUTY_MIN   = 10.0;      // Duty cycle min (%)
const float DUTY_MAX   = 90.0;      // Duty cycle max (%)
const int   SAMPLE_MS  = 100;       // Période d'échantillonnage (ms)

// ──────────────────────────────────────────
//  Variables globales
// ──────────────────────────────────────────
float dutyCycle  = 50.0;   // Duty cycle initial (%)
float prevPower  = 0.0;
float prevVoltage= 0.0;

unsigned long lastSampleTime = 0;

// ──────────────────────────────────────────
//  Prototypes
// ──────────────────────────────────────────
float readVoltage();
float readCurrent();
void  setPWM(float duty);
void  updateLCD(float v, float i, float p, float d);
void  mpptPandO(float v, float p);

// ══════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════
void setup() {
  Serial.begin(9600);

  // PWM haute fréquence sur pin 9 (Timer1, ~31 kHz)
  TCCR1A = _BV(COM1A1) | _BV(WGM11) | _BV(WGM10);
  TCCR1B = _BV(WGM12)  | _BV(CS10);   // No prescaler → ~31.4 kHz
  pinMode(PIN_PWM, OUTPUT);
  setPWM(dutyCycle);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(3, 0);
  lcd.print("MPPT Controller");
  lcd.setCursor(2, 1);
  lcd.print("P&O Algorithm");
  lcd.setCursor(1, 2);
  lcd.print("Tamanrasset Univ.");
  lcd.setCursor(5, 3);
  lcd.print("2023");
  delay(3000);
  lcd.clear();

  Serial.println("=== MPPT P&O Started ===");
}

// ══════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  if (now - lastSampleTime >= SAMPLE_MS) {
    lastSampleTime = now;

    float v = readVoltage();
    float i = readCurrent();
    float p = v * i;

    // Algorithme P&O
    mpptPandO(v, p);

    // Affichage LCD
    updateLCD(v, i, p, dutyCycle);

    // Moniteur série
    Serial.print("V="); Serial.print(v, 2);
    Serial.print("V  I="); Serial.print(i, 2);
    Serial.print("A  P="); Serial.print(p, 2);
    Serial.print("W  D="); Serial.print(dutyCycle, 1);
    Serial.println("%");
  }
}

// ══════════════════════════════════════════
//  LECTURE TENSION PV
// ══════════════════════════════════════════
float readVoltage() {
  long sum = 0;
  for (int k = 0; k < 10; k++) {
    sum += analogRead(PIN_VOLTAGE);
    delayMicroseconds(100);
  }
  float adcVal  = sum / 10.0;
  float vADC    = adcVal * (V_REF / ADC_STEPS);
  return vADC * VOLT_RATIO;
}

// ══════════════════════════════════════════
//  LECTURE COURANT ACS712
// ══════════════════════════════════════════
float readCurrent() {
  long sum = 0;
  for (int k = 0; k < 10; k++) {
    sum += analogRead(PIN_CURRENT);
    delayMicroseconds(100);
  }
  float adcVal = sum / 10.0;
  float vSens  = adcVal * (V_REF / ADC_STEPS);
  float current = (vSens - ACS_OFFSET) / ACS_SENSITIVITY;
  if (current < 0) current = 0;   // Pas de courant négatif
  return current;
}

// ══════════════════════════════════════════
//  REGLAGE PWM  (0–100 %)
// ══════════════════════════════════════════
void setPWM(float duty) {
  duty = constrain(duty, DUTY_MIN, DUTY_MAX);
  // Timer1 en mode 10-bit : max = 1023
  int ocr = (int)((duty / 100.0) * 1023.0);
  OCR1A = ocr;
}

// ══════════════════════════════════════════
//  ALGORITHME P&O
// ══════════════════════════════════════════
void mpptPandO(float v, float p) {
  float deltaP = p - prevPower;
  float deltaV = v - prevVoltage;

  if (deltaP != 0) {
    if (deltaP > 0) {
      // Puissance augmente → continuer dans le même sens
      if (deltaV > 0)
        dutyCycle += DELTA_DUTY;
      else
        dutyCycle -= DELTA_DUTY;
    } else {
      // Puissance diminue → inverser la direction
      if (deltaV > 0)
        dutyCycle -= DELTA_DUTY;
      else
        dutyCycle += DELTA_DUTY;
    }
  }

  dutyCycle = constrain(dutyCycle, DUTY_MIN, DUTY_MAX);
  setPWM(dutyCycle);

  prevPower   = p;
  prevVoltage = v;
}

// ══════════════════════════════════════════
//  AFFICHAGE LCD 20x4
// ══════════════════════════════════════════
void updateLCD(float v, float i, float p, float d) {
  // Ligne 0 : Tension
  lcd.setCursor(0, 0);
  lcd.print("Vpv:");
  lcd.print(v, 2);
  lcd.print("V          ");

  // Ligne 1 : Courant
  lcd.setCursor(0, 1);
  lcd.print("Ipv:");
  lcd.print(i, 2);
  lcd.print("A          ");

  // Ligne 2 : Puissance
  lcd.setCursor(0, 2);
  lcd.print("Ppv:");
  lcd.print(p, 2);
  lcd.print("W          ");

  // Ligne 3 : Duty Cycle
  lcd.setCursor(0, 3);
  lcd.print("Duty:");
  lcd.print(d, 1);
  lcd.print("%          ");
}
