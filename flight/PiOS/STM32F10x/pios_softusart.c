/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup   PIOS_PWM PWM Input Functions
 * @brief		Code to measure with PWM input
 * @{
 *
 * @file       pios_pwm.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      PWM Input functions (STM32 dependent)
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
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

// Adopted from ST example AN2781

/* Project Includes */
#include "pios.h"
#include "pios_com_priv.h"

#if defined(PIOS_INCLUDE_SOFTUSART)

#define SLOW_STREAM
 
#include "pios_softusart_priv.h"
#include <pios_usart_priv.h>

#define PIOS_SOFTUSART_MAX_DEVS 1

/* Provide a COM driver */
static void PIOS_SOFTUSART_ChangeBaud(uint32_t usart_id, uint32_t baud);
static void PIOS_SOFTUSART_RegisterRxCallback(uint32_t usart_id, pios_com_callback rx_in_cb, uint32_t context);
static void PIOS_SOFTUSART_RegisterTxCallback(uint32_t usart_id, pios_com_callback tx_out_cb, uint32_t context);
static void PIOS_SOFTUSART_TxStart(uint32_t usart_id, uint16_t tx_bytes_avail);
static void PIOS_SOFTUSART_RxStart(uint32_t usart_id, uint16_t rx_bytes_avail);


const struct pios_com_driver pios_softusart_com_driver = {
	.set_baud   = PIOS_SOFTUSART_ChangeBaud,
	.tx_start   = PIOS_SOFTUSART_TxStart,
	.rx_start   = PIOS_SOFTUSART_RxStart,
	.bind_tx_cb = PIOS_SOFTUSART_RegisterTxCallback,
	.bind_rx_cb = PIOS_SOFTUSART_RegisterRxCallback,
};

/* Local Variables */
enum pios_softusart_dev_magic {
	PIOS_SOFTUSART_DEV_MAGIC = 0xab30293c,
};

const uint8_t MSK_TAB[9]= { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0};

struct pios_softusart_dev {
	enum pios_softusart_dev_magic     magic;
	const struct pios_softusart_cfg  *cfg;
	
	// Comm variables
	pios_com_callback rx_in_cb;
	uint32_t rx_in_context;
	pios_com_callback tx_out_cb;
	uint32_t tx_out_context;

	bool active;       // Allow rx and tx from this port
	
	// Communication variables
	bool rx_phase;     // phase of received bit [0-1] (edge, middle)
	bool tx_phase;     // phase of transmited bit [0-1] (edge, middle)
	bool rx_parity;    // received parity [0-1]
	bool tx_parity;    // transmited parity [0-1]
	bool rx_bit9;      // received 9-th data bit [0-1]
	bool tx_bit9;      // transmited 9-th data bit [0-1]
	uint8_t rx_bit;    // counter of received bits [0-11]
	uint8_t tx_bit;    // counter of transmited bits [0-11]
	uint8_t rx_samp;   // register of samples [0-3]
	uint8_t rx_buff;   // received byte register
	uint8_t tx_data;   // transmited byte register
	uint8_t status;	   // UART status register (1= active state)
	uint32_t rx_dropped;
	
	// Precache variables for changing modes
	uint16_t tim_it; // Interrupt to enable/disable for cc mode
	uint16_t cce;    // Flag in CCER1 to enable/disable cc mode 
	uint16_t ccp;    // Flag for input polarity selection
};

static void PIOS_SOFTUSART_tim_overflow_cb (uint32_t id, uint32_t context, uint8_t channel, uint16_t count);
static void PIOS_SOFTUSART_tim_edge_cb (uint32_t id, uint32_t context, uint8_t channel, uint16_t count);
const static struct pios_tim_callbacks softusart_tim_callbacks = {
	.overflow = PIOS_SOFTUSART_tim_overflow_cb,
	.edge     = PIOS_SOFTUSART_tim_edge_cb,
};


static bool PIOS_SOFTUSART_TestStatus(struct pios_softusart_dev *dev, uint16_t flag);
static void PIOS_SOFTUSART_SetStatus(struct pios_softusart_dev *dev, uint16_t flag);
static void PIOS_SOFTUSART_SetStatus(struct pios_softusart_dev *dev, uint16_t flag);
static void PIOS_SOFTUSART_EnableCaptureMode(struct pios_softusart_dev *softusart_dev);
static void PIOS_SOFTUSART_EnableCompareMode(struct pios_softusart_dev *softusart_dev, int16_t count);
static void PIOS_SOFTUSART_SetCCE(struct pios_softusart_dev *softusart_dev, bool enable);

static bool PIOS_SOFTUSART_validate(struct pios_softusart_dev * pwm_dev)
{
	return (pwm_dev->magic == PIOS_SOFTUSART_DEV_MAGIC);
}

#if defined(PIOS_INCLUDE_FREERTOS)
static struct pios_softusart_dev * PIOS_SOFTUSART_alloc(void)
{
	struct pios_softusart_dev *pwm_softusart;
	
	pwm_softusart = (struct pios_softusart_dev *)pvPortMalloc(sizeof(*pwm_softusart));
	if (!pwm_softusart) return(NULL);
	
	pwm_softusart->magic = PIOS_SOFTUSART_DEV_MAGIC;
	return(pwm_softusart);
}
#else
static struct pios_softusart_dev pios_softusart_devs[PIOS_SOFTUSART_MAX_DEVS];
static uint8_t pios_softusart_num_devs;
static struct pios_softusart_dev * PIOS_SOFTUSART_alloc(void)
{
	struct pios_softusart_dev *pwm_softusart;
	
	if (pios_softusart_num_devs >= PIOS_SOFTUSART_MAX_DEVS) {
		return (NULL);
	}
	
	pwm_softusart = &pios_softusart_devs[pios_softusart_num_devs++];
	pwm_softusart->magic = PIOS_SOFTUSART_DEV_MAGIC;
	
	return (pwm_softusart);
}
#endif

const struct pios_softusart_cfg * PIOS_SOFTUSART_GetConfig(uint32_t softusart_id)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) softusart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);
	
	return softusart_dev->cfg;
}

int32_t PIOS_SOFTUSART_Init(uint32_t *softusart_id, const struct pios_softusart_cfg *cfg)
{
	PIOS_DEBUG_Assert(softusart_id);
	PIOS_DEBUG_Assert(cfg);
	
	struct pios_softusart_dev *softusart_dev;
	
	softusart_dev = (struct pios_softusart_dev *) PIOS_SOFTUSART_alloc();
	if (!softusart_dev) goto out_fail;
	
	/* Bind the configuration to the device instance */
	softusart_dev->cfg = cfg;

	/* Default to enabled */
	softusart_dev->active = true;
	
	// Either half duplex or separate timers
#if 0  // This doesn't work because the channels* needs to persist
	// TODO: Add support for these being null to indicate no timer
	uint32_t tim_id;
	struct pios_tim_channel channels[2];
	int32_t num_channels = 0;
	if(cfg->half_duplex) {	
		PIOS_Assert(cfg->rx.timer == cfg->tx.timer && cfg->rx.timer_chan == cfg->tx.timer_chan);
		channels[0] = cfg->rx;
		num_channels = 1;
	} else {
		PIOS_Assert(cfg->rx.timer != cfg->tx.timer || cfg->rx.timer_chan != cfg->tx.timer_chan);
		channels[0] = cfg->rx;
		channels[1] = cfg->tx;
		num_channels = 2;
	}
	if (PIOS_TIM_InitChannels(&tim_id, channels, num_channels, &softusart_tim_callbacks, (uint32_t)softusart_dev)) {
		while(1);
		return -1;
	}
#else
	uint32_t tim_id;
	if (PIOS_TIM_InitChannels(&tim_id, &(cfg->rx), 1, &softusart_tim_callbacks, (uint32_t)softusart_dev)) {
		return -1;
	}
#endif
	
	/* Configure the rx channels to be in capture/compare mode */
	const struct pios_tim_channel * chan = &cfg->rx;
	
	/* Precache some flags */
	switch(chan->timer_chan) {
		case TIM_Channel_1: 
			softusart_dev->tim_it = TIM_IT_CC1;
			softusart_dev->cce = TIM_CCER_CC1E;
			softusart_dev->ccp = TIM_CCER_CC1P;
			break;
		case TIM_Channel_2: 
			softusart_dev->tim_it = TIM_IT_CC2; 
			softusart_dev->cce = TIM_CCER_CC2E;
			softusart_dev->ccp = TIM_CCER_CC2P;
			break;
		case TIM_Channel_3: 
			softusart_dev->tim_it = TIM_IT_CC3; 
			softusart_dev->cce = TIM_CCER_CC3E;
			softusart_dev->ccp = TIM_CCER_CC3P;
			break;
		case TIM_Channel_4: 
			softusart_dev->tim_it = TIM_IT_CC4; 
			softusart_dev->cce = TIM_CCER_CC4E;
			softusart_dev->ccp = TIM_CCER_CC4P;
			break;			
	};

	// Need the update event for that timer to detect timeouts
	TIM_ITConfig(chan->timer, TIM_IT_Update, ENABLE);
	
	// Set default baud rate
	PIOS_SOFTUSART_ChangeBaud((uint32_t)softusart_dev, 4800);
	
	// Configure the IO pin
	GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->tx.pin.init);

	// No data initially in the outgoing buffer
	PIOS_SOFTUSART_SetStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY);

	PIOS_SOFTUSART_EnableCaptureMode(softusart_dev);
	*softusart_id = (uint32_t)softusart_dev;
	
	return (0);
	
out_fail:
	return (-1);
}

/**
 * @brief Disable the softusart function on this port
 * @param[in] usart_id The COM port
 * @return 0 if success, -1 if failure
 */
int32_t PIOS_SOFTUSART_Disable(uint32_t usart_id)
{
	uint32_t softusart_id = PIOS_COM_GetLower(usart_id);
	if (softusart_id == 0)
		return -1;

	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) softusart_id;
	if(!PIOS_SOFTUSART_validate(softusart_dev))
		return -1;

	softusart_dev->active = false;

	// For now enable compare mode when doing this - hardcoding pwm output
	GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->tx.pin.init);
	PIOS_SOFTUSART_EnableCompareMode(softusart_dev, 0);
	PIOS_SOFTUSART_SetCCE(softusart_dev, false);          // Definitely don't want this for input

	// PWM input and output use normal polarity
	softusart_dev->cfg->rx.timer->CCER &= ~softusart_dev->ccp;

	return 0;
}

/**
 * @brief Enable the softusart function on this port
 * @param[in] usart_id The COM port
 * @return 0 if success, -1 if failure
 */
int32_t PIOS_SOFTUSART_Enable(uint32_t usart_id)
{
	uint32_t softusart_id = PIOS_COM_GetLower(usart_id);
	if (softusart_id == 0)
		return -1;

	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) softusart_id;
	if(!PIOS_SOFTUSART_validate(softusart_dev))
		return -1;

	GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->rx.pin.init);
	PIOS_SOFTUSART_SetCCE(softusart_dev, true);       // Reenable the capture IRQ
	PIOS_SOFTUSART_EnableCaptureMode(softusart_dev);

	softusart_dev->active = true;
	return 0;
}

/**
 * @brief Check a status flag
 */
static bool PIOS_SOFTUSART_TestStatus(struct pios_softusart_dev *dev, uint16_t flag)
{
	return dev->status & flag;
}

/**
 * @brief Set a status flag
 */
static void PIOS_SOFTUSART_SetStatus(struct pios_softusart_dev *dev, uint16_t flag)
{
	dev->status |= flag;
}

/**
 * @brief Clear a status flag
 */
static void PIOS_SOFTUSART_ClrStatus(struct pios_softusart_dev *dev, uint16_t flag)
{
	dev->status &= ~flag;
}

/**
 * @brief Set the baud rate
 */
static void PIOS_SOFTUSART_ChangeBaud(uint32_t usart_id, uint32_t baud)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) usart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);

	uint32_t clock_rate;
	uint32_t newdivisor;
	
	// Need to account for prescalar on timer to get the right period
	clock_rate = (PIOS_MASTER_CLOCK / (softusart_dev->cfg->rx.timer->PSC + 1));
	newdivisor = clock_rate / baud / 2;

	TIM_SetAutoreload(softusart_dev->cfg->rx.timer, newdivisor);
	if (!softusart_dev->cfg->half_duplex) { // Only need to update second if not half duplex
		clock_rate = (PIOS_MASTER_CLOCK / (softusart_dev->cfg->tx.timer->PSC + 1));
		newdivisor = clock_rate / baud / 2;

		TIM_SetAutoreload(softusart_dev->cfg->tx.timer, newdivisor);
	}
}

/**
 * @brief Set the callback into the general com driver when a byte is received
 */
static void PIOS_SOFTUSART_RegisterRxCallback(uint32_t usart_id, pios_com_callback rx_in_cb, uint32_t context)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) usart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);
	
	/* 
	 * Order is important in these assignments since ISR uses _cb
	 * field to determine if it's ok to dereference _cb and _context
	 */
	softusart_dev->rx_in_context = context;
	softusart_dev->rx_in_cb = rx_in_cb;	
}

/**
 * @brief Set the callback into the general com driver when a byte should be transmitted
 */
static void PIOS_SOFTUSART_RegisterTxCallback(uint32_t usart_id, pios_com_callback tx_out_cb, uint32_t context)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) usart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);
	
	/* 
	 * Order is important in these assignments since ISR uses _cb
	 * field to determine if it's ok to dereference _cb and _context
	 */
	softusart_dev->tx_out_context = context;
	softusart_dev->tx_out_cb = tx_out_cb;
}

/**
 * @brief Start transmission
 * @param[in] usart_id the context for the usart device
 * @parma[in] tx_bytes_avail how many bytes are available to transmit
 */
static void PIOS_SOFTUSART_TxStart(uint32_t usart_id, uint16_t tx_bytes_avail)
{
	// TODO: Enable or disable the interrupt here?
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) usart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);

	if(PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY)) {
		//YES - initiate sending procedure
		
		if (softusart_dev->tx_out_cb) {
			uint8_t b;
			uint16_t bytes_to_send;
			bool yield;
			
			bytes_to_send = (softusart_dev->tx_out_cb)(softusart_dev->tx_out_context, &b, 1, NULL, &yield);
			
			if (bytes_to_send > 0) {
				/* Send the byte we've been given */
				softusart_dev->tx_data = b;
				
				// Clear the empty flag
				PIOS_SOFTUSART_ClrStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY);
			}

#if defined(PIOS_INCLUDE_FREERTOS)
			if (yield) {
				vPortYieldFromISR();
			}
#endif	/* PIOS_INCLUDE_FREERTOS */

		}
	}

	// If data is loaded (this call or another) then start transmittion
	/*if(!PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY) && 
		!PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_IN_PROGRESS) && 
		!PIOS_SOFTUSART_TestStatus(softusart_dev, RECEIVE_IN_PROGRESS)) {
		softusart_dev->tx_phase = 0;
		softusart_dev->tx_bit = 0;
		PIOS_SOFTUSART_SetStatus(softusart_dev, TRANSMIT_IN_PROGRESS);
	};*/

}

static void PIOS_SOFTUSART_RxStart(uint32_t usart_id, uint16_t rx_bytes_avail)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) usart_id;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);
}

#define SET_TX GPIO_SetBits(softusart_dev->cfg->tx.pin.gpio,softusart_dev->cfg->tx.pin.init.GPIO_Pin)
#define CLR_TX GPIO_ResetBits(softusart_dev->cfg->tx.pin.gpio,softusart_dev->cfg->tx.pin.init.GPIO_Pin)
#define RX_TEST GPIO_ReadInputDataBit(softusart_dev->cfg->rx.pin.gpio,softusart_dev->cfg->tx.pin.init.GPIO_Pin)

/**
 * @brief When this occurs determine whether to set output high or low
 */
static void PIOS_SOFTUSART_tim_overflow_cb (uint32_t tim_id, uint32_t context, uint8_t channel, uint16_t count)
{
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) context;
	bool valid = PIOS_SOFTUSART_validate(softusart_dev);
	PIOS_Assert(valid);

	if (softusart_dev->active == false)
		return;

	bool yield = false;
	
	if (!PIOS_SOFTUSART_validate(softusart_dev)) {
		/* Invalid device specified */
		return;
	}
	
	if (channel >= (softusart_dev->cfg->half_duplex ? 1 : 2) ) {
		/* Channel out of range */
		return;
	}
	
	if(softusart_dev->tx_phase) {
		if(PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_IN_PROGRESS)) { // edge of current bit (no service for middle)

			switch(softusart_dev->tx_bit) {     // begin of bit transmition
				case 0:
					// Enable output mode on the pin
					PIOS_SOFTUSART_SetCCE(softusart_dev, false);
					GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->tx.pin.init);

					CLR_TX; //start bit transmition
					softusart_dev->tx_bit9 = 0;
#ifdef PARITY
					softusart_dev->tx_parity = 0;
#endif
					break;
#ifdef PARITY
				case DATA_LENGTH:		
					if(softusart->tx_parity) 
						SET_TX;
					else
						CLR_TX;
					break;
#else
#ifdef BIT9
				case DATA_LENGTH:		
					if(softusart_dev->tx_bit9)
						SET_TX;
					else
						CLR_TX;
					break;
#endif
#endif
				case DATA_LENGTH+1:	
					PIOS_SOFTUSART_SetStatus(softusart_dev,TRANSMIT_DATA_REG_EMPTY);
				case DATA_LENGTH+2:	
					SET_TX;	//stop bit(s) transmition
					break;
#ifdef PARITY
				default:	
					if(softusart_dev->tx_data & MSK_TAB[softusart_dev->tx_bit-1])
					{ 
						SET_TX; 
						softusart_dev->tx_parity = ~softusart_dev->tx_parity;
					}
					else
						CLR_TX; // parity transmition
#else
				default:	
					if(softusart_dev->tx_data & MSK_TAB[softusart_dev->tx_bit-1])
						SET_TX; //  data bits
					else
						CLR_TX; // transmition

#endif
			};
			if(softusart_dev->tx_bit >= DATA_LENGTH + STOP_BITS) {
			    // end of current transmited bit

				softusart_dev->tx_phase = 0;
				softusart_dev->tx_bit = 0;

				// This should have been set with stop bit.  If this is hit then there is a race condition with
				// TxStart being called
				PIOS_Assert(PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY));
				uint8_t b;
				uint16_t bytes_to_send;
				
				bytes_to_send = (softusart_dev->tx_out_cb)(softusart_dev->tx_out_context, &b, 1, NULL, &yield);
				
				if (bytes_to_send > 0) {
					/* Send the byte we've been given */
					softusart_dev->tx_data = b;
					
					// Clear the empty flag
					PIOS_SOFTUSART_ClrStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY);
					
					PIOS_SOFTUSART_SetStatus(softusart_dev, TRANSMIT_IN_PROGRESS);

#if defined(SLOW_STREAM)
					// Now don't start the next transmission but let it be pickedu p
					GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->rx.pin.init);
					PIOS_SOFTUSART_SetCCE(softusart_dev, true);
					PIOS_SOFTUSART_ClrStatus(softusart_dev, TRANSMIT_IN_PROGRESS);
#endif
				} else {
					// Disable output mode on the GPIO pin
					GPIO_Init(softusart_dev->cfg->tx.pin.gpio, &softusart_dev->cfg->rx.pin.init);
					PIOS_SOFTUSART_SetCCE(softusart_dev, true);
					PIOS_SOFTUSART_ClrStatus(softusart_dev, TRANSMIT_IN_PROGRESS);
				}
			}
			else
				++softusart_dev->tx_bit;
		} else if(!PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_DATA_REG_EMPTY) && 
			!PIOS_SOFTUSART_TestStatus(softusart_dev, RECEIVE_IN_PROGRESS)) {
				softusart_dev->tx_phase = 0;
				softusart_dev->tx_bit = 0;
				PIOS_SOFTUSART_SetStatus(softusart_dev, TRANSMIT_IN_PROGRESS);
				PIOS_LED_Toggle(0);
		}
	};
	softusart_dev->tx_phase = !softusart_dev->tx_phase;
#if defined(PIOS_INCLUDE_FREERTOS)
	if (yield) {
		vPortYieldFromISR();
	}
#endif	/* PIOS_INCLUDE_FREERTOS */
	return;
}

/**
 * @brief Enable or disable the capture compare interrupt for the rx channel
 */
static void PIOS_SOFTUSART_SetIrqCC(struct pios_softusart_dev *softusart_dev, bool enable)
{
	TIM_ITConfig(softusart_dev->cfg->rx.timer, softusart_dev->tim_it, enable ? ENABLE : DISABLE);
}

/**
 * @brief Enable or disable the capture compare interrupt for the rx channel
 */
static void PIOS_SOFTUSART_SetCCE(struct pios_softusart_dev *softusart_dev, bool enable)
{
	if (enable)
		softusart_dev->cfg->rx.timer->CCER |= softusart_dev->cce;
	else 
		softusart_dev->cfg->rx.timer->CCER &= ~softusart_dev->cce;
}

/**
 * @brief Disable output capture and enable compare mode
 * Now the interrupt should occurr periodically at 2x baud rate for sampling line
 */
static void PIOS_SOFTUSART_EnableCompareMode(struct pios_softusart_dev *softusart_dev, int16_t count)
{
	//disable_IC_system;			    // IC interrupt - begin of start bit detected
	PIOS_SOFTUSART_SetIrqCC(softusart_dev, false);
	PIOS_SOFTUSART_SetCCE(softusart_dev, false);
	
	TIM_OCInitTypeDef tim_oc_init = {
		.TIM_OCMode = TIM_OCMode_PWM1,
		.TIM_OutputState = TIM_OutputState_Enable,
		.TIM_OutputNState = TIM_OutputNState_Disable,
		.TIM_Pulse = count,
		.TIM_OCPolarity = TIM_OCPolarity_High,
		.TIM_OCNPolarity = TIM_OCPolarity_High,
		.TIM_OCIdleState = TIM_OCIdleState_Reset,
		.TIM_OCNIdleState = TIM_OCNIdleState_Reset,
	};
	switch(softusart_dev->cfg->rx.timer_chan) {
		case TIM_Channel_1:
			softusart_dev->cfg->rx.timer->CCMR1 &= ~0x00ff;
			TIM_OC1Init(softusart_dev->cfg->rx.timer, &tim_oc_init);
			break;
		case TIM_Channel_2:
			softusart_dev->cfg->rx.timer->CCMR1 &= ~0xff00;
			TIM_OC2Init(softusart_dev->cfg->rx.timer, &tim_oc_init);
			break;
		case TIM_Channel_3:
			softusart_dev->cfg->rx.timer->CCMR2 &= ~0x00ff;
			TIM_OC3Init(softusart_dev->cfg->rx.timer, &tim_oc_init);
			break;
		case TIM_Channel_4:
			softusart_dev->cfg->rx.timer->CCMR2 &= ~0xff00;
			TIM_OC4Init(softusart_dev->cfg->rx.timer, &tim_oc_init);
			break;	
	}
	
	//Reenable interrupt
	PIOS_SOFTUSART_SetCCE(softusart_dev, true);
	PIOS_SOFTUSART_SetIrqCC(softusart_dev, true);
}

/**
 * @brief Disable output compare and enable capture mode
 */
static void PIOS_SOFTUSART_EnableCaptureMode(struct pios_softusart_dev *softusart_dev)
{
	//disable_OC_system;
	PIOS_SOFTUSART_SetIrqCC(softusart_dev, false);
	PIOS_SOFTUSART_SetCCE(softusart_dev, false);
	
	
	//Enable_IC_system.  Use a digital input filter.
	switch(softusart_dev->cfg->rx.timer_chan) {
		case TIM_Channel_1:
			softusart_dev->cfg->rx.timer->CCMR1 &= ~0x00ff;
			softusart_dev->cfg->rx.timer->CCMR1 |=  0x0011;
			break;
		case TIM_Channel_2:
			softusart_dev->cfg->rx.timer->CCMR1 &= ~0xff00;
			softusart_dev->cfg->rx.timer->CCMR1 |=  0x1100;
			break;
		case TIM_Channel_3:
			softusart_dev->cfg->rx.timer->CCMR2 &= ~0x00ff;
			softusart_dev->cfg->rx.timer->CCMR2 |=  0x0011;
			break;
		case TIM_Channel_4:
			softusart_dev->cfg->rx.timer->CCMR2 &= ~0xff00;
			softusart_dev->cfg->rx.timer->CCMR2 |=  0x1100;
			break;  
	}
	
	//softusart_dev->cfg->rx.timer->CCER |= softusart_dev->ccp;
	TIM_ICInitTypeDef tim_ic_init = {
		.TIM_Channel = softusart_dev->cfg->rx.timer_chan,
		.TIM_ICPolarity = TIM_ICPolarity_Falling,
		.TIM_ICSelection = TIM_ICSelection_DirectTI,
		.TIM_ICPrescaler = TIM_ICPSC_DIV1,
		.TIM_ICFilter = 0x4,
	};
	TIM_ICInit(softusart_dev->cfg->rx.timer, &tim_ic_init);

	//Reenable interrupt
	PIOS_SOFTUSART_SetCCE(softusart_dev, true);
	PIOS_SOFTUSART_SetIrqCC(softusart_dev, true);
}

/**
 * @brief IRQ callback for the timer capture event.  Decodes pulses into USART data.
 * @note This function works in two modes.  When looking for a start edge it is in capture
 * mode and trying to find the CCR value to sample the line.  Once engaged it goes into 
 * compare mode and on the interrupt checks the line for being high or low
 */
static void PIOS_SOFTUSART_tim_edge_cb (uint32_t tim_id, uint32_t context, uint8_t chan_idx, uint16_t count)
{
	/* Recover our device context */
	struct pios_softusart_dev *softusart_dev = (struct pios_softusart_dev *) context;
	
	if (!PIOS_SOFTUSART_validate(softusart_dev)) {
		/* Invalid device specified */
		return;
	}

	if (softusart_dev->active == false)
		return;

	if (chan_idx >= (softusart_dev->cfg->half_duplex ? 1 : 2) ) {
		/* Channel out of range */
		return;
	}
	
	bool yield = false;
	
	if(PIOS_SOFTUSART_TestStatus(softusart_dev, RECEIVE_IN_PROGRESS) && !PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_IN_PROGRESS)) {

		if(!softusart_dev->rx_phase) {
		    // Only process every other interrupt to get out of phase measurement
			
			softusart_dev->rx_samp= 0;            // middle of current bit, checking samples			
			if(RX_TEST)	softusart_dev->rx_samp++; // sampling in the middle of current bit
			if(RX_TEST)	softusart_dev->rx_samp++;
			if(RX_TEST) softusart_dev->rx_samp++;

			if(softusart_dev->rx_bit == 0) {
				if(softusart_dev->rx_samp == 0) {  // start bit!
					softusart_dev->rx_bit = 1;     // correctly received, continue
					softusart_dev->rx_buff = 0;

				} else {					// noise in start bit, find next one
					PIOS_SOFTUSART_ClrStatus(softusart_dev, RECEIVE_IN_PROGRESS);
					PIOS_SOFTUSART_EnableCaptureMode(softusart_dev);
				};
			}
			else {
				
				switch(softusart_dev->rx_samp) {		// any other bit, results?
					case 1:	
						PIOS_SOFTUSART_SetStatus(softusart_dev, RECEIVE_NOISE_ERROR);
						break;	// noise in middle samples, "0" received
					case 2: 	
						PIOS_SOFTUSART_SetStatus(softusart_dev, RECEIVE_NOISE_ERROR);
						// noise in middle samples, "1" received
#ifdef PARITY
					case 3:	
						if(softusart_dev->rx_bit < DATA_LENGTH)
							softusart_dev->rx_buff|= MSK_TAB[softusart_dev->rx_bit-1];
						if(softusart_dev->rx_bit <= DATA_LENGTH)
							softusart_dev->rx_parity = ~softusart_dev->rx_parity;
#else
#ifdef BIT9
					case 3:	
						if(softusart_dev->rx_bit < DATA_LENGTH)
							softusart_dev->rx_buff |= MSK_TAB[softusart_dev->rx_bit-1];
						if(softusart_dev->rx_bit == DATA_LENGTH)
							softusart_dev->rx_bit9=  1;
#else
					case 3:
						if(softusart_dev->rx_bit <= DATA_LENGTH)
							softusart_dev->rx_buff |= MSK_TAB[softusart_dev->rx_bit-1];
#endif
#endif
						break;	// "1" correctly received
				};
				if(softusart_dev->rx_bit > DATA_LENGTH) {
#ifdef PARITY
					// stop bit(s) are received, results?
					if(softusart_dev->rx_samp != 3  || softusart_dev->rx_parity)
						PIOS_SOFTUSART_SetStatus(softusart_dev,RECEIVE_FRAME_ERROR);// noise in stop bit or parity error
#else
					if(softusart_dev->rx_samp != 3)	// stop bit(s) are received, results?
						PIOS_SOFTUSART_SetStatus(softusart_dev,RECEIVE_FRAME_ERROR);// noise in stop bit or parity error
#endif						
					if(softusart_dev->rx_bit >= DATA_LENGTH + STOP_BITS) {
						
						// end of receive
						uint16_t rc;
						rc = (softusart_dev->rx_in_cb)(softusart_dev->rx_in_context, &softusart_dev->rx_buff, 1, NULL, &yield);
						if (rc < 1) {
							// Lost bytes on rx
							softusart_dev->rx_dropped += 1;
							PIOS_SOFTUSART_SetStatus(softusart_dev,RECEIVE_BUFFER_OVERFLOW);
						}

						yield = false;
#ifdef BIT9
						if(softusart_dev->rx_bit9)
							PIOS_SOFTUSART_SetStatus(softusart_dev,RECEIVED_9TH_DATA_BIT);
						else
							PIOS_SOFTUSART_ClrStatus(softusart_dev,RECEIVED_9TH_DATA_BIT);
#endif
						PIOS_SOFTUSART_ClrStatus(softusart_dev,RECEIVE_IN_PROGRESS);
						PIOS_SOFTUSART_EnableCaptureMode(softusart_dev);

					}
					else
						softusart_dev->rx_bit++;
					
				}
				else
					softusart_dev->rx_bit++;
			}
		}
		softusart_dev->rx_phase = !softusart_dev->rx_phase;
	} else if (!PIOS_SOFTUSART_TestStatus(softusart_dev, TRANSMIT_IN_PROGRESS)) {
		// receive is not in progres yet
		PIOS_SOFTUSART_EnableCompareMode(softusart_dev, count);
		PIOS_SOFTUSART_SetStatus(softusart_dev,RECEIVE_IN_PROGRESS);	// receive byte initialization
		softusart_dev->rx_bit = 0;
		softusart_dev->rx_phase = 0;
#ifdef PARITY
		softusart_dev->rx_parity = 0;
#else
#ifdef BIT9
		softusart_dev->rx_bit9 = 0;
#endif
#endif		
	};

#if defined(PIOS_INCLUDE_FREERTOS)
	if (yield) {
		vPortYieldFromISR();
	}
#endif	/* PIOS_INCLUDE_FREERTOS */
}

#endif

/** 
 * @}
 * @}
 */