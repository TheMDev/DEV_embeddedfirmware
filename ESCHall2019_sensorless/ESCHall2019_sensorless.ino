#define useCANx
#define useI2Cx
#define useINA226x // warning: not working - it seems like the I2C is messing up the interrupts
#define useINA332x
#define useHallSpeedx
#define useWatchdogx
#define COMPLEMENTARYPWMx
#define DEV
#define SENSORLESS
#define ADCBODGEx
#define OC_LIMIT 1.0 // current limit
#define useTRIGDELAYCOMPENSATION

#define NUMPOLES 2

#define PERIODSLEWLIM_US_PER_S 50000

#if defined(useCAN) && !defined(__MK20DX256__)
  #error "Teensy 3.2 required for CAN"
#endif
#if defined(useCAN) && defined(useI2C)
  #error "strongly discourage using CAN and I2C at the same time"
#endif

#include <i2c_t3.h>
#include "TimerOne.h"
#include "config.h"
#include "MCpwm_2019sensorless.h"
#include "CANCommands.h"
#include "Metro.h"
#include "est_BEMF_delay.h"
#include "est_hall_simple.h"

#ifdef useINA226
  #include "INA.h"
#endif

#ifdef useHallSpeed
  #define DIST_PER_TICK 0.19948525 // (20 in) * pi / (8 ticks/rev) = (0.199 m/tick)
  uint32_t lastTime_hallSpeed_us = 0;
  float hallSpeed_alpha = .95;
  float hallSpeed_prev_mps = 0;
  float hallSpeed_LPF_mps = 0;
  void hallSpeedISR(){
    uint32_t newTime = micros();
    hallSpeed_prev_mps = DIST_PER_TICK * 1e6 / (newTime - lastTime_hallSpeed_us);
    lastTime_hallSpeed_us = newTime;
  }
#endif

uint32_t lastTime_throttle = 0;
uint32_t lastTime_I2C = 0;
uint32_t lastTime_CAN = 0;
float lastDuty_UART = 0;
Metro checkFaultTimer(100);
bool FAULT = false;
Metro printTimer(2500);
volatile uint8_t recentWriteState;

float throttle = 0;
volatile uint16_t duty = 0;
volatile bool dir = true; // true = forwards

volatile commutateMode_t commutateMode = MODE_HALL;
inputMode_t inputMode = INPUT_THROTTLE;
#ifdef SENSORLESS
  bool useSensorless = true;
#else
  bool useSensorless = false;
#endif

volatile uint32_t timeToUpdateCmp = 0;

volatile uint32_t period_commutation_usPerTick = 1e6;
volatile int16_t phaseAdvance_Q10 = 0;

controlMode_t controlMode = CONTROL_DUTY;
float Kv = 188;
float Rs = 0.2671;
float maxCurrent = 5, minCurrent = 0;
float rpm = 0, Vbus = 0;

void setup(){
  setupWatchdog();
  Vbus = getBusVoltage();
  setupPWM();
  setupPins();
  setup_hall();
  #ifdef SENSORLESS
    setupADC();
  #endif
  kickDog();
  #ifdef useCAN
    setupCAN();
  #endif
  #ifdef useINA226
    INAinit();
  #endif

  kickDog();

  #ifdef useHallSpeed
    attachInterrupt(HALL_SPEED, hallSpeedISR, RISING);
  #endif
  pinMode(0, OUTPUT);
  pinMode(1, OUTPUT);

  printHelp();
}

void loop(){
  
  uint32_t curTime = millis();
  uint32_t curTimeMicros = micros();
  if (curTimeMicros > timeToUpdateCmp) {
    timeToUpdateCmp = curTimeMicros + 1000;
    updateCmp_BEMFdelay();
  }
  if ((!delayCommutateFinished) && (micros() >= delayCommutateTimer)){
    delayCommutate_isr();
  }

  updateBEMFdelay(curTimeMicros);

  #ifdef useCAN
    getThrottle_CAN();
  #endif

  if (printTimer.check()) {
    printDebug(curTime);
  }

  if (checkFaultTimer.check()){
  }

  curTimeMicros = micros();
  commutateMode_t tmp = commutateMode;
  switch (tmp) {
    case MODE_SENSORLESS_DELAY: {
      uint32_t tmp = prevTickTime_BEMFdelay;
      bool BEMFdelay_isValid = (tmp > curTimeMicros) || ((curTimeMicros - tmp) < (1.3 * period_bemfdelay_usPerTick));
      
      if (!BEMFdelay_isValid){
        commutateMode = MODE_HALL;
        hallnotISR();
        digitalWrite(13, commutateMode != MODE_HALL);
        Serial.print("BEMF crossing appears to have been missed\t");
        Serial.print(BEMFdelay_isValid); Serial.print('\t');
        Serial.print(curTimeMicros); Serial.print('\t');
        Serial.print(tmp); Serial.print('\t');
        Serial.print((int32_t)(curTimeMicros-tmp)); Serial.print('\t');
        Serial.print((period_bemfdelay_usPerTick)); Serial.print('\t');
        Serial.print((1.3 * period_bemfdelay_usPerTick)); Serial.print('\n');
        // Serial.print(period_bemfdelay_usPerTick); Serial.print('\n');
      } else {
        uint32_t percentSpeedSenseDiff = 100*period_bemfdelay_usPerTick/period_hallsimple_usPerTick;
        if ((percentSpeedSenseDiff < 60) || (percentSpeedSenseDiff > 140)){
          commutateMode = MODE_HALL;
          hallnotISR();
          digitalWrite(13, commutateMode != MODE_HALL);
          Serial.println("Transitioned out of sensorless");
        }
      }
      break;
    }
    case MODE_HALL: {
      uint32_t percentSpeedSenseDiff = 100*period_bemfdelay_usPerTick/period_hallsimple_usPerTick;
      if (useSensorless && (percentSpeedSenseDiff > 90) && (percentSpeedSenseDiff < 110)){
        commutateMode = MODE_SENSORLESS_DELAY;
        digitalWrite(13, commutateMode != MODE_HALL);
        Serial.println("Transitioned into sensorless");
      }
      break;
    }
    default: {
      commutateMode = MODE_HALL;
      break;
    }
  }

  // if ((period_commutation_usPerTick < 3000) && (commutateMode == MODE_HALL)){
  //   commutateMode = MODE_SENSORLESS_DELAY;
  //   // digitalWriteFast(13, HIGH);
  // } else if ((period_commutation_usPerTick > 3500) && (commutateMode == MODE_SENSORLESS_DELAY)){
  //   commutateMode = MODE_HALL;
  //   // digitalWriteFast(13, LOW);
  // }

  // hallnotISR();

  // delayMicroseconds(100);

  if (curTime - lastTime_throttle > 5)
  {
    // #ifdef useI2C
    // if (curTime - lastTime_I2C < 300){
    //   throttle = getThrottle_I2C();
    //   if (throttle == 0)
    //     throttle = getThrottle_analog() * 4095;
    // } else {
    //   throttle = getThrottle_analog() * 4095;
    // }
    // #elif defined(useCAN)
    // throttle = 4096*pow(getThrottle_CAN()/4096.0,3);
    // if ((curTime - lastTime_CAN > 300) || (throttle==0)){
    //   throttle = getThrottle_analog() * 4095;
    // }
    // #else

    rpm += 0.9*(60e6*1.0/period_hallsimple_usPerTick/6/NUMPOLES - rpm);
    rpm = constrain(rpm, 0, 6000);
    if (duty > (0.15*4096))
      Vbus = 3.3 * vsx_cnts[highPhase] / (1<<ADC_RES_BITS) * (39.2e3 / 3.32e3) * 16.065/14.77;

    switch (inputMode) {
      case INPUT_THROTTLE:
        throttle = getThrottle_analog(); // * 4096;
        break;
      case INPUT_UART:
        throttle = lastDuty_UART; // * 4096;
        break;
      default:
        throttle = 0;
        break;
    }

    switch (controlMode) {
      float I;
      case CONTROL_DUTY:
        duty = throttle * 4096;
        break;
      case CONTROL_CURRENT_OPENLOOP:
        I = map(throttle, 0, 1, minCurrent, maxCurrent);
        // I = (Vbus*D - rpm/Kv) / Rs
        duty = constrain((uint16_t)((I*Rs + rpm/Kv) / Vbus * 4096), 0, 4096);
        break;
    }
    // #endif

    #ifdef useHallSpeed
      float hallSpeed_tmp = min(hallSpeed_prev_mps,
                                DIST_PER_TICK * 1e6 / (micros()-lastTime_hallSpeed_us)); // allows vel to approach 0
      hallSpeed_LPF_mps = (hallSpeed_alpha)*hallSpeed_LPF_mps + (1-hallSpeed_alpha)*hallSpeed_tmp;
    #endif

    #ifdef useINA226
      updateINA();
    #endif

    hallnotISR();
    lastTime_throttle = curTime;

    kickDog();

    digitalWrite(13, commutateMode != MODE_HALL);

    readSerial();
  }
  if (ADCsampleDone) {
    Serial.println("********************************");
    for (uint16_t i = 0; i<ADCSAMPLEBUFFERSIZE; i++) {
      for (uint16_t j = 0; j<(sizeof(vsxSamples_cnts[0])/sizeof(vsxSamples_cnts[0][0])); j++) {
        Serial.print(vsxSamples_cnts[i][j]); Serial.print('\t');
      }
      Serial.println();
    }
    Serial.println("********************************");
    ADCsampleDone = false;
  }
}

void printDebug(uint32_t curTime) {
  Serial.print(curTime);
  Serial.print('\t');
  #ifdef useINA226
    Serial.print(InaVoltage_V);
    Serial.print('\t');
    Serial.print(InaCurrent_A);
    Serial.print('\t');
    Serial.print(InaPower_W);
    Serial.print('\t');
    Serial.print(InaEnergy_J);
    Serial.print('\t');
  #endif
  Serial.print(inputMode);
  Serial.print('\t');
  Serial.print(getThrottle_ADC());
  Serial.print('\t');
  Serial.print(throttle);
  Serial.print('\t');
  Serial.print(duty);
  Serial.print('\t');
  Serial.print(recentWriteState);
  Serial.print('\t');
  Serial.print(hallOrder[getHalls()]);
  Serial.print('\t');
  Serial.print(commutateMode);
  Serial.print('\t');
  Serial.print(phaseAdvance_Q10 / 1024.0);
  Serial.print('\t');
  Serial.print(rpm);
  Serial.print('\t');
  Serial.print(period_hallsimple_usPerTick);
  Serial.print('\t');
  // Serial.print(delayCommutateTimer-micros());
  // Serial.print('\t');
  Serial.print(period_bemfdelay_usPerTick);
  Serial.print('\t');
  Serial.print(period_commutation_usPerTick);
  Serial.print('\t');
  Serial.print(Vbus);
  Serial.print('\t');
  Serial.print(vsx_cnts[0]);
  Serial.print('\t');
  Serial.print(vsx_cnts[1]);
  Serial.print('\t');
  Serial.print(vsx_cnts[2]);
  Serial.print('\t');

  #ifdef useHallSpeed
  Serial.print('\t');
  Serial.print(digitalRead(HALL_SPEED));
  Serial.print('\t');
  Serial.print('\t');
  Serial.print(hallSpeed_LPF_mps,3);
  #endif

  // Serial.print('\t');
  // Serial.print(analogRead(VS_A));
  // Serial.print('\t');
  // Serial.print(analogRead(VS_B));
  // Serial.print('\t');
  // Serial.print(analogRead(VS_C));
  Serial.print('\n');
}
void printHelp() {
  Serial.println("*******************");
  Serial.println("* SERIAL COMMANDS *");
  Serial.println("*******************");
  Serial.println("h - print this help menu");
  Serial.println("r - clear a fault condition");
  Serial.println("u - switch to UART controlled throttle");
  Serial.println("t - switch to ADC controlled throttle");
  Serial.println("i - switch to I2C controlled throttle");
  Serial.println("c - switch to CAN controlled throttle");
  Serial.println("s - toggle between synchronous vs nonsynchronous switching");
  Serial.println("S - toggle between sensorless and no sensorless");
  Serial.println("P - switch to duty cycle control");
  Serial.println("I - switch to current control");
  Serial.println("d - specify the interval to print debug data");
  Serial.println("o - sample the ADC data super fast");
  Serial.println("D# - set the duty cycle to # if in UART throttle mode (i.e. D0.3 sets 30% duty cycle");
  Serial.println("a# - set the phase advance to # percent when in sensorless (i.e. a30 sets the phase advance to 30%");
  Serial.println("-------------------");
}
void readSerial() {
  if (Serial.available()){
    char input = Serial.read();
    float valInput;
    switch (input) {
      case 'h':
        printHelp();
        break;
      case 'r':
        FAULT = false;
        break;
      case 'u':
        inputMode = INPUT_UART;
        break;
      case 't':
        inputMode = INPUT_THROTTLE;
        break;
      case 'i':
        inputMode = INPUT_I2C;
        break;
      case 'c':
        inputMode = INPUT_CAN;
        break;
      case 's':
        switch(pwmMode) {
          case PWM_COMPLEMENTARY:
            pwmMode = PWM_NONSYNCHRONOUS;
            break;
          case PWM_NONSYNCHRONOUS:
            pwmMode = PWM_COMPLEMENTARY;
            break;
        }
        break;
      case 'S':
        useSensorless = !useSensorless;
        if (!useSensorless){
          commutateMode = MODE_HALL;
        }
        break;
      case 'I':
        controlMode = CONTROL_CURRENT_OPENLOOP;
        break;
      case 'P':
        controlMode = CONTROL_DUTY;
        break;
      case 'd':
        printTimer.interval(Serial.parseInt());
        break;
      case 'o':
        ADCsampleCollecting = true;
        break;
      case 'D':
        if (inputMode == INPUT_UART) {
          valInput = Serial.parseFloat();
          dir = (valInput >= 0);
          valInput = fabsf(valInput);
          lastDuty_UART = constrain(valInput, 0, 1);
        }
        break;
      case 'a':
        valInput = Serial.parseFloat();
        phaseAdvance_Q10 = constrain(valInput, -100, 100) / 100.0 * (1<<10);
        break;
      case 'K':
        Kv = Serial.parseFloat();
        break;
      case 'R':
        Rs = Serial.parseFloat();
        break;
    }
  }
}

void INAOC_isr() {
  // writeTrap(0, -1);
  // FAULT = true;
}
void commutate_isr(uint8_t phase, commutateMode_t caller) {
  if (caller != commutateMode){
    // Serial.print("tried to commutate by ");
    // Serial.println(caller);
    return;
  }
  static uint32_t lastTickTimeCom = 0;
  uint32_t curTimeMicrosCom = micros();
  uint32_t elapsedTimeCom = curTimeMicrosCom - lastTickTimeCom;
  period_commutation_usPerTick = min(constrain(
      elapsedTimeCom,
      period_commutation_usPerTick - (PERIODSLEWLIM_US_PER_S*elapsedTimeCom/1e6),
      period_commutation_usPerTick + (PERIODSLEWLIM_US_PER_S*elapsedTimeCom/1e6)),
    10000);
  lastTickTimeCom = curTimeMicrosCom;
  // Serial.print("Commutating!\t");
  // Serial.print(phase);
  // Serial.print('\t');
  // Serial.println(duty);
  if (duty < (.01*4096)){
    writeTrap(0, -1); // writing -1 floats all phases
  } else {
    writeTrap(duty, phase);
  }
  // if (recentWriteState != phase){
  //   Serial.print(hallOrder[getHalls()]);
  //   Serial.print('\t');
  //   Serial.println(phase);
  // }
  recentWriteState = phase;
  updatePhase_BEMFdelay(phase);
}