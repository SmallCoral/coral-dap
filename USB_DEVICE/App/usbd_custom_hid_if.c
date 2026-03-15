/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_custom_hid_if.c
  * @brief          : Minimal CMSIS-DAP v1 (HID) transport for STM32F042
  *
  * NOTE:
  * - This is a bring-up implementation intended to get a CMSIS-DAP style probe
  *   talking over HID and bit-banging SWD on TMS/TCK.
  * - It is not the full Arm DAPLink firmware (no MSC drag-drop, no CDC UART).
  * - It should be treated as a practical starting point, not production firmware.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "usbd_custom_hid_if.h"
#include "main.h"
#include "string.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ----------------------- CMSIS-DAP command IDs ----------------------- */
#define ID_DAP_Info                 0x00U
#define ID_DAP_HostStatus           0x01U
#define ID_DAP_Connect              0x02U
#define ID_DAP_Disconnect           0x03U
#define ID_DAP_TransferConfigure    0x04U
#define ID_DAP_Transfer             0x05U
#define ID_DAP_TransferBlock        0x06U
#define ID_DAP_TransferAbort        0x07U
#define ID_DAP_WriteABORT           0x08U
#define ID_DAP_Delay                0x09U
#define ID_DAP_ResetTarget          0x0AU
#define ID_DAP_SWJ_Pins             0x10U
#define ID_DAP_SWJ_Clock            0x11U
#define ID_DAP_SWJ_Sequence         0x12U
#define ID_DAP_SWD_Configure        0x13U
#define ID_DAP_JTAG_Sequence        0x14U
#define ID_DAP_JTAG_Configure       0x15U
#define ID_DAP_JTAG_IDCODE          0x16U
#define ID_DAP_QueueCommands        0x7EU
#define ID_DAP_ExecuteCommands      0x7FU

/* ----------------------- DAP Info IDs ----------------------- */
#define DAP_INFO_VENDOR             0x01U
#define DAP_INFO_PRODUCT            0x02U
#define DAP_INFO_SER_NUM            0x03U
#define DAP_INFO_FW_VER             0x04U
#define DAP_INFO_TARGET_VENDOR      0x05U
#define DAP_INFO_TARGET_NAME        0x06U
#define DAP_INFO_CAPABILITIES       0xF0U
#define DAP_INFO_PACKET_COUNT       0xFEU
#define DAP_INFO_PACKET_SIZE        0xFFU

/* ----------------------- Generic status ----------------------- */
#define DAP_OK                      0x00U
#define DAP_ERROR                   0xFFU

/* ----------------------- Connect return ----------------------- */
#define DAP_PORT_DISABLED           0x00U
#define DAP_PORT_SWD                0x01U
#define DAP_PORT_JTAG               0x02U

/* ----------------------- Transfer request bits ----------------------- */
#define DAP_TRANSFER_APnDP          (1U << 0)
#define DAP_TRANSFER_RnW            (1U << 1)
#define DAP_TRANSFER_A2             (1U << 2)
#define DAP_TRANSFER_A3             (1U << 3)

/* ----------------------- Transfer response ACK ----------------------- */
#define DAP_TRANSFER_OK             0x01U
#define DAP_TRANSFER_WAIT           0x02U
#define DAP_TRANSFER_FAULT          0x04U
#define DAP_TRANSFER_ERROR          0x08U

/* ----------------------- SWJ pin mapping ----------------------- */
#define DAP_SWJ_SWCLK_TCK           0x01U
#define DAP_SWJ_SWDIO_TMS           0x02U
#define DAP_SWJ_TDI                 0x04U
#define DAP_SWJ_TDO                 0x08U
#define DAP_SWJ_nTRST               0x20U
#define DAP_SWJ_nRESET              0x80U

/* ----------------------- HID report descriptor ----------------------- */
/* 64-byte IN / 64-byte OUT vendor HID */
__ALIGN_BEGIN static uint8_t CUSTOM_HID_ReportDesc_FS[USBD_CUSTOM_HID_REPORT_DESC_SIZE] __ALIGN_END =
{
  0x06, 0x00, 0xFF,  /* Usage Page (Vendor Defined 0xFF00) */
  0x09, 0x01,        /* Usage 1 */
  0xA1, 0x01,        /* Collection (Application) */
  0x15, 0x00,        /* Logical Minimum (0) */
  0x26, 0xFF, 0x00,  /* Logical Maximum (255) */
  0x75, 0x08,        /* Report Size (8) */
  0x95, 0x40,        /* Report Count (64) */
  0x09, 0x01,        /* Usage 1 */
  0x81, 0x02,        /* Input (Data,Var,Abs) */
  0x95, 0x40,        /* Report Count (64) */
  0x09, 0x01,        /* Usage 1 */
  0x91, 0x02,        /* Output (Data,Var,Abs) */
  0xC0               /* End Collection */
};

extern USBD_HandleTypeDef hUsbDeviceFS;

static int8_t CUSTOM_HID_Init_FS(void);
static int8_t CUSTOM_HID_DeInit_FS(void);
static int8_t CUSTOM_HID_OutEvent_FS(uint8_t event_idx, uint8_t state);

static void target_nrst_assert(void);
static void target_nrst_release(void);


USBD_CUSTOM_HID_ItfTypeDef USBD_CustomHID_fops_FS =
{
  CUSTOM_HID_ReportDesc_FS,
  CUSTOM_HID_Init_FS,
  CUSTOM_HID_DeInit_FS,
  CUSTOM_HID_OutEvent_FS
};

/* ----------------------- Runtime state ----------------------- */
static uint8_t g_tx_report[64];
static uint8_t g_connected_port = DAP_PORT_DISABLED;
static uint8_t g_idle_cycles = 0U;
static uint16_t g_wait_retry = 128U;
static uint16_t g_match_retry = 0U;
static uint8_t g_swd_turnaround = 1U;
static uint8_t g_swd_data_phase = 0U;
static uint32_t g_swj_clock_hz = 100000U;
static uint32_t g_delay_loops = 120U;

/* ----------------------- Utility ----------------------- */
static uint16_t rd_u16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint8_t parity32(uint32_t v)
{
  uint8_t p = 0U;
  while (v != 0U)
  {
    p ^= (uint8_t)(v & 1U);
    v >>= 1;
  }
  return p & 1U;
}

static void delay_cycles_soft(void)
{
  volatile uint32_t i;
  for (i = 0U; i < g_delay_loops; ++i)
  {
    __NOP();
  }
}

static void cmsis_dap_send(const uint8_t *buf, uint16_t len)
{
  uint16_t send_len = (len > 64U) ? 64U : len;
  uint16_t i;

  memset(g_tx_report, 0, sizeof(g_tx_report));
  for (i = 0U; i < send_len; ++i)
  {
    g_tx_report[i] = buf[i];
  }

  (void)USBD_CUSTOM_HID_SendReport(&hUsbDeviceFS, g_tx_report, sizeof(g_tx_report));
}

/* ----------------------- Pin helpers ----------------------- */
static void swdio_mode_out(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Pin = TMS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(TMS_GPIO_Port, &GPIO_InitStruct);
}

static void swdio_mode_in(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Pin = TMS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TMS_GPIO_Port, &GPIO_InitStruct);
}

static void swclk_write(uint8_t v)
{
  HAL_GPIO_WritePin(TCK_GPIO_Port, TCK_Pin, v ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void swdio_write(uint8_t v)
{
  HAL_GPIO_WritePin(TMS_GPIO_Port, TMS_Pin, v ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t swdio_read(void)
{
  return (HAL_GPIO_ReadPin(TMS_GPIO_Port, TMS_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

static void tdi_write(uint8_t v)
{
  HAL_GPIO_WritePin(TDI_GPIO_Port, TDI_Pin, v ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t tdo_read(void)
{
  return (HAL_GPIO_ReadPin(TDO_GPIO_Port, TDO_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

static void swclk_cycle(void)
{
  delay_cycles_soft();
  swclk_write(1U);
  delay_cycles_soft();
  swclk_write(0U);
}

static void swd_write_bit(uint8_t bit)
{
  swdio_write(bit);
  swclk_cycle();
}

static uint8_t swd_read_bit(void)
{
  uint8_t bit;
  delay_cycles_soft();
  swclk_write(1U);
  delay_cycles_soft();
  bit = swdio_read();
  swclk_write(0U);
  delay_cycles_soft();
  return bit;
}

static void swd_write_turnaround(uint8_t cycles)
{
  uint8_t i;
  for (i = 0U; i < cycles; ++i)
  {
    swclk_cycle();
  }
}

static void swd_line_idle(void)
{
  swdio_mode_out();
  swdio_write(1U);
  swclk_write(0U);
}

static void swd_line_reset(void)
{
  uint8_t i;
  swdio_mode_out();
  swdio_write(1U);
  for (i = 0U; i < 60U; ++i)
  {
    swclk_cycle();
  }
}

static void swj_sequence_bits(uint16_t count, const uint8_t *data)
{
  uint16_t bit_index;
  swdio_mode_out();

  for (bit_index = 0U; bit_index < count; ++bit_index)
  {
    uint8_t byte_val = data[bit_index >> 3];
    uint8_t bit = (byte_val >> (bit_index & 7U)) & 1U;
    swd_write_bit(bit);
  }

  swd_line_idle();
}

/* ----------------------- Raw SWD transfer ----------------------- */
static uint8_t swd_transfer_word(uint8_t request, uint32_t *data)
{
  uint8_t ack = 0U;
  uint8_t parity;
  uint8_t bit;
  uint32_t value = 0U;
  uint8_t i;

  /* Request phase: start(1), APnDP, RnW, A2, A3, parity, stop(0), park(1) */
  swdio_mode_out();
  swd_write_bit(1U);
  swd_write_bit((request & DAP_TRANSFER_APnDP) ? 1U : 0U);
  swd_write_bit((request & DAP_TRANSFER_RnW) ? 1U : 0U);
  swd_write_bit((request & DAP_TRANSFER_A2) ? 1U : 0U);
  swd_write_bit((request & DAP_TRANSFER_A3) ? 1U : 0U);

  parity = 0U;
  parity ^= (request & DAP_TRANSFER_APnDP) ? 1U : 0U;
  parity ^= (request & DAP_TRANSFER_RnW) ? 1U : 0U;
  parity ^= (request & DAP_TRANSFER_A2) ? 1U : 0U;
  parity ^= (request & DAP_TRANSFER_A3) ? 1U : 0U;

  swd_write_bit(parity);
  swd_write_bit(0U);
  swd_write_bit(1U);

  /* Turnaround to target */
  swdio_mode_in();
  swd_write_turnaround(g_swd_turnaround);

  ack  = swd_read_bit();
  ack |= (uint8_t)(swd_read_bit() << 1);
  ack |= (uint8_t)(swd_read_bit() << 2);

  if (ack == DAP_TRANSFER_OK)
  {
    if ((request & DAP_TRANSFER_RnW) != 0U)
    {
      for (i = 0U; i < 32U; ++i)
      {
        bit = swd_read_bit();
        value |= ((uint32_t)bit << i);
      }

      parity = swd_read_bit();

      /* Turnaround back to host */
      swdio_mode_out();
      swd_write_turnaround(g_swd_turnaround);

      if ((parity32(value) ^ parity) != 0U)
      {
        swd_line_idle();
        return DAP_TRANSFER_ERROR;
      }

      *data = value;
    }
    else
    {
      /* Turnaround back to host before write data */
      swdio_mode_out();
      swd_write_turnaround(g_swd_turnaround);

      value = *data;
      for (i = 0U; i < 32U; ++i)
      {
        swd_write_bit((uint8_t)((value >> i) & 1U));
      }
      swd_write_bit(parity32(value));
    }

    /* Idle cycles */
    swdio_write(1U);
    for (i = 0U; i < g_idle_cycles; ++i)
    {
      swclk_cycle();
    }

    swd_line_idle();
    return DAP_TRANSFER_OK;
  }

  /* Recovery on WAIT/FAULT/protocol error */
  swdio_mode_out();
  swdio_write(1U);
  swclk_cycle();
  swd_line_idle();

  if ((ack != DAP_TRANSFER_WAIT) && (ack != DAP_TRANSFER_FAULT))
  {
    return DAP_TRANSFER_ERROR;
  }

  return ack;
}

/* ----------------------- CMSIS-DAP command handlers ----------------------- */
static uint8_t dap_handle_info(const uint8_t *req, uint8_t *resp)
{
  static const char vendor[] = "SmallCoral";
  static const char product[] = "DAPLink";
  static const char serial[] = "STM32F042";
  static const char fwver[] = "0.2";
  uint8_t n = 2U;

  resp[0] = ID_DAP_Info;
  resp[1] = 0U;

  switch (req[1])
  {
    case DAP_INFO_VENDOR:
      resp[1] = (uint8_t)sizeof(vendor);
      memcpy(&resp[2], vendor, sizeof(vendor));
      n = (uint8_t)(2U + sizeof(vendor));
      break;

    case DAP_INFO_PRODUCT:
      resp[1] = (uint8_t)sizeof(product);
      memcpy(&resp[2], product, sizeof(product));
      n = (uint8_t)(2U + sizeof(product));
      break;

    case DAP_INFO_SER_NUM:
      resp[1] = (uint8_t)sizeof(serial);
      memcpy(&resp[2], serial, sizeof(serial));
      n = (uint8_t)(2U + sizeof(serial));
      break;

    case DAP_INFO_FW_VER:
      resp[1] = (uint8_t)sizeof(fwver);
      memcpy(&resp[2], fwver, sizeof(fwver));
      n = (uint8_t)(2U + sizeof(fwver));
      break;

    case DAP_INFO_CAPABILITIES:
      resp[1] = 1U;
      resp[2] = 0x01U; /* SWD only */
      n = 3U;
      break;

    case DAP_INFO_PACKET_COUNT:
      resp[1] = 1U;
      resp[2] = 1U;
      n = 3U;
      break;

    case DAP_INFO_PACKET_SIZE:
      resp[1] = 2U;
      wr_u16(&resp[2], 64U);
      n = 4U;
      break;

    default:
      /* Unsupported info item => length 0 */
      n = 2U;
      break;
  }

  return n;
}

static uint8_t dap_handle_host_status(const uint8_t *req, uint8_t *resp)
{
  (void)req;
  resp[0] = ID_DAP_HostStatus;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_connect(const uint8_t *req, uint8_t *resp)
{
  uint8_t port = req[1];

  if ((port == 0U) || (port == DAP_PORT_SWD))
  {
    g_connected_port = DAP_PORT_SWD;
    swclk_write(0U);
    swd_line_idle();
    swd_line_reset();
  }
  else
  {
    g_connected_port = DAP_PORT_DISABLED;
  }

  resp[0] = ID_DAP_Connect;
  resp[1] = g_connected_port;
  return 2U;
}

static uint8_t dap_handle_disconnect(const uint8_t *req, uint8_t *resp)
{
  (void)req;
  g_connected_port = DAP_PORT_DISABLED;
  swd_line_idle();
  resp[0] = ID_DAP_Disconnect;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_transfer_configure(const uint8_t *req, uint8_t *resp)
{
  g_idle_cycles = req[1];
  g_wait_retry = rd_u16(&req[2]);
  g_match_retry = rd_u16(&req[4]);

  resp[0] = ID_DAP_TransferConfigure;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_swj_clock(const uint8_t *req, uint8_t *resp)
{
  g_swj_clock_hz = rd_u32(&req[1]);
  if (g_swj_clock_hz == 0U)
  {
    g_swj_clock_hz = 1000000U;
  }

  /* Crude software delay tuning for 48MHz core */
  g_delay_loops = (48000000UL / (g_swj_clock_hz * 4UL));
  if (g_delay_loops > 255UL)
  {
    g_delay_loops = 255UL;
  }

  resp[0] = ID_DAP_SWJ_Clock;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_swd_configure(const uint8_t *req, uint8_t *resp)
{
  g_swd_turnaround = (uint8_t)((req[1] & 0x03U) + 1U);
  g_swd_data_phase = (uint8_t)((req[1] >> 2) & 0x01U);

  (void)g_swd_data_phase; /* currently unused, but parsed */

  resp[0] = ID_DAP_SWD_Configure;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_swj_sequence(const uint8_t *req, uint8_t *resp)
{
  uint16_t bit_count = req[1];
  if (bit_count == 0U)
  {
    bit_count = 256U;
  }

  swj_sequence_bits(bit_count, &req[2]);

  resp[0] = ID_DAP_SWJ_Sequence;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_swj_pins(const uint8_t *req, uint8_t *resp)
{
  uint8_t value = req[1];
  uint8_t select = req[2];
  uint32_t wait_us = rd_u32(&req[3]);
  uint8_t pins = 0U;

  if ((select & DAP_SWJ_SWCLK_TCK) != 0U)
  {
    swclk_write((value & DAP_SWJ_SWCLK_TCK) ? 1U : 0U);
  }

  if ((select & DAP_SWJ_SWDIO_TMS) != 0U)
  {
    swdio_mode_out();
    swdio_write((value & DAP_SWJ_SWDIO_TMS) ? 1U : 0U);
  }

  if ((select & DAP_SWJ_TDI) != 0U)
  {
    tdi_write((value & DAP_SWJ_TDI) ? 1U : 0U);
  }

  if ((select & DAP_SWJ_nRESET) != 0U)
  {
    if ((value & DAP_SWJ_nRESET) != 0U)
    {
      target_nrst_release();   /* 1 = release */
    }
    else
    {
      target_nrst_assert();    /* 0 = drive low */
    }
  }

  if (wait_us != 0U)
  {
    HAL_Delay((wait_us + 999U) / 1000U);
  }

  if (HAL_GPIO_ReadPin(TCK_GPIO_Port, TCK_Pin) == GPIO_PIN_SET)
  {
    pins |= DAP_SWJ_SWCLK_TCK;
  }

  if (HAL_GPIO_ReadPin(TMS_GPIO_Port, TMS_Pin) == GPIO_PIN_SET)
  {
    pins |= DAP_SWJ_SWDIO_TMS;
  }

  if (HAL_GPIO_ReadPin(TDI_GPIO_Port, TDI_Pin) == GPIO_PIN_SET)
  {
    pins |= DAP_SWJ_TDI;
  }

  if (tdo_read() != 0U)
  {
    pins |= DAP_SWJ_TDO;
  }

  if (HAL_GPIO_ReadPin(NRST_GPIO_Port, NRST_Pin) == GPIO_PIN_SET)
  {
    pins |= DAP_SWJ_nRESET;
  }

  resp[0] = ID_DAP_SWJ_Pins;
  resp[1] = pins;
  return 2U;
}

static uint8_t dap_handle_delay(const uint8_t *req, uint8_t *resp)
{
  uint16_t delay_us = rd_u16(&req[1]);

  /* Approximation using ms granularity when >0 */
  if (delay_us != 0U)
  {
    HAL_Delay((delay_us + 999U) / 1000U);
  }

  resp[0] = ID_DAP_Delay;
  resp[1] = DAP_OK;
  return 2U;
}

static uint8_t dap_handle_reset_target(const uint8_t *req, uint8_t *resp)
{
  (void)req;

  target_nrst_assert();
  HAL_Delay(20);
  target_nrst_release();
  HAL_Delay(20);

  resp[0] = ID_DAP_ResetTarget;
  resp[1] = DAP_OK;
  resp[2] = 0U;
  return 3U;
}

static uint8_t dap_handle_write_abort(const uint8_t *req, uint8_t *resp)
{
  uint32_t value = rd_u32(&req[2]);
  uint8_t ack;

  ack = swd_transfer_word(0x00U /* DP write addr 0x0 */, &value);

  resp[0] = ID_DAP_WriteABORT;
  resp[1] = (ack == DAP_TRANSFER_OK) ? DAP_OK : DAP_ERROR;
  return 2U;
}

static uint8_t dap_handle_transfer(const uint8_t *req, uint8_t *resp)
{
  uint8_t transfer_count = req[2];
  uint8_t completed = 0U;
  uint8_t transfer_response = DAP_TRANSFER_OK;
  uint8_t req_off = 3U;
  uint8_t rsp_off = 3U;
  uint8_t i;

  resp[0] = ID_DAP_Transfer;
  resp[1] = 0U;
  resp[2] = DAP_TRANSFER_OK;

  for (i = 0U; i < transfer_count; ++i)
  {
    uint8_t request = req[req_off++];
    uint32_t data = 0U;
    uint16_t tries = 0U;
    uint8_t ack;

    if ((request & DAP_TRANSFER_RnW) == 0U)
    {
      data = rd_u32(&req[req_off]);
      req_off += 4U;
    }

    do
    {
      uint32_t tmp = data;
      ack = swd_transfer_word(request, &tmp);
      if ((request & DAP_TRANSFER_RnW) != 0U)
      {
        data = tmp;
      }
      if (ack == DAP_TRANSFER_WAIT)
      {
        ++tries;
      }
      else
      {
        break;
      }
    } while (tries < g_wait_retry);

    transfer_response = ack;

    if (ack != DAP_TRANSFER_OK)
    {
      break;
    }

    ++completed;

    if ((request & DAP_TRANSFER_RnW) != 0U)
    {
      wr_u32(&resp[rsp_off], data);
      rsp_off += 4U;
    }
  }

  resp[1] = completed;
  resp[2] = transfer_response;
  return rsp_off;
}

static uint8_t dap_handle_transfer_block(const uint8_t *req, uint8_t *resp)
{
  uint16_t count = rd_u16(&req[1]);
  uint8_t request = req[3];
  uint16_t completed = 0U;
  uint8_t transfer_response = DAP_TRANSFER_OK;
  uint16_t i;
  uint16_t req_off = 4U;
  uint16_t rsp_off = 4U;

  resp[0] = ID_DAP_TransferBlock;
  wr_u16(&resp[1], 0U);
  resp[3] = DAP_TRANSFER_OK;

  for (i = 0U; i < count; ++i)
  {
    uint32_t data = 0U;
    uint16_t tries = 0U;
    uint8_t ack;

    if ((request & DAP_TRANSFER_RnW) == 0U)
    {
      data = rd_u32(&req[req_off]);
      req_off += 4U;
    }

    do
    {
      uint32_t tmp = data;
      ack = swd_transfer_word(request, &tmp);
      if ((request & DAP_TRANSFER_RnW) != 0U)
      {
        data = tmp;
      }
      if (ack == DAP_TRANSFER_WAIT)
      {
        ++tries;
      }
      else
      {
        break;
      }
    } while (tries < g_wait_retry);

    transfer_response = ack;

    if (ack != DAP_TRANSFER_OK)
    {
      break;
    }

    ++completed;

    if ((request & DAP_TRANSFER_RnW) != 0U)
    {
      wr_u32(&resp[rsp_off], data);
      rsp_off += 4U;
    }
  }

  wr_u16(&resp[1], completed);
  resp[3] = transfer_response;
  return (uint8_t)rsp_off;
}

static uint8_t dap_handle_packet(const uint8_t *req, uint8_t *resp)
{
  switch (req[0])
  {
    case ID_DAP_Info:
      return dap_handle_info(req, resp);

    case ID_DAP_HostStatus:
      return dap_handle_host_status(req, resp);

    case ID_DAP_Connect:
      return dap_handle_connect(req, resp);

    case ID_DAP_Disconnect:
      return dap_handle_disconnect(req, resp);

    case ID_DAP_TransferConfigure:
      return dap_handle_transfer_configure(req, resp);

    case ID_DAP_Transfer:
      return dap_handle_transfer(req, resp);

    case ID_DAP_TransferBlock:
      return dap_handle_transfer_block(req, resp);

    case ID_DAP_WriteABORT:
      return dap_handle_write_abort(req, resp);

    case ID_DAP_Delay:
      return dap_handle_delay(req, resp);

    case ID_DAP_ResetTarget:
      return dap_handle_reset_target(req, resp);

    case ID_DAP_SWJ_Pins:
      return dap_handle_swj_pins(req, resp);

    case ID_DAP_SWJ_Clock:
      return dap_handle_swj_clock(req, resp);

    case ID_DAP_SWJ_Sequence:
      return dap_handle_swj_sequence(req, resp);

    case ID_DAP_SWD_Configure:
      return dap_handle_swd_configure(req, resp);

    default:
      resp[0] = req[0];
      resp[1] = DAP_ERROR;
      return 2U;
  }
}

/* ----------------------- USB interface callbacks ----------------------- */
static int8_t CUSTOM_HID_Init_FS(void)
{
  target_nrst_release();

  g_connected_port = DAP_PORT_DISABLED;
  g_idle_cycles = 0U;
  g_wait_retry = 128U;
  g_match_retry = 0U;
  g_swd_turnaround = 1U;
  g_swd_data_phase = 0U;
  g_swj_clock_hz = 1000000U;
  g_delay_loops = 8U;

  swclk_write(0U);
  swd_line_idle();

  return (USBD_OK);
}

static int8_t CUSTOM_HID_DeInit_FS(void)
{
  return (USBD_OK);
}

static int8_t CUSTOM_HID_OutEvent_FS(uint8_t event_idx, uint8_t state)
{
  USBD_CUSTOM_HID_HandleTypeDef *hhid;
  uint8_t resp[64];
  uint8_t resp_len;

  (void)event_idx;
  (void)state;

  if (hUsbDeviceFS.pClassData == NULL)
  {
    return (USBD_FAIL);
  }

  hhid = (USBD_CUSTOM_HID_HandleTypeDef *)hUsbDeviceFS.pClassData;

  memset(resp, 0, sizeof(resp));
  resp_len = dap_handle_packet(hhid->Report_buf, resp);
  cmsis_dap_send(resp, resp_len);

  return (USBD_OK);
}

static void target_nrst_assert(void)
{
  HAL_GPIO_WritePin(NRST_GPIO_Port, NRST_Pin, GPIO_PIN_RESET);
}

static void target_nrst_release(void)
{
  HAL_GPIO_WritePin(NRST_GPIO_Port, NRST_Pin, GPIO_PIN_SET);
}
