	#include "stm32f411xe.h"
	#include <stdint.h>
	#include <stdio.h>

	/* =====================================================================
	 *  BUILD OPTION
	 *  WEB_MODE 0 : your original standalone behaviour (card -> button ->
	 *               OTP to phone -> OTP typed on the phone over Bluetooth).
	 *               USART1 is initialised but only answers PING.
	 *  WEB_MODE 1 : the website controls the login over USART1 (PA9/PA10).
	 *               The board only scans for a card after the backend sends
	 *               AUTH_START, and the OTP must come back over USART1.
	 *  You can also set it in Keil: Options > C/C++ > Define: WEB_MODE=0
	 * ===================================================================== */
	#ifndef WEB_MODE
	#define WEB_MODE 1   /* 1 = website flow (this build); set to 0 for the original standalone behaviour */
	#endif

	/* ---------------- Pin definitions ---------------- */
	#define BUTTON_PIN       0U
	#define RED_LED_PIN      12U
	#define GREEN_LED_PIN    13U

	#define RFID_CS_PIN      4U
	#define RFID_RST_PIN     0U
	#define LCD_ADDR         0x27U

	#define LCD_RS           0x01U
	#define LCD_EN           0x04U
	#define LCD_BACKLIGHT    0x08U

	/* MFRC522 Commands & Registers */
	#define CommandReg       0x01U
	#define CommIEnReg       0x02U
	#define DivIEnReg        0x03U
	#define CommIrqReg       0x04U
	#define DivIrqReg        0x05U
	#define ErrorReg         0x06U
	#define Status2Reg       0x08U
	#define FIFODataReg      0x09U
	#define FIFOLevelReg     0x0AU
	#define ControlReg       0x0CU
	#define BitFramingReg    0x0DU
	#define ModeReg          0x11U
	#define TxModeReg        0x12U
	#define RxModeReg        0x13U
	#define ModWidthReg      0x24U
	#define TxControlReg     0x14U
	#define RFCfgReg         0x26U
	#define TxASKReg         0x15U
	#define CRCResultRegH    0x21U
	#define CRCResultRegL    0x22U
	#define TModeReg         0x2AU
	#define TPrescalerReg    0x2BU
	#define TReloadRegH      0x2CU
	#define TReloadRegL      0x2DU
	#define VersionReg       0x37U

	#define PCD_Idle         0x00U
	#define PCD_CalcCRC      0x03U
	#define PCD_Transceive   0x0CU
	#define PCD_SoftReset    0x0FU

	#define PICC_REQA        0x26U
	#define PICC_WUPA        0x52U
	#define PICC_SEL_CL1     0x93U
	#define PICC_SEL_CL2     0x95U
	#define PICC_SEL_CL3     0x97U
	#define PICC_CT          0x88U
	#define PICC_CMD_SEL     0x70U

	#define BT_RX_BUFFER_SIZE 32U

	/* ---------------- NEW: website link settings ---------------- */
	#define PC_LINE_MAX       32U      /* longest accepted command line (incl. NUL) */
	#define PC_QUEUE_LEN      3U       /* holds up to 2 unread command lines        */
	#define SCAN_TIMEOUT_MS   30000U   /* web mode: time to tap the card            */
	#define BUTTON_TIMEOUT_MS 30000U   /* 30 seconds to press the board button      */
	#define OTP_TIMEOUT_MS    30000U   /* same 30 s as your original code           */
	#define MAX_BAD_CARDS     3U
	#define MAX_OTP_TRIES     3U

	#if WEB_MODE
		#define START_TEXT  "WEB LOGIN READY"
		#define WEB_EVT(h, t) PC_SendMsg((h), (t))
	#else
		#define START_TEXT  "SCAN RFID CARD"
		#define WEB_EVT(h, t) ((void)0)
	#endif

	/* ---------------- Global State & SysTick ---------------- */
	volatile uint32_t sysTickCounter = 0;
	static volatile char btRxBuffer[BT_RX_BUFFER_SIZE];
	static volatile uint8_t btRxIndex = 0U;
	static volatile uint8_t btCommandReady = 0U;

	/* NEW: USART1 (PC) receive queue, filled by the interrupt */
	static volatile char    pcQueue[PC_QUEUE_LEN][PC_LINE_MAX];
	static volatile uint8_t pcQHead = 0U;       /* written by the interrupt only */
	static volatile uint8_t pcQTail = 0U;       /* written by main loop only     */
	static volatile uint8_t pcLineIndex = 0U;
	static volatile uint8_t pcLineOverflow = 0U;

	static const uint8_t authorizedUID[4] = { 0x26U, 0xE4U, 0xB2U, 0xE3U };

	typedef enum {
			STATE_IDLE,            /* NEW: web mode, waiting for AUTH_START */
			STATE_SCAN_RFID,
			STATE_WAIT_BUTTON,
			STATE_WAIT_PC_OTP,
			STATE_GRANTED
	} SystemState_t;

	/* These were locals in main(); the website command handler needs them */
	static SystemState_t currentState = STATE_SCAN_RFID;
	static char      generatedOTP[7] = {0};
	static uint32_t  timeoutCounter = 0U;
	static uint32_t  countdownLastSecond = 0xFFFFFFFFU;
	static char      txId[5] = {0};            /* current transaction id, 4 hex chars */
	static uint8_t   otpAttempts = 0U;
	static uint8_t   badCards = 0U;

	/* SysTick Interrupt Handler */
	void SysTick_Handler(void) {
			sysTickCounter++;
	}

	static void delay(volatile uint32_t count) {
			while (count--) { __NOP(); }
	}

	/* ---------------- I2C & LCD Functions ---------------- */
	static uint8_t I2C1_WaitFlag(volatile uint32_t *reg, uint32_t mask, uint8_t set) {
			uint32_t timeout = 100000U;
			while (timeout--) {
					if (set) { if ((*reg & mask) != 0U) return 1U; } 
					else     { if ((*reg & mask) == 0U) return 1U; }
			}
			return 0U;
	}

	static void I2C1_Init(void) {
			RCC->AHB1ENR |= (1U << 1U);
			RCC->APB1ENR |= (1U << 21U);
			GPIOB->MODER &= ~((3U << 16U) | (3U << 18U));
			GPIOB->MODER |=  ((2U << 16U) | (2U << 18U));
			GPIOB->OTYPER |= (1U << 8U) | (1U << 9U);
			GPIOB->OSPEEDR |= (3U << 16U) | (3U << 18U);
			GPIOB->PUPDR &= ~((3U << 16U) | (3U << 18U));
			GPIOB->AFR[1] &= ~((15U << 0U) | (15U << 4U));
			GPIOB->AFR[1] |=  ((4U << 0U) | (4U << 4U));
			RCC->APB1RSTR |= (1U << 21U);
			RCC->APB1RSTR &= ~(1U << 21U);
			I2C1->CR1 = 0U;
			I2C1->CR2 = 16U;
			I2C1->CCR = 80U;
			I2C1->TRISE = 17U;
			I2C1->CR1 |= (1U << 0U);
	}

	static uint8_t I2C1_Write(uint8_t data) {
			uint32_t timeout = 100000U;
			while (I2C1->SR2 & (1U << 1U)) { if (--timeout == 0U) return 0U; }
			I2C1->CR1 |= (1U << 8U);
			if (!I2C1_WaitFlag(&I2C1->SR1, (1U << 0U), 1U)) return 0U;
			I2C1->DR = (uint8_t)(LCD_ADDR << 1U);
			if (!I2C1_WaitFlag(&I2C1->SR1, (1U << 1U), 1U)) { I2C1->CR1 |= (1U << 9U); return 0U; }
			(void)I2C1->SR1; (void)I2C1->SR2;
			if (!I2C1_WaitFlag(&I2C1->SR1, (1U << 7U), 1U)) { I2C1->CR1 |= (1U << 9U); return 0U; }
			I2C1->DR = data;
			if (!I2C1_WaitFlag(&I2C1->SR1, (1U << 2U), 1U)) { I2C1->CR1 |= (1U << 9U); return 0U; }
			I2C1->CR1 |= (1U << 9U);
			return 1U;
	}

	static void LCD_WriteNibble(uint8_t nibble, uint8_t rs) {
			uint8_t data = (uint8_t)((nibble & 0xF0U) | LCD_BACKLIGHT);
			if (rs) data |= LCD_RS;
			(void)I2C1_Write((uint8_t)(data | LCD_EN));
			delay(1000U);
			(void)I2C1_Write((uint8_t)(data & (uint8_t)~LCD_EN));
			delay(1000U);
	}

	static void LCD_Send(uint8_t value, uint8_t rs) {
			LCD_WriteNibble((uint8_t)(value & 0xF0U), rs);
			LCD_WriteNibble((uint8_t)(value << 4U), rs);
	}

	static void LCD_Command(uint8_t command) {
			LCD_Send(command, 0U);
			if (command == 0x01U || command == 0x02U) delay(50000U);
	}

	static void LCD_Data(uint8_t data) { LCD_Send(data, 1U); }

	static void LCD_Init(void) {
			delay(200000U);
			LCD_WriteNibble(0x30U, 0U); delay(50000U);
			LCD_WriteNibble(0x30U, 0U); delay(10000U);
			LCD_WriteNibble(0x30U, 0U); delay(10000U);
			LCD_WriteNibble(0x20U, 0U);
			LCD_Command(0x28U);
			LCD_Command(0x0CU);
			LCD_Command(0x06U);
			LCD_Command(0x01U);
	}

	static void LCD_SetCursor(uint8_t row, uint8_t col) {
			uint8_t address = (row == 0U) ? col : (uint8_t)(0x40U + col);
			LCD_Command((uint8_t)(0x80U | address));
	}

	static void LCD_Print(const char *text) {
			while (*text) LCD_Data((uint8_t)*text++);
	}

	/* Draws a countdown on the second LCD row. The LCD is 16x2. */
	static void LCD_ShowCountdown(const char *label, uint32_t durationMs) {
			uint32_t elapsed = sysTickCounter - timeoutCounter;
			uint32_t remaining = (elapsed < durationMs)
					? (durationMs - elapsed + 999U) / 1000U : 0U;
			if (remaining > 99U) remaining = 99U;
			if (remaining == countdownLastSecond) return;
			countdownLastSecond = remaining;
			LCD_Command(0x01U);
			LCD_Print(label);
			LCD_SetCursor(1U, 0U);
			LCD_Print("TIME LEFT: ");
			LCD_Data((uint8_t)('0' + (remaining / 10U)));
			LCD_Data((uint8_t)('0' + (remaining % 10U)));
			LCD_Data('s');
	}

	/* ---------------- SPI & RFID Functions ---------------- */
	static void SPI1_Init(void) {
			RCC->AHB1ENR |= (1U << 0U) | (1U << 1U);
			RCC->APB2ENR |= (1U << 12U);
			GPIOA->MODER &= ~((3U << 10U) | (3U << 12U) | (3U << 14U));
			GPIOA->MODER |=  ((2U << 10U) | (2U << 12U) | (2U << 14U));
			GPIOA->AFR[0] &= ~((15U << 20U) | (15U << 24U) | (15U << 28U));
			GPIOA->AFR[0] |=  ((5U << 20U) | (5U << 24U) | (5U << 28U));
			GPIOA->OTYPER &= ~((1U << 5U) | (1U << 6U) | (1U << 7U));
			GPIOA->MODER &= ~(3U << 8U); GPIOA->MODER |=  (1U << 8U);
			GPIOB->MODER &= ~(3U << 0U); GPIOB->MODER |=  (1U << 0U);
			GPIOA->BSRR = (1U << RFID_CS_PIN);
			GPIOB->BSRR = (1U << RFID_RST_PIN);
			SPI1->CR1 = 0U;
			SPI1->CR1 |= (1U << 2U) | (1U << 8U) | (1U << 9U) | (3U << 3U) | (1U << 6U);
	}

	static uint8_t SPI1_Transfer(uint8_t data) {
			uint32_t timeout = 100000U;
			while (!(SPI1->SR & (1U << 1U))) { if (--timeout == 0U) return 0x00U; }
			*(__IO uint8_t *)&SPI1->DR = data;
			timeout = 100000U;
			while (!(SPI1->SR & (1U << 0U))) { if (--timeout == 0U) return 0x00U; }
			return *(__IO uint8_t *)&SPI1->DR;
	}

	static void RFID_WriteRegister(uint8_t reg, uint8_t value) {
			GPIOA->BSRR = (1U << (RFID_CS_PIN + 16U));
			(void)SPI1_Transfer((uint8_t)(((uint32_t)reg << 1U) & 0x7EU));
			(void)SPI1_Transfer(value);
			GPIOA->BSRR = (1U << RFID_CS_PIN);
	}

	static uint8_t RFID_ReadRegister(uint8_t reg) {
			uint8_t value;
			GPIOA->BSRR = (1U << (RFID_CS_PIN + 16U));
			(void)SPI1_Transfer((uint8_t)((((uint32_t)reg << 1U) & 0x7EU) | 0x80U));
			value = SPI1_Transfer(0x00U);
			GPIOA->BSRR = (1U << RFID_CS_PIN);
			return value;
	}

	static void RFID_SetBitMask(uint8_t reg, uint8_t mask) {
			RFID_WriteRegister(reg, (uint8_t)(RFID_ReadRegister(reg) | mask));
	}

	static void RFID_ClearBitMask(uint8_t reg, uint8_t mask) {
			RFID_WriteRegister(reg, (uint8_t)(RFID_ReadRegister(reg) & (uint8_t)~mask));
	}

	static void RFID_Init(void) {
			GPIOB->BSRR = (1U << (RFID_RST_PIN + 16U)); delay(10000U);
			GPIOB->BSRR = (1U << RFID_RST_PIN); delay(10000U);
			RFID_WriteRegister(CommandReg, PCD_SoftReset); delay(100000U);
			RFID_WriteRegister(TModeReg, 0x8DU);
			RFID_WriteRegister(TPrescalerReg, 0x3EU);
			RFID_WriteRegister(TReloadRegL, 30U);
			RFID_WriteRegister(TReloadRegH, 0U);
			RFID_WriteRegister(TxModeReg, 0x00U);
			RFID_WriteRegister(RxModeReg, 0x00U);
			RFID_WriteRegister(ModWidthReg, 0x26U);
			RFID_WriteRegister(TxASKReg, 0x40U);
			RFID_WriteRegister(ModeReg, 0x3DU);
			RFID_WriteRegister(RFCfgReg, 0x70U);
			RFID_WriteRegister(BitFramingReg, 0x00U);
			RFID_WriteRegister(CommandReg, PCD_Idle);
			uint8_t value = RFID_ReadRegister(TxControlReg);
			if ((value & 0x03U) != 0x03U) RFID_SetBitMask(TxControlReg, 0x03U);
	}

	static uint8_t RFID_Transceive(const uint8_t *sendData, uint8_t sendLen, uint8_t *backData, uint8_t *backLen) {
			uint8_t irq = 0U, n, count;
			uint32_t wait = 100000U;
			if (sendLen == 0U || backLen == 0) return 0U;
			RFID_WriteRegister(CommandReg, PCD_Idle);
			RFID_WriteRegister(CommIrqReg, 0x7FU);
			RFID_WriteRegister(FIFOLevelReg, 0x80U);
			RFID_WriteRegister(ErrorReg, 0x00U);
			for (count = 0U; count < sendLen; count++) RFID_WriteRegister(FIFODataReg, sendData[count]);
			RFID_WriteRegister(CommandReg, PCD_Transceive);
			RFID_SetBitMask(BitFramingReg, 0x80U);
			do { irq = RFID_ReadRegister(CommIrqReg); if ((irq & 0x30U) != 0U || (irq & 0x01U) != 0U) break; } while (--wait != 0U);
			RFID_ClearBitMask(BitFramingReg, 0x80U);
			if (wait == 0U || (irq & 0x01U) != 0U) return 0U;
			if ((RFID_ReadRegister(ErrorReg) & 0x1BU) != 0U) return 0U;
			if ((irq & 0x20U) == 0U) return 0U;
			n = RFID_ReadRegister(FIFOLevelReg);
			if (n > *backLen) return 0U;
			*backLen = n;
			for (count = 0U; count < n; count++) backData[count] = RFID_ReadRegister(FIFODataReg);
			return 1U;
	}

	static uint8_t RFID_Request(uint8_t command, uint8_t *atqa) {
			uint8_t send[1], response[2], responseLen = 2U;
			RFID_WriteRegister(CommandReg, PCD_Idle);
			RFID_WriteRegister(BitFramingReg, 0x07U);
			send[0] = command;
			uint8_t ok = RFID_Transceive(send, 1U, response, &responseLen);
			RFID_WriteRegister(BitFramingReg, 0x00U);
			if (!ok || responseLen != 2U) return 0U;
			atqa[0] = response[0]; atqa[1] = response[1];
			return 1U;
	}

	static uint8_t RFID_AnticollisionLevel(uint8_t selectCode, uint8_t *levelData) {
			uint8_t send[2], response[5], responseLen = 5U, i, bcc = 0U;
			RFID_WriteRegister(BitFramingReg, 0x00U);
			send[0] = selectCode; send[1] = 0x20U;
			if (!RFID_Transceive(send, 2U, response, &responseLen) || responseLen != 5U) return 0U;
			for (i = 0U; i < 4U; i++) bcc ^= response[i];
			if (bcc != response[4]) return 0U;
			for (i = 0U; i < 5U; i++) levelData[i] = response[i];
			return 1U;
	}

	static uint8_t RFID_ReadUID(uint8_t *uid, uint8_t *uidLength) {
			uint8_t data[5];
			if (!RFID_AnticollisionLevel(PICC_SEL_CL1, data)) return 0U;
			for (uint8_t i = 0U; i < 4U; i++) uid[i] = data[i];
			*uidLength = 4U;
			return 1U;
	}

	/* ---------------- UART & HC-05 Functions (USART2, unchanged) ---------------- */
	static void USART2_Init(void) {
			RCC->AHB1ENR |= (1U << 0U);
			RCC->APB1ENR |= (1U << 17U);
			GPIOA->MODER &= ~((3U << 4U) | (3U << 6U));
			GPIOA->MODER |=  ((2U << 4U) | (2U << 6U));
			GPIOA->OTYPER &= ~((1U << 2U) | (1U << 3U));
			GPIOA->OSPEEDR &= ~((3U << 4U) | (3U << 6U));
			GPIOA->OSPEEDR |=  ((2U << 4U) | (2U << 6U));
			GPIOA->PUPDR &= ~((3U << 4U) | (3U << 6U));
			GPIOA->PUPDR |=  (1U << 6U);
			GPIOA->AFR[0] &= ~((15U << 8U) | (15U << 12U));
			GPIOA->AFR[0] |=  ((7U << 8U) | (7U << 12U));
			USART2->BRR = (16000000U + (9600U / 2U)) / 9600U;
			USART2->CR1 = (1U << 13U) | (1U << 3U) | (1U << 2U) | (1U << 5U);
			NVIC_SetPriority(USART2_IRQn, 3U);
			NVIC_EnableIRQ(USART2_IRQn);
	}

	void USART2_IRQHandler(void) {
			if (USART2->SR & (1U << 5U)) {
					char value = (char)(USART2->DR & 0xFFU);
					if (!btCommandReady) {
							if (value == '\r' || value == '\n') {
									if (btRxIndex > 0U) {
											btRxBuffer[btRxIndex] = '\0';
											btCommandReady = 1U;
											btRxIndex = 0U;
									}
							} else {
									if (btRxIndex < (BT_RX_BUFFER_SIZE - 1U)) btRxBuffer[btRxIndex++] = value;
							}
					}
			}
	}

	static void BT_SendString(const char *text) {
			while (*text != '\0') {
					while ((USART2->SR & (1U << 7U)) == 0U);
					USART2->DR = (uint8_t)(*text++);
			}
	}

	static uint8_t BT_GetCommand(char *destination) {
			if (!btCommandReady) return 0U;
			__disable_irq();
			uint8_t i = 0;
			while (btRxBuffer[i] != '\0') { destination[i] = btRxBuffer[i]; i++; }
			destination[i] = '\0';
			btCommandReady = 0U;
			__enable_irq();
			return 1U;
	}

	/* ---------------- NEW: USART1 link to the PC (PA9 = TX, PA10 = RX) ---------------- */
	static void USART1_Init(void) {
			RCC->AHB1ENR |= (1U << 0U);                         /* GPIOA clock          */
			RCC->APB2ENR |= (1U << 4U);                         /* USART1 clock (APB2)  */
			GPIOA->MODER   &= ~((3U << 18U) | (3U << 20U));     /* PA9, PA10 -> AF      */
			GPIOA->MODER   |=  ((2U << 18U) | (2U << 20U));
			GPIOA->OTYPER  &= ~((1U << 9U) | (1U << 10U));
			GPIOA->OSPEEDR &= ~((3U << 18U) | (3U << 20U));
			GPIOA->OSPEEDR |=  ((2U << 18U) | (2U << 20U));
			GPIOA->PUPDR   &= ~((3U << 18U) | (3U << 20U));
			GPIOA->PUPDR   |=  (1U << 20U);                     /* pull-up on PA10 (RX) */
			GPIOA->AFR[1]  &= ~((15U << 4U) | (15U << 8U));     /* AFRH: PA9 = bits 4-7, PA10 = bits 8-11 */
			GPIOA->AFR[1]  |=  ((7U << 4U) | (7U << 8U));       /* AF7 = USART1         */
			USART1->BRR = (16000000U + (9600U / 2U)) / 9600U;   /* 9600 baud, 16 MHz HSI */
			USART1->CR1 = (1U << 13U) | (1U << 3U) | (1U << 2U) | (1U << 5U); /* UE, TE, RE, RXNEIE */
			NVIC_SetPriority(USART1_IRQn, 3U);
			NVIC_EnableIRQ(USART1_IRQn);
	}

	/* Interrupt only collects bytes into a line queue; all real work is in main loop */
	void USART1_IRQHandler(void) {
			uint32_t sr = USART1->SR;
			if (sr & ((1U << 5U) | (1U << 3U))) {               /* RXNE or ORE */
					char value = (char)(USART1->DR & 0xFFU);        /* reading DR clears both */
					if (value == '\r' || value == '\n') {
							if (pcLineIndex > 0U && pcLineOverflow == 0U) {
									uint8_t next = (uint8_t)((pcQHead + 1U) % PC_QUEUE_LEN);
									if (next != pcQTail) {                  /* queue not full */
											pcQueue[pcQHead][pcLineIndex] = '\0';
											pcQHead = next;
									}
							}
							pcLineIndex = 0U;
							pcLineOverflow = 0U;
					} else if (pcLineIndex < (PC_LINE_MAX - 1U)) {
							pcQueue[pcQHead][pcLineIndex++] = value;
					} else {
							pcLineOverflow = 1U;                        /* line too long: drop it */
					}
			}
	}

	static void PC_SendString(const char *text) {
			while (*text != '\0') {
					while ((USART1->SR & (1U << 7U)) == 0U);
					USART1->DR = (uint8_t)(*text++);
			}
	}

	/* Sends  HEAD:<txId>[:TAIL]\r\n  e.g. "EVT:CARD_OK:a1b2" or "RES:FAIL:a1b2:EXPIRED" */
	static void PC_SendMsg(const char *head, const char *tail) {
			PC_SendString(head);
			PC_SendString(":");
			PC_SendString(txId);
			if (tail != 0) { PC_SendString(":"); PC_SendString(tail); }
			PC_SendString("\r\n");
	}

	static uint8_t PC_GetCommand(char *dst) {
			if (pcQTail == pcQHead) return 0U;
			uint8_t i = 0U;
			while (i < (PC_LINE_MAX - 1U) && pcQueue[pcQTail][i] != '\0') {
					dst[i] = pcQueue[pcQTail][i];
					i++;
			}
			dst[i] = '\0';
			pcQTail = (uint8_t)((pcQTail + 1U) % PC_QUEUE_LEN);
			return 1U;
	}

	/* ---------------- System Helper Functions ---------------- */
	static uint8_t UID_IsAuthorized(const uint8_t *uid, uint8_t uidLength) {
			if (uidLength != 4U) return 0U;
			for (uint8_t i = 0U; i < 4U; i++) { if (uid[i] != authorizedUID[i]) return 0U; }
			return 1U;
	}

	static void LEDs_SetLocked(void) {
			GPIOB->BSRR = (1U << RED_LED_PIN) | (1U << (GREEN_LED_PIN + 16U));
	}

	static void LEDs_SetGranted(void) {
			GPIOB->BSRR = (1U << GREEN_LED_PIN) | (1U << (RED_LED_PIN + 16U));
	}

	static void Generate_OTP(char *otpString) {
			// Generate a pseudo-random 6 digit number using SysTick as a seed
			uint32_t randomNum = (sysTickCounter * 1103515245 + 12345) % 1000000;
			sprintf(otpString, "%06lu", (unsigned long)randomNum);
	}

	/* ---------------- NEW: small string helpers (no <string.h> needed) ---------------- */
	static uint8_t StrLen(const char *s) {
			uint8_t n = 0U;
			while (s[n] != '\0' && n < 250U) n++;
			return n;
	}

	static uint8_t StartsWith(const char *s, const char *prefix) {
			while (*prefix != '\0') { if (*s++ != *prefix++) return 0U; }
			return 1U;
	}

	static uint8_t IsHex4(const char *s) {                  /* 4 lowercase hex chars */
			for (uint8_t i = 0U; i < 4U; i++) {
					char c = s[i];
					if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0U;
			}
			return 1U;
	}

	static uint8_t IsDigits6(const char *s) {
			for (uint8_t i = 0U; i < 6U; i++) { if (s[i] < '0' || s[i] > '9') return 0U; }
			return 1U;
	}

	static uint8_t IdMatches(const char *s) {
			for (uint8_t i = 0U; i < 4U; i++) { if (s[i] != txId[i]) return 0U; }
			return 1U;
	}

	static void ClearOtp(void) {
			for (uint8_t i = 0U; i < 7U; i++) generatedOTP[i] = 0;
	}

	static void EnterState(SystemState_t next) {
			currentState = next;
			timeoutCounter = sysTickCounter;
			countdownLastSecond = 0xFFFFFFFFU;
	}

	/* Shows the start screen and returns to the first state of the active mode */
	static void ReturnToStart(void) {
			LCD_Command(0x01U);
			LCD_Print(START_TEXT);
	#if WEB_MODE
			EnterState(STATE_IDLE);
	#else
			EnterState(STATE_SCAN_RFID);
	#endif
	}

	#if WEB_MODE
	/* End the current website attempt: tell the PC, clear the OTP, lock, go idle */
	static void FailAuth(const char *reason, const char *lcdMsg) {
			PC_SendMsg("RES:FAIL", reason);
			ClearOtp();
			LEDs_SetLocked();
			LCD_Command(0x01U);
			LCD_Print(lcdMsg);
			LCD_SetCursor(1U, 0U);
			LCD_Print("LOGIN FAILED");
			delay(500000U);
			ReturnToStart();
	}
	#endif

	/* Handles one complete command line received from the website backend */
	static void HandlePcCommand(const char *cmd) {
			uint8_t len = StrLen(cmd);

			if (len == 4U && StartsWith(cmd, "PING")) {
					PC_SendString("PONG\r\n");
					return;
			}

	#if WEB_MODE
			/* AUTH_START:abcd */
			if (len == 15U && StartsWith(cmd, "AUTH_START:") && IsHex4(cmd + 11)) {
					if (currentState != STATE_IDLE) { PC_SendString("NAK:BUSY\r\n"); return; }
					for (uint8_t i = 0U; i < 4U; i++) txId[i] = cmd[11U + i];
					txId[4] = '\0';
					badCards = 0U;
					otpAttempts = 0U;
					ClearOtp();
					EnterState(STATE_SCAN_RFID);
					LCD_ShowCountdown("SCAN RFID CARD", SCAN_TIMEOUT_MS);
					PC_SendMsg("ACK:AUTH_START", 0);
					return;
			}

			/* AUTH_VERIFY:abcd:123456  (exactly 23 characters) */
			if (len == 23U && StartsWith(cmd, "AUTH_VERIFY:") && IsHex4(cmd + 12)
					&& cmd[16] == ':' && IsDigits6(cmd + 17)) {
					if (currentState != STATE_WAIT_PC_OTP || !IdMatches(cmd + 12)) {
							PC_SendString("NAK:NO_ACTIVE\r\n");
							return;
					}
					uint8_t diff = 0U;
					for (uint8_t i = 0U; i < 6U; i++) diff |= (uint8_t)(cmd[17U + i] ^ generatedOTP[i]);

					if (diff == 0U) {
							ClearOtp();                                  /* one-time use */
							LEDs_SetGranted();
							LCD_Command(0x01U);
							LCD_Print("ACCESS GRANTED");
							delay(500000U);
							LCD_Command(0x01U);
							LCD_Print("ADMIN LOGGED IN");
							BT_SendString("SUCCESS: SYSTEM UNLOCKED\r\n");
							PC_SendMsg("RES:OK", 0);
							EnterState(STATE_GRANTED);
					} else {
							otpAttempts++;
							BT_SendString("ERROR: INCORRECT OTP\r\n");
							if (otpAttempts >= MAX_OTP_TRIES) {
									FailAuth("TOO_MANY", "TOO MANY TRIES");
							} else {
									char remaining[2];
									remaining[0] = (char)('0' + (MAX_OTP_TRIES - otpAttempts));
									remaining[1] = '\0';
									PC_SendMsg("EVT:BAD_OTP", remaining);
									LCD_Command(0x01U);
									LCD_Print("WRONG OTP");
									delay(500000U);
										LCD_Command(0x01U);
										LCD_Print("OTP SENT TO APP");
										timeoutCounter = sysTickCounter;
										countdownLastSecond = 0xFFFFFFFFU;
										LCD_ShowCountdown("OTP SENT TO APP", OTP_TIMEOUT_MS);
							}
					}
					return;
			}

			/* AUTH_CANCEL:abcd */
			if (len == 16U && StartsWith(cmd, "AUTH_CANCEL:") && IsHex4(cmd + 12)) {
					if ((currentState == STATE_SCAN_RFID || currentState == STATE_WAIT_BUTTON
							 || currentState == STATE_WAIT_PC_OTP) && IdMatches(cmd + 12)) {
							FailAuth("CANCELLED", "CANCELLED");
					} else {
							PC_SendString("NAK:NO_ACTIVE\r\n");
					}
					return;
			}

			/* DEV_LOCK : website logout clears every authentication state */
			if (len == 8U && StartsWith(cmd, "DEV_LOCK")) {
					ClearOtp();
					txId[0] = '\0';
					badCards = 0U;
					otpAttempts = 0U;
					LEDs_SetLocked();
					LCD_Command(0x01U);
					LCD_Print("RESETTING...");
					delay(200000U);
					ReturnToStart();
					PC_SendString("ACK:DEV_LOCK\r\n");
					return;
			}

			PC_SendString("NAK:BAD_CMD\r\n");
	#else
			PC_SendString("NAK:STANDALONE\r\n");
	#endif
	}

	/* ---------------- MAIN PROGRAM ---------------- */
	int main(void) {
			uint8_t atqa[2], uid[10], uidLength = 0U;
			char receivedCmd[BT_RX_BUFFER_SIZE];
			char pcCmd[PC_LINE_MAX];

	#if WEB_MODE
			currentState = STATE_IDLE;
	#else
			currentState = STATE_SCAN_RFID;
	#endif

			/* Initialize Clocks & SysTick for 1ms interrupts (Assuming 16MHz HSI) */
			RCC->AHB1ENR |= (1U << 0U) | (1U << 1U);
			SysTick_Config(16000000 / 1000); 

			/* GPIO Setup */
			GPIOA->MODER &= ~(3U << 0U); GPIOA->PUPDR |= (1U << 0U); // Button PA0
			GPIOB->MODER &= ~((3U << 24U) | (3U << 26U)); GPIOB->MODER |= ((1U << 24U) | (1U << 26U)); // LEDs
			
			LEDs_SetLocked();
			I2C1_Init();
			LCD_Init();
			SPI1_Init();
			RFID_Init();
			USART2_Init();
			USART1_Init();                                       /* NEW */

			LCD_Command(0x01U);
			LCD_SetCursor(0U, 0U);
			LCD_Print(START_TEXT);
			BT_SendString("SYSTEM READY\r\n");
			PC_SendString("READY\r\n");                          /* NEW */

			while (1) {
					/* NEW: process any command line from the website backend */
					if (PC_GetCommand(pcCmd)) {
							HandlePcCommand(pcCmd);
					}

					switch(currentState) {

							case STATE_IDLE:
									/* Web mode only: wait for AUTH_START */
									break;
							
							case STATE_SCAN_RFID:
	#if WEB_MODE
									LCD_ShowCountdown("SCAN RFID CARD", SCAN_TIMEOUT_MS);
									if ((sysTickCounter - timeoutCounter) > SCAN_TIMEOUT_MS) {
											FailAuth("TIMEOUT_CARD", "TIMEOUT!");
											break;
									}
	#endif
									if (RFID_Request(PICC_REQA, atqa) || RFID_Request(PICC_WUPA, atqa)) {
											if (RFID_ReadUID(uid, &uidLength)) {
													if (UID_IsAuthorized(uid, uidLength)) {
									WEB_EVT("EVT:CARD_OK", 0);
									EnterState(STATE_WAIT_BUTTON);
									LCD_ShowCountdown("PRESS BUTTON", BUTTON_TIMEOUT_MS);
													} else {
															LCD_Command(0x01U);
															LCD_Print("ACCESS DENIED");
															WEB_EVT("EVT:CARD_BAD", 0);
															delay(500000U);
	#if WEB_MODE
															badCards++;
															if (badCards >= MAX_BAD_CARDS) {
																	FailAuth("CARD_REJECTED", "ACCESS DENIED");
																	break;
															}
	#endif
															LCD_Command(0x01U);
															LCD_Print("SCAN RFID CARD");
													}
													delay(200000U); // Prevent rapid re-triggering
											}
									}
									break;

							case STATE_WAIT_BUTTON:
	#if WEB_MODE
									LCD_ShowCountdown("PRESS BUTTON", BUTTON_TIMEOUT_MS);
	#endif
									// Check if PA0 is pressed (Active Low)
									if ((GPIOA->IDR & (1U << BUTTON_PIN)) == 0U) {
											delay(20000U); // Debounce
											if ((GPIOA->IDR & (1U << BUTTON_PIN)) == 0U) {
													Generate_OTP(generatedOTP);
													
									BT_SendString("ENTER THIS OTP TO UNLOCK:\r\n");
													BT_SendString(generatedOTP);
													BT_SendString("\r\n");
													
													btCommandReady = 0; // Clear old serial data
													WEB_EVT("EVT:OTP_SENT", 0);
									EnterState(STATE_WAIT_PC_OTP);
									LCD_ShowCountdown("OTP SENT TO APP", OTP_TIMEOUT_MS);
													break;
											}
									}
									
					// 30-second timeout to press the button
									if ((sysTickCounter - timeoutCounter) > BUTTON_TIMEOUT_MS) {
	#if WEB_MODE
											FailAuth("TIMEOUT_BUTTON", "TIMEOUT!");
	#else
											LCD_Command(0x01U); LCD_Print("TIMEOUT!");
											delay(500000U);
											LCD_Command(0x01U); LCD_Print("SCAN RFID CARD");
											currentState = STATE_SCAN_RFID;
	#endif
									}
									break;

							case STATE_WAIT_PC_OTP:
	#if WEB_MODE
									LCD_ShowCountdown("OTP SENT TO APP", OTP_TIMEOUT_MS);
	#endif
	#if !WEB_MODE
									/* Standalone mode: OTP typed on the phone (Bluetooth), as in your original code */
									if (BT_GetCommand(receivedCmd)) {
											uint8_t match = 1;
											for (int i=0; i<6; i++) {
													if (receivedCmd[i] != generatedOTP[i]) match = 0;
											}
											
											if (match) {
													LEDs_SetGranted();
													LCD_Command(0x01U);
													LCD_Print("ACCESS GRANTED");
													BT_SendString("SUCCESS: SYSTEM UNLOCKED\r\n");
													currentState = STATE_GRANTED;
											} else {
													LCD_Command(0x01U);
													LCD_Print("WRONG OTP");
													BT_SendString("ERROR: INCORRECT OTP\r\n");
													delay(500000U);
													LCD_Command(0x01U); LCD_Print("SCAN RFID CARD");
													currentState = STATE_SCAN_RFID;
											}
									}
	#endif
									/* Web mode: the OTP arrives over USART1 and is checked in HandlePcCommand() */
									
									// 30-second timeout to enter the OTP
									if (currentState == STATE_WAIT_PC_OTP
											&& (sysTickCounter - timeoutCounter) > OTP_TIMEOUT_MS) {
											BT_SendString("ERROR: OTP EXPIRED\r\n");
	#if WEB_MODE
											FailAuth("EXPIRED", "OTP EXPIRED");
	#else
											LCD_Command(0x01U); LCD_Print("OTP EXPIRED");
											delay(500000U);
											LCD_Command(0x01U); LCD_Print("SCAN RFID CARD");
											currentState = STATE_SCAN_RFID;
	#endif
									}
									break;

							case STATE_GRANTED:
									// Stay unlocked until button is pressed again to lock it
									if ((GPIOA->IDR & (1U << BUTTON_PIN)) == 0U) {
											delay(20000U); // Debounce
											LEDs_SetLocked();
											LCD_Command(0x01U); LCD_Print("SYSTEM LOCKED");
											BT_SendString("SYSTEM LOCKED MANUALLY\r\n");
											PC_SendString("EVT:LOCKED\r\n");
											delay(500000U);
											ReturnToStart();
									}
									break;
					}
			}
	}
