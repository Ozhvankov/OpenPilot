/**
  ******************************************************************************
  * @addtogroup PIOS PIOS Core hardware abstraction layer
  * @{
  * @addtogroup PIOS_HCSR04 HCSR04 Functions
  * @brief Hardware functions to deal with the altitude pressure sensor
  * @{
  *
  * @file       pios_hcsr04.c
  * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
  * @brief      HCSR04 sonar Sensor Routines
  * @see        The GNU Public License (GPL) Version 3
  *
  ******************************************************************************/
/* 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 3 of the License, or 
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Project Includes */
#include "pios.h"

#if defined(PIOS_INCLUDE_HCSR04)
#if !(defined(PIOS_INCLUDE_SPEKTRUM) || defined(PIOS_INCLUDE_SBUS) || defined(PIOS_INCLUDE_PPM))
#error Only supported with Spektrum, PPM or S.Bus interface!
#endif

/* Local Variables */

static TIM_ICInitTypeDef TIM_ICInitStructure;
static uint8_t CaptureState;
static uint16_t RiseValue;
static uint16_t FallValue;
static uint16_t CaptureValue;
static uint8_t CapCounter;
static uint16_t TimerCounter;

#ifndef USE_STM32103CB_CC_Rev1
#define PIOS_HCSR04_TRIG_GPIO_PORT                  GPIOD
#define PIOS_HCSR04_TRIG_PIN                        GPIO_Pin_2
#define PIOS_HCSR04_TIMER	TIM3
#define PIOS_HCSR04_CC		TIM_IT_CC2
#define PIOS_HCSR04_CHANNEL	TIM_Channel_2
#define PIOS_HCSR04_GETCAPTURE(x) 	TIM_GetCapture2(x)
#define PIOS_HCSR04_INPUT_GPIO_PORT     GPIOB
#define PIOS_HCSR04_INPUT_PIN            GPIO_Pin_5
#define PIOS_HCSR04_IRQ					TIM3_IRQn
#define PIOS_HCSR04_RCC		RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE)
#define PIOS_HCSR04_REMAP	GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3, ENABLE)
#define PIOS_HCSR04_IRQ_FUNC void TIM3_IRQHandler(void)
#else
#define PIOS_HCSR04_TRIG_GPIO_PORT                  GPIOA
#define PIOS_HCSR04_TRIG_PIN                        GPIO_Pin_0
#define PIOS_HCSR04_TIMER		TIM2
#define PIOS_HCSR04_CC			TIM_IT_CC2
#define PIOS_HCSR04_CHANNEL		TIM_Channel_2
#define PIOS_HCSR04_GETCAPTURE(x) 	TIM_GetCapture2(x)
#define PIOS_HCSR04_INPUT_GPIO_PORT     GPIOA
#define PIOS_HCSR04_INPUT_PIN            GPIO_Pin_1
#define PIOS_HCSR04_IRQ					TIM2_IRQn
#define PIOS_HCSR04_RCC		RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE)
#define PIOS_HCSR04_REMAP	;;
#define PIOS_HCSR04_IRQ_FUNC void TIM2_IRQHandler(void)
#endif
/**
* Initialise the HC-SR04 sensor
*/
void PIOS_HCSR04_Init(void)
{
	/* Flush counter variables */
	CaptureState = 0;
	RiseValue = 0;
	FallValue = 0;
	CaptureValue = 0;

	/* Init triggerpin */
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_StructInit(&GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Pin = PIOS_HCSR04_TRIG_PIN;
	GPIO_Init(PIOS_HCSR04_TRIG_GPIO_PORT, &GPIO_InitStructure);
	PIOS_HCSR04_TRIG_GPIO_PORT->BSRR = PIOS_HCSR04_TRIG_PIN;

	/* Setup RCC */
	PIOS_HCSR04_RCC;

	/* Enable timer interrupts */
	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_InitStructure.NVIC_IRQChannel = PIOS_HCSR04_IRQ;
	NVIC_Init(&NVIC_InitStructure);

	/* Partial pin remap for TIM3 (PB5) */
	PIOS_HCSR04_REMAP;

	/* Configure input pins */
	GPIO_StructInit(&GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Pin = PIOS_HCSR04_INPUT_PIN;
	GPIO_Init(PIOS_HCSR04_INPUT_GPIO_PORT, &GPIO_InitStructure);

	/* Configure timer for input capture */
	TIM_ICInitStructure.TIM_ICPolarity = TIM_ICPolarity_Rising;
	TIM_ICInitStructure.TIM_ICSelection = TIM_ICSelection_DirectTI;
	TIM_ICInitStructure.TIM_ICPrescaler = TIM_ICPSC_DIV1;
	TIM_ICInitStructure.TIM_ICFilter = 0x0;
	TIM_ICInitStructure.TIM_Channel = PIOS_HCSR04_CHANNEL;
	TIM_ICInit(PIOS_HCSR04_TIMER, &TIM_ICInitStructure);

	/* Shared timer works */
	/* Configure timer clocks */
#ifndef USE_STM32103CB_CC_Rev1
	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
	TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
	TIM_TimeBaseStructure.TIM_Period = 0xFFFF;
	TIM_TimeBaseStructure.TIM_Prescaler = (PIOS_MASTER_CLOCK / 1000000) - 1;
	TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_InternalClockConfig(PIOS_HCSR04_TIMER);
	TIM_TimeBaseInit(PIOS_HCSR04_TIMER, &TIM_TimeBaseStructure);
#endif

	/* Enable the Capture Compare Interrupt Request */
	TIM_ITConfig(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC | TIM_IT_Update, DISABLE);

	/* Enable timers */
	TIM_Cmd(PIOS_HCSR04_TIMER, ENABLE);

	/* Setup local variable which stays in this scope */
	/* Doing this here and using a local variable saves doing it in the ISR */
	TIM_ICInitStructure.TIM_ICSelection = TIM_ICSelection_DirectTI;
	TIM_ICInitStructure.TIM_ICPrescaler = TIM_ICPSC_DIV1;
	TIM_ICInitStructure.TIM_ICFilter = 0x0;
}

/**
* Get the value of an sonar timer
* \output >0 timer value
*/
uint16_t PIOS_HCSR04_Get(void)
{
	return CaptureValue;
}

/**
* Get the value of an sonar timer
* \output >0 timer value
*/
uint8_t PIOS_HCSR04_Completed(void)
{
	return CapCounter;
}
/**
* Trigger sonar sensor
*/
void PIOS_HCSR04_Trigger(void)
{
	CapCounter=0;
	PIOS_HCSR04_TRIG_GPIO_PORT->BSRR = PIOS_HCSR04_TRIG_PIN;
	PIOS_DELAY_WaituS(15);
	PIOS_HCSR04_TRIG_GPIO_PORT->BRR = PIOS_HCSR04_TRIG_PIN;
	TIM_ClearITPendingBit(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC);
	TIM_ClearITPendingBit(PIOS_HCSR04_TIMER, TIM_IT_Update);
	TIM_ITConfig(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC, ENABLE);
}


/**
* Handle TIM3 global interrupt request
*/
//void PIOS_PWM_irq_handler(TIM_TypeDef * timer)
PIOS_HCSR04_IRQ_FUNC
{
	if (TIM_GetITStatus(PIOS_HCSR04_TIMER, TIM_IT_Update) != RESET) {
		TimerCounter+=PIOS_HCSR04_TIMER->ARR;
		TIM_ClearITPendingBit(PIOS_HCSR04_TIMER, TIM_IT_Update);
		return;
	}

	/* Do this as it's more efficient */
	if (TIM_GetITStatus(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC) == SET) {
		if (CaptureState == 0) {
			RiseValue = PIOS_HCSR04_GETCAPTURE(PIOS_HCSR04_TIMER);
			/* init overflow adder */
			TimerCounter=0;
			TIM_ClearITPendingBit(PIOS_HCSR04_TIMER, TIM_IT_Update);
			TIM_ITConfig(PIOS_HCSR04_TIMER, TIM_IT_Update, ENABLE);
		} else {
			FallValue = TimerCounter+PIOS_HCSR04_GETCAPTURE(PIOS_HCSR04_TIMER);
		}
	}

	/* Clear PIOS_HCSR04_TIMER Capture compare interrupt pending bit */
	TIM_ClearITPendingBit(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC);

	/* Simple rise or fall state machine */
	if (CaptureState == 0) {
		/* Switch states */
		CaptureState = 1;

		/* Switch polarity of input capture */
		TIM_ICInitStructure.TIM_ICPolarity = TIM_ICPolarity_Falling;
		TIM_ICInitStructure.TIM_Channel = PIOS_HCSR04_CHANNEL;
		TIM_ICInit(PIOS_HCSR04_TIMER, &TIM_ICInitStructure);

	} else {
		/* Capture computation */
		if (FallValue > RiseValue) {
			CaptureValue = (FallValue - RiseValue);
		} else {
			CaptureValue = ((0xFFFF - RiseValue) + FallValue);
		}

		/* Switch states */
		CaptureState = 0;

		/* Increase supervisor counter */
		CapCounter++;
		TIM_ITConfig(PIOS_HCSR04_TIMER, PIOS_HCSR04_CC | TIM_IT_Update, DISABLE);

		/* Switch polarity of input capture */
		TIM_ICInitStructure.TIM_ICPolarity = TIM_ICPolarity_Rising;
		TIM_ICInitStructure.TIM_Channel = PIOS_HCSR04_CHANNEL;
		TIM_ICInit(PIOS_HCSR04_TIMER, &TIM_ICInitStructure);

	}
}


#endif
