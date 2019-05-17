#define useCANx
#define useI2Cx
#define useHallSpeedx
#define useWatchdogx
#define COMPLEMENTARYPWMx
#define DEV
#define SENSORLESS
#define ADCBODGE

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
Metro checkFaultTimer(100);
volatile uint8_t recentWriteState;

volatile uint16_t throttle = 0;
volatile bool dir = false;

volatile commutateMode_t commutateMode = MODE_HALL;

volatile uint32_t timeToUpdateCmp = 0;

void setup(){
  setupWatchdog();
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

  kickDog();

  #ifdef useHallSpeed
    attachInterrupt(HALL_SPEED, hallSpeedISR, RISING);
  #endif
}

void loop(){
  
  uint32_t curTime = millis();
  uint32_t curTimeMicros = micros();
  if (curTimeMicros > timeToUpdateCmp) {
    timeToUpdateCmp = curTimeMicros + 1000;
    updateCmp_ADC();
  }
  
  #ifdef useCAN
    getThrottle_CAN();
  #endif

  if (curTime - lastTime_throttle > 5)
  {
    #ifdef useI2C
    if (curTime - lastTime_I2C < 300){
      throttle = getThrottle_I2C();
      if (throttle == 0)
        throttle = getThrottle_analog() * 4095;
    } else {
      throttle = getThrottle_analog() * 4095;
    }
    #elif defined(useCAN)
    throttle = 4096*pow(getThrottle_CAN()/4096.0,3);
    if ((curTime - lastTime_CAN > 300) || (throttle==0)){
      throttle = getThrottle_analog() * 4095;
    }
    #else
      #ifdef COMPLEMENTARYPWM
      throttle = getThrottle_analog() * MODULO;
      #else
      throttle = getThrottle_analog() * 4096;
      #endif
    #endif

    #ifdef useHallSpeed
      float hallSpeed_tmp = min(hallSpeed_prev_mps,
                                DIST_PER_TICK * 1e6 / (micros()-lastTime_hallSpeed_us)); // allows vel to approach 0
      hallSpeed_LPF_mps = (hallSpeed_alpha)*hallSpeed_LPF_mps + (1-hallSpeed_alpha)*hallSpeed_tmp;
    #endif

    hallISR();
    lastTime_throttle = curTime;
    Serial.print(getThrottle_ADC());
    Serial.print("\t");
    Serial.print(throttle);
    Serial.print("\t");
    Serial.print(recentWriteState);
    Serial.print("\t");
    Serial.print(realPos);
    Serial.print("\t");
    Serial.print(hallOrder[getHalls()]);
    Serial.print("\t");
    Serial.print(commutateMode);
    Serial.print("\t");
    Serial.print(cmpVal);
    Serial.print('\t');
    // Serial.print(speed);
    // Serial.print("\t");
    // Serial.print(m_pll_speed / 360);
    // Serial.print("\t");
    // Serial.print(m_pll_phase);
    // Serial.print('\t');
    // Serial.print(digitalRead(19));
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

    kickDog();
  }

  if (checkFaultTimer.check()){
  }

  // hallISR();

  // delayMicroseconds(100);
}

void commutate_isr(uint8_t phase, commutateMode_t caller) {
  if (caller != commutateMode){
    return;
  }
  // Serial.print("Commutating!\t");
  // Serial.print(phase);
  // Serial.print('\t');
  // Serial.println(throttle);
  if (throttle < (.01*MODULO)){
    writeTrap(0, -1); // writing -1 floats all phases
  } else {
    writeTrap(throttle, phase);
  }
  // if (recentWriteState != phase){
  //   Serial.print(hallOrder[getHalls()]);
  //   Serial.print("\t");
  //   Serial.println(phase);
  // }
  recentWriteState = phase;
  updatePhase_ADC(phase);
  updateDuty_ADC((float)throttle / MODULO);
}