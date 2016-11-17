/* rs485-bricklet
 * Copyright (C) 2016 Olaf Lüke <olaf@tinkerforge.com>
 *
 * rs485.c: RS485 handling
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "rs485.h"

#include "configs/config.h"
#include "bricklib2/hal/uartbb/uartbb.h"
#include "bricklib2/hal/system_timer/system_timer.h"
#include "bricklib2/utility/ringbuffer.h"

#include "xmc_uart.h"
#include "xmc_scu.h"

extern RS485 rs485;

#define rs485_rx_irq_handler  IRQ_Hdlr_11
#define rs485_tx_irq_handler  IRQ_Hdlr_12
#define rs485_tff_irq_handler IRQ_Hdlr_13

#define RS485_HALF_DUPLEX_RX_ENABLE() (RS485_NRXE_AND_TXE_PORT->OMR = ((0x10000 << RS485_TXE_PIN_NUM) | (0x10000 << RS485_NRXE_PIN_NUM)))
#define RS485_HALF_DUPLEX_TX_ENABLE() (RS485_NRXE_AND_TXE_PORT->OMR = ((0x1     << RS485_TXE_PIN_NUM) | (0x1     << RS485_NRXE_PIN_NUM)))

void __attribute__((optimize("-O3"))) rs485_tff_irq_handler(void) {
	XMC_GPIO_SetOutputHigh(UARTBB_TX_PIN);
	if((RS485_USIC->PSR_ASCMode & (XMC_UART_CH_STATUS_FLAG_TRANSMITTER_FRAME_FINISHED | XMC_UART_CH_STATUS_FLAG_TRANSFER_STATUS_BUSY)) ==  XMC_UART_CH_STATUS_FLAG_TRANSMITTER_FRAME_FINISHED) {
		RS485_HALF_DUPLEX_RX_ENABLE();
	}
	XMC_GPIO_SetOutputLow(UARTBB_TX_PIN);
}

void __attribute__((optimize("-O3"))) rs485_rx_irq_handler(void) {
	XMC_GPIO_SetOutputHigh(UARTBB_TX_PIN);

	while(!XMC_USIC_CH_RXFIFO_IsEmpty(RS485_USIC)) {
		if(!ringbuffer_add(&rs485.ringbuffer_rx, RS485_USIC->OUTR)) {
			uartbb_puts("rb overflow!\n\r");
			XMC_GPIO_SetOutputLow(UARTBB_TX_PIN);
			return;
		}

//		rs485.ringbuffer_rx.buffer[rs485.ringbuffer_rx.end] = RS485_USIC->OUTR;
//		rs485.ringbuffer_rx.end = (rs485.ringbuffer_rx.end + 1) % rs485.buffer_size_rx;
	}

	XMC_GPIO_SetOutputLow(UARTBB_TX_PIN);
}

void __attribute__((optimize("-O3"))) rs485_tx_irq_handler(void) {
	XMC_GPIO_SetOutputHigh(UARTBB_TX_PIN);

	RS485_HALF_DUPLEX_TX_ENABLE();
	while(!XMC_USIC_CH_TXFIFO_IsFull(RS485_USIC)) {
		uint8_t data;
		if(!ringbuffer_get(&rs485.ringbuffer_tx, &data)) {
			XMC_USIC_CH_TXFIFO_DisableEvent(RS485_USIC, XMC_USIC_CH_TXFIFO_EVENT_CONF_STANDARD);
			XMC_GPIO_SetOutputLow(UARTBB_TX_PIN);
			return;
		}

		RS485_USIC->IN[0] = data;
	}

	XMC_GPIO_SetOutputLow(UARTBB_TX_PIN);
}

void rs485_init(RS485 *rs485) {
	// Initialize struct
	memset(rs485->buffer, 0, RS485_BUFFER_SIZE);
	// By default we use a 50/50 segmentation of rx/tx buffer
	rs485->buffer_size_rx = RS485_BUFFER_SIZE/2;
	// rx buffer is at buffer[0:buffer_size_rx]
	ringbuffer_init(&rs485->ringbuffer_rx, rs485->buffer_size_rx, &rs485->buffer[0]);
	// tx buffer is at buffer[buffer_size_rx:RS485_BUFFER_SIZE]
	ringbuffer_init(&rs485->ringbuffer_tx, RS485_BUFFER_SIZE-rs485->buffer_size_rx, &rs485->buffer[rs485->buffer_size_rx]);

	// USIC channel configuration
	const XMC_UART_CH_CONFIG_t channel_config = {
		.baudrate      = 1000000, //19200,
		.data_bits     = 8,
		.frame_length  = 8,
		.stop_bits     = 1,
		.oversampling  = 16,
		.parity_mode   = XMC_USIC_CH_PARITY_MODE_NONE
	};

	// TX pin configuration
	const XMC_GPIO_CONFIG_t tx_pin_config = {
		.mode             = RS485_TX_PIN_AF,
		.output_level     = XMC_GPIO_OUTPUT_LEVEL_HIGH
	};

	// RX pin configuration
	const XMC_GPIO_CONFIG_t rx_pin_config = {
		.mode             = XMC_GPIO_MODE_INPUT_PULL_UP,
		.input_hysteresis = XMC_GPIO_INPUT_HYSTERESIS_STANDARD
	};

	// TXE (tx enable) pin configuration
	const XMC_GPIO_CONFIG_t txe_pin_config = {
		.mode             = XMC_GPIO_MODE_OUTPUT_PUSH_PULL,
		.output_level     = XMC_GPIO_OUTPUT_LEVEL_LOW
	};

	// NRXE (not rx enable) pin configuration
	const XMC_GPIO_CONFIG_t nrxe_pin_config = {
		.mode             = XMC_GPIO_MODE_OUTPUT_PUSH_PULL,
		.output_level     = XMC_GPIO_OUTPUT_LEVEL_LOW
	};

	// Configure  pins
	XMC_GPIO_Init(RS485_TX_PIN, &tx_pin_config);
	XMC_GPIO_Init(RS485_RX_PIN, &rx_pin_config);
	XMC_GPIO_Init(RS485_TXE_PIN, &txe_pin_config);
	XMC_GPIO_Init(RS485_NRXE_PIN, &nrxe_pin_config);

	// Initialize USIC channel in UART master mode
	XMC_UART_CH_Init(RS485_USIC, &channel_config);

	// Set input source path
	XMC_UART_CH_SetInputSource(RS485_USIC, RS485_RX_INPUT, RS485_RX_SOURCE);

	// Configure transmit FIFO
	XMC_USIC_CH_TXFIFO_Configure(RS485_USIC, 32, XMC_USIC_CH_FIFO_SIZE_32WORDS, 16);

	// Configure receive FIFO
	XMC_USIC_CH_RXFIFO_Configure(RS485_USIC, 0, XMC_USIC_CH_FIFO_SIZE_32WORDS, 0);

	// UART protocol events
	XMC_USIC_CH_SetInterruptNodePointer(RS485_USIC, XMC_USIC_CH_INTERRUPT_NODE_POINTER_PROTOCOL, /*RS485_SERVICE_REQUEST_TX*/ 4);
	XMC_UART_CH_EnableEvent(RS485_USIC, XMC_UART_CH_EVENT_FRAME_FINISHED);

	// Set service request for tx FIFO transmit interrupt
	XMC_USIC_CH_TXFIFO_SetInterruptNodePointer(RS485_USIC, XMC_USIC_CH_TXFIFO_INTERRUPT_NODE_POINTER_STANDARD, RS485_SERVICE_REQUEST_TX);  // IRQ FIREFLY_X1_IRQ_TX

	// Set service request for rx FIFO receive interrupt
	XMC_USIC_CH_RXFIFO_SetInterruptNodePointer(RS485_USIC, XMC_USIC_CH_RXFIFO_INTERRUPT_NODE_POINTER_STANDARD, RS485_SERVICE_REQUEST_RX);  // IRQ FIREFLY_X1_IRQ_RX
	XMC_USIC_CH_RXFIFO_SetInterruptNodePointer(RS485_USIC, XMC_USIC_CH_RXFIFO_INTERRUPT_NODE_POINTER_ALTERNATE, RS485_SERVICE_REQUEST_RX); // IRQ FIREFLY_X1_IRQ_RX

	// Set priority and enable NVIC node for transfer frame finished interrupt
	NVIC_SetPriority((IRQn_Type)RS485_IRQ_TFF, RS485_IRQ_TFF_PRIORITY);
	XMC_SCU_SetInterruptControl(RS485_IRQ_TFF, RS485_IRQCTRL_TFF);
	NVIC_EnableIRQ((IRQn_Type)RS485_IRQ_TFF);

	// Set priority and enable NVIC node for transmit interrupt
	NVIC_SetPriority((IRQn_Type)RS485_IRQ_TX, RS485_IRQ_TX_PRIORITY);
	XMC_SCU_SetInterruptControl(RS485_IRQ_TX, RS485_IRQCTRL_TX);
	NVIC_EnableIRQ((IRQn_Type)RS485_IRQ_TX);

	// Set priority and enable NVIC node for receive interrupt
	NVIC_SetPriority((IRQn_Type)RS485_IRQ_RX, RS485_IRQ_RX_PRIORITY);
	XMC_SCU_SetInterruptControl(RS485_IRQ_RX, RS485_IRQCTRL_RX);
	NVIC_EnableIRQ((IRQn_Type)RS485_IRQ_RX);

	// Start UART
	XMC_UART_CH_Start(RS485_USIC);

//	XMC_USIC_CH_EnableEvent(RS485_USIC, (uint32_t)((uint32_t)XMC_USIC_CH_EVENT_STANDARD_RECEIVE | (uint32_t)XMC_USIC_CH_EVENT_ALTERNATIVE_RECEIVE));
	XMC_USIC_CH_RXFIFO_EnableEvent(RS485_USIC, XMC_USIC_CH_RXFIFO_EVENT_CONF_STANDARD | XMC_USIC_CH_RXFIFO_EVENT_CONF_ALTERNATE);
}

void rs485_tick(RS485 *rs485) {
	static char counter = '1';
	static uint32_t t = 0;
	if(system_timer_is_time_elapsed_ms(t, 1000)) {
		if(counter == '9') {
			counter = '1';
		} else {
			counter++;
		}

		char s[] = "  Hello World!";
		s[0] = counter;

		for(uint8_t i = 0; i < sizeof(s)/sizeof(s[0]) - 1; i++) {
			ringbuffer_add(&rs485->ringbuffer_tx, (uint8_t)s[i]);
		}

		t = system_timer_get_ms();

		XMC_USIC_CH_TXFIFO_EnableEvent(RS485_USIC, XMC_USIC_CH_TXFIFO_EVENT_CONF_STANDARD);
		XMC_USIC_CH_TriggerServiceRequest(RS485_USIC, RS485_SERVICE_REQUEST_TX);
	}

	uint8_t data;
	if(ringbuffer_get(&rs485->ringbuffer_rx, &data)) {
//		uartbb_puts("recv: ");
//		uartbb_puti(data);
//		uartbb_putnl();
	}
}
