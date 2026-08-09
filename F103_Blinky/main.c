#include <stdint.h>

#define REG32(address) (*(volatile uint32_t *)(address))

#define RCC_APB2ENR REG32(0x40021018UL)

#define GPIOB_CRH  REG32(0x40010C04UL)
#define GPIOB_BSRR REG32(0x40010C10UL)
#define GPIOB_BRR  REG32(0x40010C14UL)

#define SYSTICK_CTRL REG32(0xE000E010UL)
#define SYSTICK_LOAD REG32(0xE000E014UL)
#define SYSTICK_VAL  REG32(0xE000E018UL)

#define RCC_APB2ENR_IOPBEN (1UL << 3)
#define LED_PIN             (1UL << 13)
#define SYSTICK_COUNTFLAG   (1UL << 16)
#define SYSTICK_ENABLE      (1UL << 0)
#define SYSTICK_CLKSOURCE   (1UL << 2)

/* The STM32F103 starts from the 8 MHz internal HSI clock after reset. */
#define HALF_PERIOD_TICKS (4000000UL)

static void wait_half_second(void)
{
  while ((SYSTICK_CTRL & SYSTICK_COUNTFLAG) == 0UL)
  {
  }
}

int main(void)
{
  RCC_APB2ENR |= RCC_APB2ENR_IOPBEN;

  /* PB13: general-purpose push-pull output, maximum output speed 2 MHz. */
  GPIOB_CRH = (GPIOB_CRH & ~(0xFUL << 20)) | (0x2UL << 20);
  GPIOB_BRR = LED_PIN;

  SYSTICK_LOAD = HALF_PERIOD_TICKS - 1UL;
  SYSTICK_VAL = 0UL;
  SYSTICK_CTRL = SYSTICK_CLKSOURCE | SYSTICK_ENABLE;

  for (;;)
  {
    GPIOB_BSRR = LED_PIN;
    wait_half_second();

    GPIOB_BRR = LED_PIN;
    wait_half_second();
  }
}
