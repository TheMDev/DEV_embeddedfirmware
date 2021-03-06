// #ifndef EST_BEMF_H
// #define EST_BEMF_H

// #include "ADC_2019sensorless.h"
// #include "config.h"
// volatile uint32_t prevTickTime_BEMFdelay;
// volatile uint32_t period_bemfdelay_usPerTick = 0;
// extern volatile uint32_t period_commutation_usPerTick;

// #include "IntervalTimer.h"
// IntervalTimer delayCommutateTimer;
// IntervalTimer delayMissedTickTimer;
// // volatile uint32_t delayCommutateTimer = 0;
// volatile bool delayCommutateFinished = true;

// volatile void BEMFcrossing_isr(volatile uint16_t vsx_cnts[3]);
// extern void commutate_isr(uint8_t phase, commutateMode_t caller);
// void delayCommutate_isr();
// void missedTick_delay();

// void updateCmp_BEMFdelay();
// void updatePhase_BEMFdelay(uint8_t drivePhase);
// static const uint8_t floatPhases[6] = {2, 0, 1, 2, 0, 1};
// static const uint8_t highPhases[6]  = {0, 2, 2, 1, 1, 0};
// // static const uint8_t floatPhases[6] = {VS_C, VS_A, VS_B, VS_C, VS_A, VS_B};
// // static const uint8_t highPhases[6] = {VS_A, VS_C, VS_C, VS_B, VS_B, VS_A};
// static const bool isRisingEdges[6] = {true, false, true, false, true, false};

// volatile uint8_t floatPhase, highPhase;
// volatile bool isRisingEdge;

// volatile uint8_t curPhase_BEMFdelay;
// volatile uint32_t cmpVal;
// volatile uint32_t intV = 0;
// volatile bool cmpOn = false;

// extern volatile bool dir;
// extern volatile uint8_t curPhase_BEMFdelay;
// volatile uint8_t triggerPhase_delay;

// extern volatile int16_t phaseAdvance_Q10;

// bool leftSensorless = false;
// extern void exitSensorless();

// void updateBEMFdelay(uint32_t curTimeMicros) {
// }

// volatile void BEMFcrossing_isr(volatile uint16_t vsx_cnts[3]) {
// 	static volatile uint32_t prevZCtimes_us[1<<3];
// 	static volatile uint8_t prevZCtimesInd = 0;

// 	if (!delayCommutateFinished || (triggerPhase_delay == curPhase_BEMFdelay)) {
// 		return;
// 	}
// 	uint32_t curTickTime_us = micros();
// 	delayMissedTickTimer.end();

// 	// correction for discrete ADC sampling
// 	#ifdef useTRIGDELAYCOMPENSATION
// 		uint32_t triggerDelay_us = period_bemfdelay_usPerTick * abs((int16_t)(vsx_cnts[floatPhase] - (vsx_cnts[highPhase]>>1))) / vsx_cnts[highPhase];
// 		triggerDelay_us = constrain(triggerDelay_us, 0, (uint32_t)1000000/PWM_FREQ);
// 		curTickTime_us -= triggerDelay_us;
// 	#endif

// 	uint32_t elapsedTime_us = curTickTime_us - prevTickTime_BEMFdelay;
// 	if (elapsedTime_us < (0.9*period_bemfdelay_usPerTick)) {
// 		if (elapsedTime_us > 1000)
// 			period_bemfdelay_usPerTick *= .9;
// 		return;
// 	}
// 	delayCommutateFinished = false;
// 	triggerPhase_delay = curPhase_BEMFdelay;

// 	period_bemfdelay_usPerTick = (curTickTime_us - prevZCtimes_us[prevZCtimesInd]) >> 3;
// 	prevZCtimes_us[prevZCtimesInd++] = curTickTime_us;
// 	prevZCtimesInd %= 1<<3;
// 	// period_bemfdelay_usPerTick = min(constrain(
// 	// 		elapsedTime_us,
// 	// 		period_bemfdelay_usPerTick - (PERIODSLEWLIM_US_PER_S*elapsedTime_us >> 20), ///1e6),
// 	// 		period_bemfdelay_usPerTick + (PERIODSLEWLIM_US_PER_S*elapsedTime_us >> 20)), ///1e6)),
// 	// 	100000);
// 	prevTickTime_BEMFdelay = curTickTime_us;

// 	// if (delayCommutateFinished){
// 	// 	delayCommutateTimer = 0;
// 	// }
// 	// delayCommutateTimer = curTickTime_us + (period_bemfdelay_usPerTick>>1) - (((int32_t)(phaseAdvance_Q10 * (period_bemfdelay_usPerTick>>1))) >> 10);
// 	delayCommutateTimer.priority(5);
// 	delayCommutateTimer.begin(delayCommutate_isr, (period_bemfdelay_usPerTick>>1) - (((int32_t)(phaseAdvance_Q10 * (period_bemfdelay_usPerTick>>1))) >> 10) - triggerDelay_us);
// 	delayMissedTickTimer.begin(missedTick_delay, ((period_bemfdelay_usPerTick*3) >> 1) - (triggerDelay_us));
// }
// void delayCommutate_isr() {
// 	delayCommutateTimer.end();

// 	uint8_t pos_BEMF = dir ? (triggerPhase_delay+1)%6 : (triggerPhase_delay+5)%6;

// 	commutate_isr(pos_BEMF, MODE_SENSORLESS_DELAY);

// 	volatile static bool GPIO0on;
// 	// digitalWrite(0, triggerPhase_delay==0);
// 	digitalWrite(1, pos_BEMF==0);
// 	GPIO0on = !GPIO0on;

// 	delayCommutateFinished = true;
// }

// float getSpeed_erps() {
// 	return 6000000.0/period_bemfdelay_usPerTick;
// }

// void missedTick_delay() {
// 	delayMissedTickTimer.end();
// 	exitSensorless();
// }

// // IntervalTimer postSwitchDelayTimer;
// extern volatile uint32_t timeToUpdateCmp;
// extern volatile uint32_t period_commutation_usPerTick;
// void updatePhase_BEMFdelay(uint8_t drivePhase) {
// 	if (curPhase_BEMFdelay != drivePhase){
// 		curPhase_BEMFdelay = drivePhase;
// 		if (curPhase_BEMFdelay >= 6){
// 			return;
// 		}
// 		timeToUpdateCmp = micros() + min(period_commutation_usPerTick/5, 200);
// 		cmpOn = false;

// 		floatPhase = floatPhases[curPhase_BEMFdelay];
// 		highPhase = highPhases[curPhase_BEMFdelay];
// 		isRisingEdge = isRisingEdges[curPhase_BEMFdelay] ^ (!dir);

// 		cmpVal = intV;
// 		intV = 0;
// 		// adc->disableCompare(ADC_0);
// 		// adc->startContinuous(floatPhases[curPhase_BEMFdelay], ADC_0);
// 		// adc->startContinuous(highPhases[curPhase_BEMFdelay], ADC_1);
// 		// bool suc = postSwitchDelayTimer.begin(updateCmp_ADC, min(period_commutation_usPerTick/10, 1000));
// 	}
// }
// void updateCmp_BEMFdelay() {
// 	cmpOn = true;
// }
// void inline BEMFdelay_update(volatile uint16_t vsx_cnts[3]) {
// 	// if ((!delayCommutateFinished) && (micros() >= delayCommutateTimer)){
// 	// delayCommutate_isr();
// 	// }

// 	if (!cmpOn)
// 		return;

// 	if (isRisingEdge) {
// 		intV += (vsx_cnts[floatPhase]>(vsx_cnts[highPhase]>>1)) ? vsx_cnts[floatPhase] - (vsx_cnts[highPhase]>>1) : 0;
// 	} else {
// 		intV += (vsx_cnts[floatPhase]<(vsx_cnts[highPhase]>>1)) ? (vsx_cnts[highPhase]>>1) - vsx_cnts[floatPhase] : 0;
// 	}

// 	if (!cmpOn || (vsx_cnts[floatPhase] < 100)){
// 		return;
// 	}

// 	// volatile bool floatGt0 = (vsx_cnts[floatPhase] > (vsx_cnts[highPhase]>>1)); // don't think i need to multiply by duty since it's properly phase aligned
// 	if (!isRisingEdge ^ (vsx_cnts[floatPhase] > (vsx_cnts[highPhase]>>1))){
// 		BEMFcrossing_isr(vsx_cnts);
		
// 		// static volatile int16_t LEDon;
// 		// #define LED_DIV 1
// 		// digitalWriteFast(0, HIGH);
// 		// // LEDon = !LEDon;
// 		// LEDon ++;
// 		// LEDon %= LED_DIV*2;
// 	}
// 	else {
// 		// digitalWriteFast(0, LOW);
// 	}
// }

// #endif