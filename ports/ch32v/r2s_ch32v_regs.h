// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* CH32V003 register map: the addresses and bits the port glue touches.
 * Facts follow the WCH reference manual; the open ch32v003fun headers
 * cross-checked the map on 2026-11-10. Only the used registers appear. */
#ifndef R2S_CH32V_REGS_H
#define R2S_CH32V_REGS_H

#include <stdint.h>

/* Peripheral bases: APB2 and AHB hang off the peripheral block, the
 * SysTick off the core block, flash and SRAM at their alias addresses. */
#define R2S_CH32V_APB2_BASE 0x40010000u
#define R2S_CH32V_AHB_BASE 0x40020000u
#define R2S_CH32V_CORE_BASE 0xE0000000u
#define R2S_CH32V_FLASH_BASE 0x08000000u
#define R2S_CH32V_SRAM_BASE 0x20000000u
#define R2S_CH32V_SRAM_BYTES 0x800u

/* The 48 MHz the port init switches on: 24 MHz HSI times the fixed x2 PLL. */
#define R2S_CH32V_HCLK_HZ 48000000u
#define R2S_CH32V_HSI_HZ 24000000u

/* Reset and clock control: CTLR, CFGR0, INTR, reset registers, then the
 * AHB enable, the APB2 enable the port touches, APB1 enable, and status. */
typedef struct
{
  volatile uint32_t CTLR;
  volatile uint32_t CFGR0;
  volatile uint32_t INTR;
  volatile uint32_t APB2PRSTR;
  volatile uint32_t APB1PRSTR;
  volatile uint32_t AHBPCENR;
  volatile uint32_t APB2PCENR;
  volatile uint32_t APB1PCENR;
  volatile uint32_t RESERVED0;
  volatile uint32_t RSTSCKR;
} r2s_ch32v_rcc_t;

/* General-purpose I/O: configuration low and high, input, output, set,
 * reset, and lock. PD5 and PD6 live in the low register. */
typedef struct
{
  volatile uint32_t CFGLR;
  volatile uint32_t CFGHR;
  volatile uint32_t INDR;
  volatile uint32_t OUTDR;
  volatile uint32_t BSHR;
  volatile uint32_t BCR;
  volatile uint32_t LCKR;
} r2s_ch32v_gpio_t;

/* USART: status, data, baud, and three control registers. */
typedef struct
{
  volatile uint32_t STATR;
  volatile uint32_t DATAR;
  volatile uint32_t BRR;
  volatile uint32_t CTLR1;
  volatile uint32_t CTLR2;
  volatile uint32_t CTLR3;
  volatile uint32_t GPR;
} r2s_ch32v_usart_t;

/* SysTick: control, status, a free-running 32-bit count, and compare. */
typedef struct
{
  volatile uint32_t CTLR;
  volatile uint32_t SR;
  volatile uint32_t CNT;
  volatile uint32_t RESERVED0;
  volatile uint32_t CMP;
  volatile uint32_t RESERVED1;
} r2s_ch32v_stk_t;

/* Clock control: trim default, HSI on, PLL on and ready. */
#define R2S_CH32V_RCC_CTLR_HSITRIM (0x10u << 3u)
#define R2S_CH32V_RCC_CTLR_HSION 0x00000001u
#define R2S_CH32V_RCC_CTLR_PLLON 0x01000000u
#define R2S_CH32V_RCC_CTLR_PLLRDY 0x02000000u
/* Configuration: AHB undivided, HSI times two, then PLL as the source. */
#define R2S_CH32V_RCC_CFGR0_HPRE_DIV1 0x00000000u
#define R2S_CH32V_RCC_CFGR0_PLLSRC_HSI_MUL2 0x00000000u
#define R2S_CH32V_RCC_CFGR0_SW_PLL 0x00000002u
#define R2S_CH32V_RCC_CFGR0_SWS 0x0000000Cu
#define R2S_CH32V_RCC_CFGR0_SWS_PLL 0x00000008u
/* Interrupt register: writing ones clears the ready flags. */
#define R2S_CH32V_RCC_INTR_CLEAR 0x009F0000u
/* APB2 enables: GPIOD for the pins, USART1 for the port. */
#define R2S_CH32V_RCC_GPIODEN 0x00000020u
#define R2S_CH32V_RCC_USART1EN 0x00004000u

/* USART1 lives on PD5 (TX) and PD6 (RX): the default pin mapping, so no
 * alternate-function remap is needed. */
#define R2S_CH32V_TX_PIN 5u
#define R2S_CH32V_RX_PIN 6u
/* TX is alternate push-pull at 10 MHz, RX floats. */
#define R2S_CH32V_GPIO_TX_AF_PP 0x9u
#define R2S_CH32V_GPIO_RX_FLOAT 0x4u
/* USART bits: transmit empty, receive not empty, receiver, transmitter on. */
#define R2S_CH32V_USART_TXE 0x00000080u
#define R2S_CH32V_USART_RXNE 0x00000020u
#define R2S_CH32V_USART_RE 0x00000004u
#define R2S_CH32V_USART_TE 0x00000008u
#define R2S_CH32V_USART_UE 0x00002000u

/* SysTick enable: counting at HCLK/8 with no interrupt, which the polled
 * tick reads directly. */
#define R2S_CH32V_STK_STE 0x00000001u
/* Counts per millisecond at 48 MHz: 48 MHz divided by 8, per 1000. */
#define R2S_CH32V_CNT_PER_MS 6000u

#ifdef R2S_CH32V_HOST_TEST
/* Host tests own the peripherals: the fakes below stand in for settled
 * hardware, and the reset presets the PLL ready flags so the init waits
 * pass without a clock tree. */
extern r2s_ch32v_rcc_t R2S_CH32V_TEST_rcc;
extern r2s_ch32v_gpio_t R2S_CH32V_TEST_gpiod;
extern r2s_ch32v_usart_t R2S_CH32V_TEST_usart1;
extern r2s_ch32v_stk_t R2S_CH32V_TEST_stk;
extern uint8_t R2S_CH32V_TEST_tx[256];
extern uint16_t R2S_CH32V_TEST_tx_count;

#define R2S_CH32V_RCC (&R2S_CH32V_TEST_rcc)
#define R2S_CH32V_GPIOD (&R2S_CH32V_TEST_gpiod)
#define R2S_CH32V_USART1 (&R2S_CH32V_TEST_usart1)
#define R2S_CH32V_STK (&R2S_CH32V_TEST_stk)

/* Zeroes the fakes and presets the settled-clock flags. */
void r2s_ch32v_test_regs_reset(void);
#else
#define R2S_CH32V_RCC ((r2s_ch32v_rcc_t *)(R2S_CH32V_AHB_BASE + 0x1000u))
#define R2S_CH32V_GPIOD ((r2s_ch32v_gpio_t *)(R2S_CH32V_APB2_BASE + 0x1400u))
#define R2S_CH32V_USART1 ((r2s_ch32v_usart_t *)(R2S_CH32V_APB2_BASE + 0x3800u))
#define R2S_CH32V_STK ((r2s_ch32v_stk_t *)(R2S_CH32V_CORE_BASE + 0xF000u))
#endif

/// Divisor for a baud rate from the given clock.
/**
 * Truncates like the AVR divisor, so the USART runs at most the asked
 * rate; the bridge tolerates the fraction left over.
 *
 * \param[in] hclk_hz the USART clock in hertz.
 * \param[in] baud the asked baud rate, nonzero by contract.
 * \return hclk_hz divided by baud, or 0 when baud is 0.
 */
static inline uint32_t r2s_ch32v_brr(uint32_t hclk_hz, uint32_t baud)
{
  if (baud == 0u) {
    return 0u;
  }
  return hclk_hz / baud;
}

/// Milliseconds from a raw SysTick count.
/**
 * The caller subtracts two counts first, so the 32-bit wraparound lands
 * in the difference the division then converts.
 *
 * \param[in] cnt the SysTick count difference to convert.
 * \param[in] per_ms counts per millisecond at the running clock.
 * \return Whole milliseconds in cnt.
 */
static inline uint32_t r2s_ch32v_ms(uint32_t cnt, uint32_t per_ms)
{
  return cnt / per_ms;
}

/* Sends one byte: the DATAR store on target, the capture buffer on host.
 * The host form lets the contract test read back the order bytes left in. */
#ifdef R2S_CH32V_HOST_TEST
static inline void r2s_ch32v_usart_send(r2s_ch32v_usart_t *usart, uint8_t byte)
{
  usart->DATAR = (uint32_t)byte;
  if (R2S_CH32V_TEST_tx_count < sizeof(R2S_CH32V_TEST_tx)) {
    R2S_CH32V_TEST_tx[R2S_CH32V_TEST_tx_count] = byte;
    R2S_CH32V_TEST_tx_count++;
  }
}
#else
static inline void r2s_ch32v_usart_send(r2s_ch32v_usart_t *usart, uint8_t byte)
{
  usart->DATAR = (uint32_t)byte;
}
#endif

/* Reads one byte: the DATAR load on both builds, which the test primes.
 * The host form also clears RXNE, the way reading DATAR does in hardware,
 * so a primed byte reads once and the port then reports empty. */
static inline uint8_t r2s_ch32v_usart_recv(r2s_ch32v_usart_t *usart)
{
  uint8_t byte = (uint8_t)usart->DATAR;
#ifdef R2S_CH32V_HOST_TEST
  usart->STATR &= (uint32_t)(~R2S_CH32V_USART_RXNE);
#endif
  return byte;
}

#endif
