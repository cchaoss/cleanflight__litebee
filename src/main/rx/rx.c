/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <string.h>

#include <platform.h>
#include "build/build_config.h"
#include "build/debug.h"

#include "common/maths.h"
#include "common/utils.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "config/feature.h"
#include "config/config_reset.h"

#include "drivers/serial.h"
#include "drivers/adc.h"

#ifdef NRF
#include "drivers/bus_i2c.h"
#include "drivers/nrf2401.h"
#endif

#include "io/serial.h"

#include "fc/rc_controls.h"
#include "fc/config.h"

#include "flight/failsafe.h"

#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"
#include "drivers/system.h"

#include "rx/pwm.h"
#include "rx/sbus.h"
#include "rx/spektrum.h"
#include "rx/sumd.h"
#include "rx/sumh.h"
#include "rx/msp.h"
#include "rx/xbus.h"
#include "rx/ibus.h"
#include "rx/srxl.h"

#include "rx/rx.h"

//#define DEBUG_RX_SIGNAL_LOSS

const char rcChannelLetters[] = "AERT12345678abcdefgh";

uint16_t rssi = 0;                  // range: [0;1023]

static bool rxDataReceived = false;
static bool rxSignalReceived = false;
static bool rxSignalReceivedNotDataDriven = false;
static bool rxFlightChannelsValid = false;
static bool rxIsInFailsafeMode = true;
static bool rxIsInFailsafeModeNotDataDriven = true;

static uint32_t rxUpdateAt = 0;
static uint32_t needRxSignalBefore = 0;
static uint32_t suspendRxSignalUntil = 0;
static uint8_t  skipRxSamples = 0;

int16_t rcRaw[MAX_SUPPORTED_RC_CHANNEL_COUNT];     // interval [1000;2000]
int16_t rcData[MAX_SUPPORTED_RC_CHANNEL_COUNT];     // interval [1000;2000]
uint32_t rcInvalidPulsPeriod[MAX_SUPPORTED_RC_CHANNEL_COUNT];

#define MAX_INVALID_PULS_TIME    300
#define PPM_AND_PWM_SAMPLE_COUNT 3

#define DELAY_50_HZ (1000000 / 50)
#define DELAY_10_HZ (1000000 / 10)
#define DELAY_5_HZ (1000000 / 5)
#define SKIP_RC_ON_SUSPEND_PERIOD 1500000           // 1.5 second period in usec (call frequency independent)
#define SKIP_RC_SAMPLES_ON_RESUME  2                // flush 2 samples to drop wrong measurements (timing independent)

static uint8_t rcSampleIndex = 0;

rxRuntimeConfig_t rxRuntimeConfig;

PG_REGISTER_WITH_RESET_TEMPLATE(rxConfig_t, rxConfig, PG_RX_CONFIG, 0);

PG_REGISTER_ARR_WITH_RESET_FN(rxFailsafeChannelConfig_t, MAX_SUPPORTED_RC_CHANNEL_COUNT, failsafeChannelConfigs, PG_FAILSAFE_CHANNEL_CONFIG, 0);
PG_REGISTER_ARR_WITH_RESET_FN(rxChannelRangeConfiguration_t, NON_AUX_CHANNEL_COUNT, channelRanges, PG_CHANNEL_RANGE_CONFIG, 0);

PG_RESET_TEMPLATE(rxConfig_t, rxConfig,
    .sbus_inversion = 1,
    .midrc = 1500,
    .mincheck = 1100,
    .maxcheck = 1900,
    .rx_min_usec = 885,          // any of first 4 channels below this value will trigger rx loss detection
    .rx_max_usec = 2115,         // any of first 4 channels above this value will trigger rx loss detection
    .rssi_scale = RSSI_SCALE_DEFAULT,
);

void pgResetFn_channelRanges(rxChannelRangeConfiguration_t *instance)
{
    // set default calibration to full range and 1:1 mapping
    for (int i = 0; i < NON_AUX_CHANNEL_COUNT; i++) {
        instance[i].min = PWM_RANGE_MIN;
        instance[i].max = PWM_RANGE_MAX;
    }
}

void pgResetFn_failsafeChannelConfigs(rxFailsafeChannelConfig_t *instance)
{
    for (int i = 0; i < MAX_SUPPORTED_RC_CHANNEL_COUNT; i++) {
        instance[i].mode = (i < NON_AUX_CHANNEL_COUNT) ? RX_FAILSAFE_MODE_AUTO : RX_FAILSAFE_MODE_HOLD;
        instance[i].step = (i == THROTTLE)
            ? CHANNEL_VALUE_TO_RXFAIL_STEP(rxConfig()->rx_min_usec)
            : CHANNEL_VALUE_TO_RXFAIL_STEP(rxConfig()->midrc);
    }
}

static uint16_t nullReadRawRC(rxRuntimeConfig_t *rxRuntimeConfig, uint8_t channel)
{
    UNUSED(rxRuntimeConfig);
    UNUSED(channel);

    return PPM_RCVR_TIMEOUT;
}

static rcReadRawDataPtr rcReadRawFunc = nullReadRawRC;
static uint16_t rxRefreshRate;

void serialRxInit(rxConfig_t *rxConfig);

#define REQUIRED_CHANNEL_MASK 0x0F // first 4 channels

static uint8_t validFlightChannelMask;

STATIC_UNIT_TESTED void rxResetFlightChannelStatus(void)
{
    validFlightChannelMask = REQUIRED_CHANNEL_MASK;
}

STATIC_UNIT_TESTED bool rxHaveValidFlightChannels(void)
{
    return (validFlightChannelMask == REQUIRED_CHANNEL_MASK);
}

STATIC_UNIT_TESTED bool isPulseValid(uint16_t pulseDuration)
{
    return  pulseDuration >= rxConfig()->rx_min_usec &&
            pulseDuration <= rxConfig()->rx_max_usec;
}

// pulse duration is in micro seconds (usec)
STATIC_UNIT_TESTED void rxUpdateFlightChannelStatus(uint8_t channel, bool valid)
{
    if (channel < NON_AUX_CHANNEL_COUNT && !valid) {
        // if signal is invalid - mark channel as BAD
        validFlightChannelMask &= ~(1 << channel);
    }
}

void rxInit(modeActivationCondition_t *modeActivationConditions)
{
    uint8_t i;
    uint16_t value;

    rcSampleIndex = 0;

    for (i = 0; i < MAX_SUPPORTED_RC_CHANNEL_COUNT; i++) {
        rcData[i] = rxConfig()->midrc;
        rcInvalidPulsPeriod[i] = millis() + MAX_INVALID_PULS_TIME;
    }

    rcData[THROTTLE] = (feature(FEATURE_3D)) ? rxConfig()->midrc : rxConfig()->rx_min_usec;

    // Initialize ARM switch to OFF position when arming via switch is defined
    for (i = 0; i < MAX_MODE_ACTIVATION_CONDITION_COUNT; i++) {
        modeActivationCondition_t *modeActivationCondition = &modeActivationConditions[i];
        if (modeActivationCondition->modeId == BOXARM && IS_RANGE_USABLE(&modeActivationCondition->range)) {
            // ARM switch is defined, determine an OFF value
            if (modeActivationCondition->range.startStep > 0) {
                value = MODE_STEP_TO_CHANNEL_VALUE((modeActivationCondition->range.startStep - 1));
            } else {
                value = MODE_STEP_TO_CHANNEL_VALUE((modeActivationCondition->range.endStep + 1));
            }
            // Initialize ARM AUX channel to OFF value
            rcData[modeActivationCondition->auxChannelIndex + NON_AUX_CHANNEL_COUNT] = value;
        }
    }

#ifdef SERIAL_RX
    if (feature(FEATURE_RX_SERIAL)) {
        serialRxInit(rxConfig());
    }
#endif

    if (feature(FEATURE_RX_MSP)) {
        rxRefreshRate = 20000;
        rxMspInit(&rxRuntimeConfig, &rcReadRawFunc);
    }

    if (feature(FEATURE_RX_PPM) || feature(FEATURE_RX_PARALLEL_PWM)) {
        rxRefreshRate = 20000;
        rxPwmInit(&rxRuntimeConfig, &rcReadRawFunc);
    }
}

#ifdef SERIAL_RX
void serialRxInit(rxConfig_t *rxConfig)
{
    bool enabled = false;
    switch (rxConfig->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
            rxRefreshRate = 22000;
            enabled = spektrumInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SPEKTRUM2048:
            rxRefreshRate = 11000;
            enabled = spektrumInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SBUS:
            rxRefreshRate = 11000;
            enabled = sbusInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SUMD:
            rxRefreshRate = 11000;
            enabled = sumdInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SUMH:
            rxRefreshRate = 11000;
            enabled = sumhInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SRXL:
			rxRefreshRate = 11000;
            enabled = srxlInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_XBUS_MODE_B_RJ01:
            rxRefreshRate = 11000;
            enabled = xBusInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_IBUS:
            enabled = ibusInit(&rxRuntimeConfig, &rcReadRawFunc);
            break;
    }

    if (!enabled) {
        featureClear(FEATURE_RX_SERIAL);
        rcReadRawFunc = nullReadRawRC;
    }
}

uint8_t serialRxFrameStatus(void)
{
    /**
     * FIXME: Each of the xxxxFrameStatus() methods MUST be able to survive being called without the
     * corresponding xxxInit() method having been called first.
     *
     * This situation arises when the cli or the msp changes the value of rxConfig->serialrx_provider
     *
     * A solution is for the ___Init() to configure the serialRxFrameStatus function pointer which
     * should be used instead of the switch statement below.
     */
    switch (rxConfig()->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
        case SERIALRX_SPEKTRUM2048:
            return spektrumFrameStatus();
        case SERIALRX_SBUS:
            return sbusFrameStatus();
        case SERIALRX_SUMD:
            return sumdFrameStatus();
        case SERIALRX_SUMH:
            return sumhFrameStatus();
        case SERIALRX_SRXL:
						return srxlFrameStatus();
        case SERIALRX_XBUS_MODE_B_RJ01:
            return xBusFrameStatus();
        case SERIALRX_IBUS:
            return ibusFrameStatus();
    }
    return SERIAL_RX_FRAME_PENDING;
}
#endif

uint8_t calculateChannelRemapping(uint8_t *channelMap, uint8_t channelMapEntryCount, uint8_t channelToRemap)
{
    if (channelToRemap < channelMapEntryCount) {
        return channelMap[channelToRemap];
    }
    return channelToRemap;
}

bool rxIsReceivingSignal(void)
{
    return rxSignalReceived;
}

bool rxAreFlightChannelsValid(void)
{
    return rxFlightChannelsValid;
}
static bool isRxDataDriven(void)
{
    return !(feature(FEATURE_RX_PARALLEL_PWM | FEATURE_RX_PPM));
}

static void resetRxSignalReceivedFlagIfNeeded(uint32_t currentTime)
{
    if (!rxSignalReceived) {
        return;
    }

    if (((int32_t)(currentTime - needRxSignalBefore) >= 0)) {
        rxSignalReceived = false;
        rxSignalReceivedNotDataDriven = false;
    }
}

void suspendRxSignal(void)
{
    suspendRxSignalUntil = micros() + SKIP_RC_ON_SUSPEND_PERIOD;
    skipRxSamples = SKIP_RC_SAMPLES_ON_RESUME;
    failsafeOnRxSuspend(SKIP_RC_ON_SUSPEND_PERIOD);
}

void resumeRxSignal(void)
{
    suspendRxSignalUntil = micros();
    skipRxSamples = SKIP_RC_SAMPLES_ON_RESUME;
    failsafeOnRxResume();
}

void updateRx(uint32_t currentTime)
{
    resetRxSignalReceivedFlagIfNeeded(currentTime);

    if (isRxDataDriven()) {
        rxDataReceived = false;
    }


#ifdef SERIAL_RX
    if (feature(FEATURE_RX_SERIAL)) {
        uint8_t frameStatus = serialRxFrameStatus();

        if (frameStatus & SERIAL_RX_FRAME_COMPLETE) {
            rxDataReceived = true;
            rxIsInFailsafeMode = (frameStatus & SERIAL_RX_FRAME_FAILSAFE) != 0;
            rxSignalReceived = !rxIsInFailsafeMode;
            needRxSignalBefore = currentTime + DELAY_10_HZ;
        }
    }
#endif

    if (feature(FEATURE_RX_MSP)) {
        rxDataReceived = rxMspFrameComplete();

        if (rxDataReceived) {
            rxSignalReceived = true;
            rxIsInFailsafeMode = false;
            needRxSignalBefore = currentTime + DELAY_5_HZ;
        }
    }

    if (feature(FEATURE_RX_PPM)) {
        if (isPPMDataBeingReceived()) {
            rxSignalReceivedNotDataDriven = true;
            rxIsInFailsafeModeNotDataDriven = false;
            needRxSignalBefore = currentTime + DELAY_10_HZ;
            resetPPMDataReceivedState();
        }
    }

    if (feature(FEATURE_RX_PARALLEL_PWM)) {
        if (isPWMDataBeingReceived()) {
            rxSignalReceivedNotDataDriven = true;
            rxIsInFailsafeModeNotDataDriven = false;
            needRxSignalBefore = currentTime + DELAY_10_HZ;
        }
    }

}

bool shouldProcessRx(uint32_t currentTime)
{
    return rxDataReceived || ((int32_t)(currentTime - rxUpdateAt) >= 0); // data driven or 50Hz
}

static uint16_t calculateNonDataDrivenChannel(uint8_t chan, uint16_t sample)
{
    static uint16_t rcSamples[MAX_SUPPORTED_RX_PARALLEL_PWM_OR_PPM_CHANNEL_COUNT][PPM_AND_PWM_SAMPLE_COUNT];
    static bool rxSamplesCollected = false;

    uint8_t currentSampleIndex = rcSampleIndex % PPM_AND_PWM_SAMPLE_COUNT;

    // update the recent samples and compute the average of them
    rcSamples[chan][currentSampleIndex] = sample;

    // avoid returning an incorrect average which would otherwise occur before enough samples
    if (!rxSamplesCollected) {
        if (rcSampleIndex < PPM_AND_PWM_SAMPLE_COUNT) {
            return sample;
        }
        rxSamplesCollected = true;
    }

    uint16_t rcDataMean = 0;
    uint8_t sampleIndex;
    for (sampleIndex = 0; sampleIndex < PPM_AND_PWM_SAMPLE_COUNT; sampleIndex++)
        rcDataMean += rcSamples[chan][sampleIndex];

    return rcDataMean / PPM_AND_PWM_SAMPLE_COUNT;
}

static uint16_t getRxfailValue(uint8_t channel)
{
    rxFailsafeChannelConfig_t *failsafeChannelConfig = failsafeChannelConfigs(channel);
    uint8_t mode = failsafeChannelConfig->mode;

    // force auto mode to prevent fly away when failsafe stage 2 is disabled
    if ( channel < NON_AUX_CHANNEL_COUNT && (!feature(FEATURE_FAILSAFE)) ) {
        mode = RX_FAILSAFE_MODE_AUTO;
    }

    switch(mode) {
        case RX_FAILSAFE_MODE_AUTO:
            switch (channel) {
                case ROLL:
                case PITCH:
                case YAW:
                    return rxConfig()->midrc;

                case THROTTLE:
                    if (feature(FEATURE_3D))
                        return rxConfig()->midrc;
                    else
                        return rxConfig()->rx_min_usec;
            }
            /* no break */

        default:
        case RX_FAILSAFE_MODE_INVALID:
        case RX_FAILSAFE_MODE_HOLD:
            return rcData[channel];

        case RX_FAILSAFE_MODE_SET:
            return RXFAIL_STEP_TO_CHANNEL_VALUE(failsafeChannelConfig->step);
    }
}

STATIC_UNIT_TESTED uint16_t applyRxChannelRangeConfiguraton(int sample, rxChannelRangeConfiguration_t *range)
{
    // Avoid corruption of channel with a value of PPM_RCVR_TIMEOUT
    if (sample == PPM_RCVR_TIMEOUT) {
        return PPM_RCVR_TIMEOUT;
    }

    sample = scaleRange(sample, range->min, range->max, PWM_RANGE_MIN, PWM_RANGE_MAX);
    sample = MIN(MAX(PWM_PULSE_MIN, sample), PWM_PULSE_MAX);

    return sample;
}

static void readRxChannelsApplyRanges(void)
{
    uint8_t channel;

    for (channel = 0; channel < rxRuntimeConfig.channelCount; channel++) {

        uint8_t rawChannel = calculateChannelRemapping(rxConfig()->rcmap, ARRAYLEN(rxConfig()->rcmap), channel);

        // sample the channel
        uint16_t sample = rcReadRawFunc(&rxRuntimeConfig, rawChannel);

        // apply the rx calibration
        if (channel < NON_AUX_CHANNEL_COUNT) {
            sample = applyRxChannelRangeConfiguraton(sample, channelRanges(channel));
        }

        rcRaw[channel] = sample;
    }


}

static void detectAndApplySignalLossBehaviour(void)
{
    int channel;
    uint16_t sample;
    bool useValueFromRx = true;
    bool rxIsDataDriven = isRxDataDriven();
    uint32_t currentMilliTime = millis();
    if (!rxIsDataDriven) {
        rxSignalReceived = rxSignalReceivedNotDataDriven;
        rxIsInFailsafeMode = rxIsInFailsafeModeNotDataDriven;
    }

    if (!rxSignalReceived || rxIsInFailsafeMode) {
        useValueFromRx = false;
    }

#ifdef DEBUG_RX_SIGNAL_LOSS
    debug[0] = rxSignalReceived;
    debug[1] = rxIsInFailsafeMode;
    debug[2] = rcReadRawFunc(&rxRuntimeConfig, 0);
#endif

    rxResetFlightChannelStatus();

    for (channel = 0; channel < rxRuntimeConfig.channelCount; channel++) {

        sample = (useValueFromRx) ? rcRaw[channel] : PPM_RCVR_TIMEOUT;

        bool validPulse = isPulseValid(sample);

        if (!validPulse) {
            if (currentMilliTime < rcInvalidPulsPeriod[channel]) {
                sample = rcData[channel];           // hold channel for MAX_INVALID_PULS_TIME
            } else {
                sample = getRxfailValue(channel);   // after that apply rxfail value
                rxUpdateFlightChannelStatus(channel, validPulse);
            }
        } else {
            rcInvalidPulsPeriod[channel] = currentMilliTime + MAX_INVALID_PULS_TIME;
        }

        if (rxIsDataDriven) {
            rcData[channel] = sample;
        } else {
            rcData[channel] = calculateNonDataDrivenChannel(channel, sample);
        }
    }

    rxFlightChannelsValid = rxHaveValidFlightChannels();

    if ((rxFlightChannelsValid) && !(rcModeIsActive(BOXFAILSAFE) && feature(FEATURE_FAILSAFE))) {
        failsafeOnValidDataReceived();
    } else {
        rxIsInFailsafeMode = rxIsInFailsafeModeNotDataDriven = true;
        failsafeOnValidDataFailed();

        for (channel = 0; channel < rxRuntimeConfig.channelCount; channel++) {
            rcData[channel] = getRxfailValue(channel);
        }
    }

#ifdef DEBUG_RX_SIGNAL_LOSS
    debug[3] = rcData[THROTTLE];
#endif

}


void calculateRxChannelsAndUpdateFailsafe(uint32_t currentTime)
{
	
	rxUpdateAt = currentTime + (1000000 / 80);
    //rxUpdateAt = currentTime + DELAY_50_HZ;

    // only proceed when no more samples to skip and suspend period is over
    if (skipRxSamples) {
        if (currentTime > suspendRxSignalUntil) {
            skipRxSamples--;
        }
        return;
    }

    readRxChannelsApplyRanges();
#ifndef NRF
    detectAndApplySignalLossBehaviour();
#endif	
	rcSampleIndex++;


#ifdef NRF
	static bool overturn = true;	
	if(overturn){
		//if(flag.batt < 20 && millis() > 3000)	led_beep_sleep();//下个版本不需要
#if 1	//低电压降落+失控保护——use height
		static uint8_t b;
		if(!nrf_rx() || flag.batt_low){
			if(mspData.mspCmd & ONLINE)	
				mspData.mspCmd &= ~MOTOR;//在线模式控制电机转时遥控断电，电机一直转，无法控制-->11/14
	
			if(!flag.batt_low){
				mspData.motor[PIT] = 1500;
				mspData.motor[ROL] = 1500;
				mspData.motor[YA ] = 1500;
			}

			mspData.mspCmd |= ALTHOLD;//开定高
			if(mspData.motor[THR] >= 1650)mspData.motor[THR] = 1550;
				else if(mspData.motor[THR] >= 1490)mspData.motor[THR] = 1433;
					else mspData.motor[THR] = 1360;
			
			
			if(flag.height <= 300){	
				mspData.mspCmd &= ~ALTHOLD;//关定高
				b++;
				if(b > 80){
					b = 80;
					mspData.motor[THR] = 1100;
					mspData.mspCmd &= ~ARM;
				}
				else mspData.motor[THR] = 1360;
			}else b =0;
		}
#endif 

		SetTX_Mode();
	}
	else	{nrf_tx();	SetRX_Mode();}

#if 1	//限制高度6m 左右
		debug[0] = flag.height;
		static uint8_t a;
		if(flag.height > 800){
			a++;
			if(a > 20){
				a = 20;
				if(mspData.motor[THR] >= 1650)mspData.motor[THR] = 1588;
					else if(mspData.motor[THR] >= 1490)mspData.motor[THR] = 1438;
						else mspData.motor[THR] = 1350;
				mspData.mspCmd |= ALTHOLD;
			}
		}else a = 0;
#endif
#if 0	//test i2c read and write
/*
	static uint8_t sta;
	static uint16_t j = 0,i=0,x=0;
	if(mspData.mspCmd & OFFLINE)
	{
		for(char a = 0;a<8;a++)
		{
			i2cRead(0x08,0xff,1, &sta);
			if(sta == j) {i++;}
				else {j = sta;x++;}
			j++;
			if(j == 256) {i=0;j=0;}
		}
		for(uint16_t a =0;a<8;a++)
		{
			i2cWrite(0x08,0,a);
			delayMicroseconds(25);
		}
	}
	rcData[6] = sta;
	rcData[7] = i;
	rcData[8] = x;
*/
/*
		static uint8_t sta,length,x;
		if(mspData.mspCmd & OFFLINE)
		{
			i2cRead(0x08,0xff,1, &sta);
			if(sta == 4) 
			{	i2cRead(0x08,0xff,1, &sta);
				if(sta == 5) 
				{
					i2cRead(0x08,0xff,1, &length);
					for(uint8_t i = 0;i<length;i++) 
					{
						i2cRead(0x08,0xff,1, &sta);
						if(sta == (i+7)) x++;				
					}
				}
			}
			for(uint16_t a =0;a<8;a++)
			{
				i2cWrite(0x08,0,a);
				delayMicroseconds(25);
			}
		}else x =0;
		rcData[8] = x;
*/
#endif
	rx_data_process(rcData);
	overturn = !overturn;


	rcData[5] = mspData.mspCmd;
	rcData[6] = mspData.motor[0];
	rcData[7] = mspData.motor[1];
	rcData[8] = mspData.motor[2];
	rcData[9] = mspData.motor[3];
#endif


}

void parseRcChannels(const char *input, rxConfig_t *rxConfig)
{
    const char *c, *s;

    for (c = input; *c; c++) {
        s = strchr(rcChannelLetters, *c);
        if (s && (s < rcChannelLetters + MAX_MAPPABLE_RX_INPUTS))
            rxConfig->rcmap[s - rcChannelLetters] = c - input;
    }
}

void updateRSSIPWM(void)
{
    int16_t pwmRssi = 0;
    // Read value of AUX channel as rssi
    pwmRssi = rcData[rxConfig()->rssi_channel - 1];
	
	// RSSI_Invert option	
	if (rxConfig()->rssi_ppm_invert) {
	    pwmRssi = ((2000 - pwmRssi) + 1000);
	}
	
    // Range of rawPwmRssi is [1000;2000]. rssi should be in [0;1023];
    rssi = (uint16_t)((constrain(pwmRssi - 1000, 0, 1000) / 1000.0f) * 1023.0f);
}

#define RSSI_ADC_SAMPLE_COUNT 16
//#define RSSI_SCALE (0xFFF / 100.0f)

void updateRSSIADC(uint32_t currentTime)
{
#ifndef ADC_RSSI
    UNUSED(currentTime);
#else
    static uint8_t adcRssiSamples[RSSI_ADC_SAMPLE_COUNT];
    static uint8_t adcRssiSampleIndex = 0;
    static uint32_t rssiUpdateAt = 0;

    if ((int32_t)(currentTime - rssiUpdateAt) < 0) {
        return;
    }
    rssiUpdateAt = currentTime + DELAY_50_HZ;

    int16_t adcRssiMean = 0;
    uint16_t adcRssiSample = adcGetChannel(ADC_RSSI);
    uint8_t rssiPercentage = adcRssiSample / rxConfig()->rssi_scale;

    adcRssiSampleIndex = (adcRssiSampleIndex + 1) % RSSI_ADC_SAMPLE_COUNT;

    adcRssiSamples[adcRssiSampleIndex] = rssiPercentage;

    uint8_t sampleIndex;

    for (sampleIndex = 0; sampleIndex < RSSI_ADC_SAMPLE_COUNT; sampleIndex++) {
        adcRssiMean += adcRssiSamples[sampleIndex];
    }

    adcRssiMean = adcRssiMean / RSSI_ADC_SAMPLE_COUNT;

    rssi = (uint16_t)((constrain(adcRssiMean, 0, 100) / 100.0f) * 1023.0f);
#endif
}

void updateRSSI(uint32_t currentTime)
{

    if (rxConfig()->rssi_channel > 0) {
        updateRSSIPWM();
    } else if (feature(FEATURE_RSSI_ADC)) {
        updateRSSIADC(currentTime);
    }
}

void initRxRefreshRate(uint16_t *rxRefreshRatePtr)
{
    *rxRefreshRatePtr = rxRefreshRate;
}

