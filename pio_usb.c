/**
 * Copyright (c) 2021 sekigon-gonnoc
 */

#pragma GCC push_options
#pragma GCC optimize("-O3")

#include <stdio.h>
#include <stdint.h>
#include <string.h> // memcpy

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "pio_usb.h"
#include "usb_definitions.h"
#include "usb_crc.h"
#include "usb_tx.pio.h"
#include "usb_rx.pio.h"

#define UNUSED_PARAMETER(x) (void)x

#define IRQ_TX_COMP_MASK (1 << usb_tx_fs_IRQ_COMP)
#define IRQ_RX_COMP_MASK (1 << IRQ_RX_EOP)
#define IRQ_RX_ALL_MASK ((1 << IRQ_RX_EOP) | (1 << IRQ_RX_BS_ERR))

typedef struct {
  PIO pio_usb_tx;  // colud not set to volatile
  uint sm_tx;
  uint offset_tx;
  uint tx_ch;

  PIO pio_usb_rx;  // colud not set to volatile
  uint sm_rx;
  uint offset_rx;
  uint offset_eop;
  uint rx_reset_instr;

  uint8_t usb_rx_buffer[128];
} pio_port_t;

static pio_port_t pio_port[1];
static root_port_t root_port[1];

static pio_usb_configuration_t current_config;

static void __not_in_flash_func(usb_transfer)(const pio_port_t *pp,
                                              uint8_t *data, uint16_t len) {
  dma_channel_transfer_from_buffer_now(pp->tx_ch, data, len);

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  pio0->irq |= IRQ_TX_COMP_MASK;  // clear complete flag

  while ((pio0->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, false);
  dma_channel_abort(pp->tx_ch);
}

void __no_inline_not_in_flash_func(send_setup_packet)(const pio_port_t *pp,
                                                      uint8_t addr,
                                                      uint8_t ep_num) {
  uint8_t packet[] = {USB_SYNC, USB_PID_SETUP, 0, 0};
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);

  usb_transfer(pp, packet, sizeof(packet));
}

void __no_inline_not_in_flash_func(send_out_token)(const pio_port_t *pp,
                                                   uint8_t addr,
                                                   uint8_t ep_num) {
  uint8_t packet[] = {USB_SYNC, USB_PID_OUT, 0, 0};
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);

  usb_transfer(pp, packet, sizeof(packet));
}

static void __no_inline_not_in_flash_func(send_ack)(const pio_port_t *pp) {
  uint8_t data[] = {USB_SYNC, USB_PID_ACK};
  // usb_transfer(data, sizeof(data));
  dma_channel_transfer_from_buffer_now(pp->tx_ch, data, 2);

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  pio0->irq |= IRQ_TX_COMP_MASK;  // clear complete flag

  while ((pio0->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, false);
  dma_channel_abort(pp->tx_ch);
}

void __no_inline_not_in_flash_func(send_nak)(const pio_port_t *pp) {
  uint8_t data[] = {USB_SYNC, USB_PID_NAK};
  usb_transfer(pp, data, sizeof(data));
}

static void __no_inline_not_in_flash_func(prepare_receive)(const pio_port_t *pp) {
  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pio_sm_clear_fifos(pp->pio_usb_rx, pp->sm_rx);
  pio_sm_restart(pp->pio_usb_rx, pp->sm_rx);
  pio_sm_exec(pp->pio_usb_rx, pp->sm_rx, pp->rx_reset_instr);
}

static void __no_inline_not_in_flash_func(start_receive)(const pio_port_t *pp) {
  pp->pio_usb_rx->ctrl |= (1 << pp->sm_rx);
  pp->pio_usb_rx->irq |= IRQ_RX_ALL_MASK;
}

void __no_inline_not_in_flash_func(data_transfer)(const pio_port_t *pp,
                                                  uint8_t *tx_data_address,
                                                  uint8_t tx_data_len) {
  prepare_receive(pp);
  usb_transfer(pp, tx_data_address, tx_data_len);
  start_receive(pp);
}

void __no_inline_not_in_flash_func(control_setup_transfer)(
    const pio_port_t *pp, uint8_t device_address, uint8_t *tx_data_address,
    uint8_t tx_data_len) {
  prepare_receive(pp);

  send_setup_packet(pp, device_address, 0);
  usb_transfer(pp, tx_data_address, tx_data_len);

  start_receive(pp);
}

void __no_inline_not_in_flash_func(control_out_transfer)(
    const pio_port_t *pp, uint8_t device_address, uint8_t *tx_data_address,
    uint8_t tx_data_len) {
  prepare_receive(pp);

  send_out_token(pp, device_address, 0);
  usb_transfer(pp, tx_data_address, tx_data_len);

  start_receive(pp);
}

void  __no_inline_not_in_flash_func(calc_in_token)(uint8_t * packet, uint8_t addr, uint8_t ep_num) {
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[0] = USB_SYNC;
  packet[1] = USB_PID_IN;
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);
}

static void __no_inline_not_in_flash_func(wait_handshake)(pio_port_t* pp) {
  int16_t t = 120;
  int16_t idx = 0;

  while (t--) {
    if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
      uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
      pp->usb_rx_buffer[idx++] = data;
      if (idx == 2) {
        break;
      }
    }
  }

  if (t > 0) {
    while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      continue;
    }
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
}

static int __no_inline_not_in_flash_func(usb_out_transaction)(pio_port_t* pp, uint8_t addr, endpoint_t * ep) {
  int res = -1;

  prepare_receive(pp);
  send_out_token(pp, addr, ep->ep_num & 0xf);
  usb_transfer(pp, (uint8_t*)ep->buffer, ep->packet_len);
  start_receive(pp);

  wait_handshake(pp);

  if (pp->usb_rx_buffer[1] == USB_PID_ACK) {
    res = 0;
    ep->data_id ^= 1;
    ep->new_data_flag = false;
  } else {
    res = -1;
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pp->usb_rx_buffer[0] = 0;
  pp->usb_rx_buffer[1] = 0;

  return res;
}

static int __no_inline_not_in_flash_func(receive_packet_and_ack)(pio_port_t* pp) {
  uint16_t crc = 0xffff;
  uint16_t crc_prev = 0xffff;
  uint16_t crc_prev2 = 0xffff;
  uint16_t crc_receive = 0xffff;
  bool  crc_match = false;
  int16_t t = 120;
  uint16_t idx = 0;

  while (t--) {
    if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
      uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
      pp->usb_rx_buffer[idx++] = data;
      if (idx == 2) {
        break;
      }
    }
  }

  // timing critical start
  if (t > 0) {
    while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
        uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        crc_prev2 = crc_prev;
        crc_prev = crc;
        crc = update_usb_crc16(crc, data);
        pp->usb_rx_buffer[idx++] = data;
        crc_receive = (crc_receive >> 8) | (data << 8);
        crc_match = ((crc_receive ^ 0xffff) == crc_prev2);
      }
    }
  }

  if (idx >= 4 && crc_match) {
    send_ack(pp);
    // timing critical end
    return idx - 4;
  }

  return -1;
}

static int __no_inline_not_in_flash_func(usb_in_transaction)(pio_port_t* pp, uint8_t addr, endpoint_t * ep) {
  int res = -1;

  uint8_t expect_token = ep->data_id == 0 ? USB_PID_DATA0 : USB_PID_DATA1;
  uint8_t packet[4];
  calc_in_token(packet, addr, ep->ep_num & 0x0f);
  data_transfer(pp, packet, sizeof(packet));

  int receive_len = receive_packet_and_ack(pp);
  if (receive_len >= 0) {
    if (pp->usb_rx_buffer[1] == expect_token) {
      memcpy((void *)(ep->buffer), &pp->usb_rx_buffer[2], receive_len);
      ep->packet_len = receive_len;
      ep->data_id ^= 1;
      ep->new_data_flag = true;
    }
    res = 0;
  } else {
    res = -1;
    if ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      res = -2;
    }
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pp->usb_rx_buffer[0] = 0;
  pp->usb_rx_buffer[1] = 0;

  return res;
}

static int __no_inline_not_in_flash_func(usb_control_out_transfer)(
    pio_port_t *pp, uint8_t addr, control_pipe_t *pipe) {
  int res = 0;

  if (pipe->stage == STAGE_SETUP) {
    control_setup_transfer(pp, addr, pipe->setup_packet.tx_address,
                           pipe->setup_packet.tx_length);
    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      if (pipe->out_data_packet.tx_address != NULL) {
        pipe->stage = STAGE_OUT;
      } else {
        pipe->stage = STAGE_STATUS;
      }
      pp->usb_rx_buffer[1] = 0;  // reset buffer
    }
  } else if (pipe->stage == STAGE_OUT) {
    control_out_transfer(pp, addr, pipe->out_data_packet.tx_address,
                         pipe->out_data_packet.tx_length);
    wait_handshake(pp);
    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
        pipe->stage = STAGE_STATUS;
      }
      pp->usb_rx_buffer[1] = 0;
  } else if (pipe->stage == STAGE_STATUS) {
    uint8_t packet[4];
    calc_in_token(packet, addr, 0);
    data_transfer(pp, packet, sizeof(packet));

    volatile int16_t t = 120;
    volatile int16_t idx = 0;
    while (t--) {
      if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
        pp->usb_rx_buffer[idx++] = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        if (idx == 2) {
          break;
        }
      }
    }

    if (t > 0) {
      while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
        if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
          pp->usb_rx_buffer[idx++] = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        }
      }

      if (pp->usb_rx_buffer[1] == USB_PID_DATA1) {
        send_ack(pp);
        pipe->stage = STAGE_COMPLETE;
      }
    }

    pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
    pp->usb_rx_buffer[1] = 0;
  }

  return res;
}

static uint8_t __no_inline_not_in_flash_func(usb_control_in_transfer)(pio_port_t* pp, uint8_t addr, control_pipe_t * pipe) {
  if (pipe->stage == STAGE_SETUP) {
    control_setup_transfer(pp, addr, pipe->setup_packet.tx_address,
                           pipe->setup_packet.tx_length);

    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      pipe->stage = STAGE_IN;
      pp->usb_rx_buffer[1] = 0; // reset buffer
      pipe->data_in_num = 1;
      pipe->buffer_idx = 0;
    }
  } else if (pipe->stage == STAGE_IN) {
    uint8_t expect_token =
        pipe->data_in_num == 1 ? USB_PID_DATA1 : USB_PID_DATA0;
    uint8_t packet[4];
    calc_in_token(packet, addr, 0);
    data_transfer(pp, packet, sizeof(packet));

    int receive_len = receive_packet_and_ack(pp);

    if (receive_len >= 0) {
      if (pp->usb_rx_buffer[1] == expect_token) {
        memcpy((void *)(pipe->rx_buffer + pipe->buffer_idx), &pp->usb_rx_buffer[2],
               receive_len);
        pipe->buffer_idx += receive_len;

        if (pipe->request_length <= pipe->buffer_idx) {
          pipe->stage = STAGE_STATUS;
        }

        pipe->data_in_num ^= 1;  // togle data num
      }
    }

    pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
    pp->usb_rx_buffer[1] = 0;  // reset buffer
  }

  else if (pipe->stage == STAGE_STATUS) {
    uint8_t status_packet[] = {USB_SYNC, USB_PID_DATA1, 0, 0};
    control_out_transfer(pp, addr, status_packet,
                         sizeof(status_packet));

    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      pipe->stage = STAGE_COMPLETE;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    } else if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_NAK) {
      pipe->stage = STAGE_STATUS;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    } else {
      pipe->stage = STAGE_ERROR;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    }
  }

  return pipe->stage;
}

static bool __no_inline_not_in_flash_func(connection_check)(root_port_t * port) {
  if (gpio_get(port->pin_dp) == 0 && gpio_get(port->pin_dm) == 0) {
    busy_wait_us_32(1);

    if (gpio_get(port->pin_dp) == 0 && gpio_get(port->pin_dm) == 0) {
      // device disconnect
      if (port->root_device.connected) {
        port->root_device.connected = false;
        port->root_device.event = EVENT_DISCONNECT;
        port->event = EVENT_DISCONNECT;
      }
      return false;
    }
  }

  return true;
}

static void __no_inline_not_in_flash_func(configure_fullspeed_host)(pio_port_t* pp, const pio_usb_configuration_t * c, root_port_t * port) {
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  pp->sm_tx = c->sm_tx;
  pp->tx_ch = c->tx_ch;
  pp->pio_usb_rx = c->pio_rx_num == 0 ? pio0 : pio1;
  pp->sm_rx = c->sm_rx;
  port->pin_dp = c->pin_dp;
  port->pin_dm = c->pin_dp + 1;

  if (pp->offset_tx){
    pio_remove_program(pp->pio_usb_tx, &usb_tx_fs_program, pp->offset_tx);
  }

  pp->offset_tx = pio_add_program(pp->pio_usb_tx, &usb_tx_fs_program);
  usb_tx_fs_program_init(pp->pio_usb_tx, pp->sm_tx, pp->offset_tx, port->pin_dp);

  if (pp->offset_rx){
    pio_remove_program(pp->pio_usb_rx, &usb_rx_fs_program, pp->offset_rx);
  }

  if (c->debug_pin_rx < 0) {
    pp->offset_rx = pio_add_program(pp->pio_usb_rx, &usb_rx_fs_program);
  } else {
    pp->offset_rx = pio_add_program(pp->pio_usb_rx, &usb_rx_fs_debug_program);
  }
  usb_rx_fs_program_init(pp->pio_usb_rx, pp->sm_rx, pp->offset_rx, port->pin_dp, c->debug_pin_rx);
  pp->rx_reset_instr = pio_encode_jmp(pp->offset_rx);

  if (pp->offset_eop){
    pio_remove_program(pp->pio_usb_rx, &eop_detect_program, pp->offset_eop);
  }

  if (c->debug_pin_eop < 0) {
    pp->offset_eop = pio_add_program(pp->pio_usb_rx, &eop_detect_program);
  } else {
    pp->offset_eop = pio_add_program(pp->pio_usb_rx, &eop_detect_debug_program);
  }
  eop_detect_program_init(pp->pio_usb_rx, c->sm_eop, pp->offset_eop, port->pin_dp, true,
                          c->debug_pin_eop);
}

static void __no_inline_not_in_flash_func(configure_lowspeed_host)(pio_port_t* pp, const pio_usb_configuration_t * c, root_port_t * port) {
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  pp->sm_tx = c->sm_tx;
  pp->tx_ch = c->tx_ch;
  pp->pio_usb_rx = c->pio_rx_num == 0 ? pio0 : pio1;
  pp->sm_rx = c->sm_rx;
  port->pin_dp = c->pin_dp;
  port->pin_dm = c->pin_dp + 1;

  if (pp->offset_tx){
    pio_remove_program(pp->pio_usb_tx, &usb_tx_fs_program, pp->offset_tx);
  }

  pp->offset_tx = pio_add_program(pp->pio_usb_tx, &usb_tx_ls_program);
  usb_tx_ls_program_init(pp->pio_usb_tx, pp->sm_tx, pp->offset_tx, port->pin_dp);

  if (pp->offset_rx){
    pio_remove_program(pp->pio_usb_rx, &usb_rx_fs_program, pp->offset_rx);
  }

  if (c->debug_pin_rx < 0) {
    pp->offset_rx = pio_add_program(pp->pio_usb_rx, &usb_rx_ls_program);
  } else {
    pp->offset_rx = pio_add_program(pp->pio_usb_rx, &usb_rx_ls_debug_program);
  }
  usb_rx_ls_program_init(pp->pio_usb_rx, pp->sm_rx, pp->offset_rx, port->pin_dp, c->debug_pin_rx);
  pp->rx_reset_instr = pio_encode_jmp(pp->offset_rx);

  if (pp->offset_eop){
    pio_remove_program(pp->pio_usb_rx, &eop_detect_program, pp->offset_eop);
  }

  if (c->debug_pin_eop < 0) {
    pp->offset_eop = pio_add_program(pp->pio_usb_rx, &eop_detect_program);
  } else {
    pp->offset_eop = pio_add_program(pp->pio_usb_rx, &eop_detect_debug_program);
  }
  eop_detect_program_init(pp->pio_usb_rx, c->sm_eop, pp->offset_eop, port->pin_dp, false,
                          c->debug_pin_eop);
}

static bool __no_inline_not_in_flash_func(sof_timer)(repeating_timer_t *_rt) {
  uint8_t sof_packet[4] = {USB_SYNC, USB_PID_SOF, 0x00, 0x10};
  static uint8_t sof_count = 0;
  UNUSED_PARAMETER(_rt);

  usb_device_t *device = &root_port[0].root_device;
  uint8_t addr = root_port[0].root_device.address;
  control_pipe_t *pipe = &root_port[0].root_device.control_pipe;
  pio_port_t *pp = &pio_port[0];

  if (device->connected && connection_check(&root_port[0])) {

    usb_transfer(pp, sof_packet, sizeof(sof_packet));

    switch (pipe->operation) {
      case CONTROL_NONE:
        break;
      case CONTROL_IN:
        usb_control_in_transfer(pp, addr, pipe);
        if (pipe->stage == STAGE_COMPLETE) {
          pipe->stage = STAGE_SETUP;
          pipe->operation = CONTROL_COMPLETE;
        } else if (pipe->stage == STAGE_ERROR) {
          pipe->stage = STAGE_SETUP;
          pipe->operation = CONTROL_ERROR;
        }
        break;
      case CONTROL_OUT:
        usb_control_out_transfer(pp, addr, pipe);
        if (pipe->stage == STAGE_COMPLETE) {
          pipe->stage = STAGE_SETUP;
          pipe->operation = CONTROL_COMPLETE;
        } else if (pipe->stage == STAGE_ERROR) {
          pipe->stage = STAGE_SETUP;
          pipe->operation = CONTROL_ERROR;
        }
        break;
      default:
        break;
    }

    for (int ep_idx = 0; ep_idx < PIO_USB_EP_CNT; ep_idx++) {
      endpoint_t * ep = &device->endpoint[ep_idx];
      if (!ep->ep_num || !ep->is_interrupt) {
        continue;
      }

      if (ep->interval_counter > 0) {
        ep->interval_counter--;
        continue;
      }

      if (ep->ep_num & EP_IN) {
        int res = usb_in_transaction(pp, device->address, ep);
          ep->interval_counter = ep->interval - 1;
        if (res == 0) {
          // ep->interval_counter = ep->interval;
        } else if (res <= -2) {
          // fatal
          device->connected = false;
          return true;
        }
      } else {
        if (ep->new_data_flag) {
          int res = usb_out_transaction(pp, device->address, ep);
            ep->interval_counter = ep->interval - 1;
          if (res == 0) {
            ep->interval_counter = ep->interval - 1;
          }
        }
      }
    }

  } else {
    if (device->event == EVENT_NONE &&
        ((gpio_get(root_port[0].pin_dp) == 1 && gpio_get(root_port[0].pin_dm) == 0) ||
         ((gpio_get(root_port[0].pin_dp) == 0 && gpio_get(root_port[0].pin_dm) == 1)))) {
      root_port->event = EVENT_CONNECT;
      device->event = EVENT_CONNECT;
    }
  }

  sof_count = (sof_count + 1) & 0x1f;
  sof_packet[2] = sof_count & 0xff;
  sof_packet[3] = (calc_usb_crc5(sof_count) << 3) | (sof_count >> 8);

  return true;
}

static void on_device_connect(pio_port_t *pp, root_port_t *port) {
  bool fullspeed_flag = false;

  if (gpio_get(port->pin_dp) == 1 && gpio_get(port->pin_dm) == 0) {
    fullspeed_flag = true;
  } else if (gpio_get(port->pin_dp) == 0 && gpio_get(port->pin_dm) == 1) {
    fullspeed_flag = false;
  }

  pio_sm_set_pins_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b00 << port->pin_dp),
                            (0b11u << port->pin_dp));
  pio_sm_set_pindirs_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b11u << port->pin_dp),
                               (0b11u << port->pin_dp));

  busy_wait_ms(100);

  pio_sm_set_pindirs_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b00u << port->pin_dp),
                               (0b11u << port->pin_dp));

  busy_wait_us(100);

  if (fullspeed_flag && gpio_get(port->pin_dp) == 1 && gpio_get(port->pin_dm) == 0) {
    if (!port->root_device.connected) {
      configure_fullspeed_host(pp, &current_config, port);
      port->root_device.is_fullspeed = true;
      port->root_device.connected = true;
    }
  } else if (!fullspeed_flag && gpio_get(port->pin_dp) == 0 &&
             gpio_get(port->pin_dm) == 1) {
    if (!port->root_device.connected) {
      configure_lowspeed_host(pp, &current_config, port);
      port->root_device.is_fullspeed = false;
      port->root_device.connected = true;
    }
  }
}

static void update_packet_crc16(usb_setup_packet_t * packet) {
  uint16_t crc16 = calc_usb_crc16(&packet->request_type,
                                  sizeof(*packet) - 4);
  packet->crc16[0] = crc16 & 0xff;
  packet->crc16[1] = crc16 >> 8;
}

static int __no_inline_not_in_flash_func(control_out_protocol)(
    usb_device_t *device, uint8_t *setup_data, uint16_t setup_length,
    uint8_t *out_data, uint16_t out_length) {
  int res = 0;

  control_pipe_t *pipe = &device->control_pipe;

  if (pipe->operation == CONTROL_NONE) {
    pipe->setup_packet.tx_address = setup_data;
    pipe->setup_packet.tx_length = setup_length;
    pipe->out_data_packet.tx_address = out_data;
    pipe->out_data_packet.tx_length = out_length;
    pipe->operation = CONTROL_OUT;
  } else {
    return -1;
  }

  const uint64_t timeout = 5000 * 1000;  // 5s
  uint64_t start_time = time_us_64();
  while (pipe->operation == CONTROL_OUT &&
         time_us_64() - start_time < timeout) {
    if (!device->connected) {
      pipe->operation = CONTROL_NONE;
      return -1;
    }
  }

  if (time_us_64() - start_time >= timeout) {
    printf("control out[timeout]\n");
    res = -2;
  } else if (pipe->operation == CONTROL_ERROR) {
    printf("control out[error]\n");
    res = -1;
  } else if (pipe->operation == CONTROL_COMPLETE) {
    printf("control out[complete]\n");
    res = 0;
  }
    pipe->operation = CONTROL_NONE;

  return res;
}

static int __no_inline_not_in_flash_func(control_in_protocol)(
    usb_device_t *device, uint8_t *data, uint16_t length) {
  int res = 0;

  control_pipe_t *pipe = &device->control_pipe;

  if (pipe->operation == CONTROL_NONE) {
    pipe->setup_packet.tx_address = data;
    pipe->setup_packet.tx_length = length;
    pipe->operation = CONTROL_IN;
  } else {
    return -1;
  }

  const uint64_t timeout = 5000 * 1000;  // 5s
  uint64_t start_time = time_us_64();
  while (pipe->operation == CONTROL_IN &&
         time_us_64() - start_time < timeout) {
    if (!device->connected) {
      pipe->operation = CONTROL_NONE;
      return -1;
    }
  }

  if (time_us_64() - start_time >= timeout) {
    printf("control in[timeout]\n");
    res = -2;
  } else if (pipe->operation == CONTROL_ERROR) {
    printf("control in[error]\n");
    res = -1;
  } else if (pipe->operation == CONTROL_COMPLETE) {
    printf("control in[complete]\n");
    res = 0;
  }
  pipe->operation = CONTROL_NONE;

  return res;
}

int __no_inline_not_in_flash_func(pio_usb_get_in_data)(endpoint_t *ep,
                                                       uint8_t *buffer,
                                                       uint8_t len) {
  if ((ep->ep_num & EP_IN) && ep->new_data_flag) {
    len = len < ep->packet_len ? len : ep->packet_len;
    memcpy(buffer, (void *)ep->buffer, len);

    ep->new_data_flag = false;

    return len;
  }

  return -1;
}

int __no_inline_not_in_flash_func(pio_usb_set_out_data)(endpoint_t *ep,
                                                          const uint8_t *buffer,
                                                          uint8_t len) {
  if (ep->new_data_flag || !ep->is_interrupt || (ep->ep_num & EP_IN)) {
    return -1;
  }

  ep->buffer[0] = USB_SYNC;
  ep->buffer[1] = ep->data_id == 0 ? USB_PID_DATA0 : USB_PID_DATA1;
  memcpy((uint8_t *)&ep->buffer[2], buffer, len);
  uint16_t crc = calc_usb_crc16(buffer, len);
  ep->buffer[2 + len] = crc & 0xff;
  ep->buffer[2 + len + 1] = (crc >> 8) & 0xff;
  ep->packet_len = len + 4;

  ep->new_data_flag = true;
  return 0;
}

static int enumerate_device(usb_device_t * device) {
  int res = 0;
  device->control_pipe.request_length = 18;

  usb_setup_packet_t get_device_descriptor_request =
      GET_DEVICE_DESCRIPTOR_REQ_DEFAULT;
  update_packet_crc16(&get_device_descriptor_request);
  res = control_in_protocol(device, (uint8_t *)&get_device_descriptor_request,
                            sizeof(get_device_descriptor_request));
  if (res != 0) {
    return res;
  }

  const device_descriptor_t * desc = (device_descriptor_t*)device->control_pipe.rx_buffer;
  device->vid = desc->vid[0] | (desc->vid[1] << 8);
  device->pid = desc->pid[0] | (desc->pid[1] << 8);

  usb_setup_packet_t set_address_request = SET_ADDRESS_REQ_DEFAULT;
  set_address_request.value_lsb = 1;
  set_address_request.value_msb = 0;
  update_packet_crc16(&set_address_request);
  res = control_out_protocol(device, (uint8_t *)&set_address_request,
                             sizeof(set_address_request), NULL, 0);
  if (res != 0) {
    return res;
  }
  device->address = 1;

  usb_setup_packet_t get_configuration_descriptor_request =
      GET_CONFIGURATION_DESCRIPTOR_REQ_DEFAULT;
  get_configuration_descriptor_request.length_lsb = 9;
  get_configuration_descriptor_request.length_msb = 0;
  device->control_pipe.request_length = 9;
  update_packet_crc16(&get_configuration_descriptor_request);
  res = control_in_protocol(device, (uint8_t *)&get_configuration_descriptor_request,
                            sizeof(get_configuration_descriptor_request));
  if (res != 0) {
    return res;
  }

  get_configuration_descriptor_request.length_lsb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->total_length_lsb;
  get_configuration_descriptor_request.length_msb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->total_length_msb;
  device->control_pipe.request_length =
      get_configuration_descriptor_request.length_lsb |
      (get_configuration_descriptor_request.index_msb << 8);
  update_packet_crc16(&get_configuration_descriptor_request);
  res = control_in_protocol(device, (uint8_t *)&get_configuration_descriptor_request,
                            sizeof(get_configuration_descriptor_request));

  if (res != 0) {
    return res;
  }
  uint8_t configuration_descrptor_data[512];
  int16_t configuration_descrptor_length =
      device->control_pipe.request_length > 512
          ? 512
          : device->control_pipe.request_length;
  memcpy(configuration_descrptor_data,
         (const void *)device->control_pipe.rx_buffer,
         configuration_descrptor_length);

  usb_setup_packet_t set_usb_configuration_request =
      SET_CONFIGURATION_REQ_DEFAULT;
  set_usb_configuration_request.value_lsb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->configuration_value;
  update_packet_crc16(&set_usb_configuration_request);
  res = control_out_protocol(device, (uint8_t *)&set_usb_configuration_request,
                             sizeof(set_usb_configuration_request), NULL, 0);

  if (res != 0) {
    return res;
  }
  volatile uint8_t ep_idx = 0;
  volatile uint8_t interface = 0;
  volatile uint8_t class = 0;
  uint8_t *descriptor = configuration_descrptor_data;
  while (configuration_descrptor_length > 0) {
    switch (descriptor[1]) {
      case DESC_TYPE_INTERFACE: {
        const interface_descriptor_t *d =
            (const interface_descriptor_t *)descriptor;
        printf(
            "inum:%d, altsetting:%d, numep:%d, iclass:%d, isubclass:%d, "
            "iprotcol:%d, iface:%d\n",
            d->inum, d->altsetting, d->numep, d->iclass, d->isubclass,
            d->iprotocol, d->iface);
        interface = d->inum;
        class = d->iclass;
      } break;
      case DESC_TYPE_ENDPOINT: {
        const endpoint_descriptor_t *d =
            (const endpoint_descriptor_t *)descriptor;
        printf("\t\t\tepaddr:0x%02x, attr:%d, size:%d, interval:%d\n", d->epaddr,
               d->attr, d->max_size[0] | (d->max_size[1] << 8), d->interval);

        if (class == CLASS_HID && d->attr == EP_ATTR_INTERRUPT) {
          volatile endpoint_t *ep = &device->endpoint[ep_idx];
          ep->interval = d->interval;
          ep->interval_counter = 0;
          ep->size = d->max_size[0] | (d->max_size[1] << 8);
          ep->is_interrupt = true;
          ep->ep_num = d->epaddr;
          ep_idx++;
        }
      } break;
      case DESC_TYPE_HID: {
        const hid_descriptor_t *d = (const hid_descriptor_t *)descriptor;
        printf(
            "\tbcdHID:%x.%x, country:%d, desc num:%d, desc_type:%d, "
            "desc_size:%d\n",
            d->bcd_hid[1], d->bcd_hid[0], d->contry_code, d->num_desc,
            d->desc_type, d->desc_len[0] | (d->desc_len[1] << 8));

        usb_setup_packet_t set_hid_idle_request = SET_HID_IDLE_REQ_DEFAULT;
        set_hid_idle_request.value_lsb = interface;
        set_hid_idle_request.value_msb = 0;
        update_packet_crc16(&set_hid_idle_request);
        control_out_protocol(device, (uint8_t *)&set_hid_idle_request,
                             sizeof(set_hid_idle_request), NULL, 0);

        usb_setup_packet_t get_hid_report_descrpitor_request =
            GET_HID_REPORT_DESCRIPTOR_DEFAULT;
        get_hid_report_descrpitor_request.index_lsb = interface;
        get_hid_report_descrpitor_request.index_msb = 0;
        get_hid_report_descrpitor_request.length_lsb = d->desc_len[0];
        get_hid_report_descrpitor_request.length_msb = d->desc_len[1];
        uint16_t desc_len = d->desc_len[0] | (d->desc_len[1] << 8);
        device->control_pipe.request_length = desc_len;
        update_packet_crc16(&get_hid_report_descrpitor_request);
        control_in_protocol(device,
                            (uint8_t *)&get_hid_report_descrpitor_request,
                            sizeof(get_hid_report_descrpitor_request));
        printf("\t\tReport descriptor:");
        for (int i = 0; i < desc_len; i++) {
          printf("%02x ", device->control_pipe.rx_buffer[i]);
        }
        printf("\n");
        stdio_flush();

      } break;
      default:
        break;
    }

    configuration_descrptor_length -= descriptor[0];
    descriptor += descriptor[0];
  }

  return res;
}

static void configure_tx_channel(uint8_t ch, PIO pio, uint sm) {
  dma_channel_config conf = dma_channel_get_default_config(ch);

  channel_config_set_read_increment(&conf, true);
  channel_config_set_write_increment(&conf, false);
  channel_config_set_transfer_data_size(&conf, DMA_SIZE_8);
  channel_config_set_dreq(&conf, pio_get_dreq(pio, sm, true));

  dma_channel_set_config(ch, &conf, false);
  dma_channel_set_write_addr(ch, &pio->txf[sm], false);
}

static repeating_timer_t sof_rt;
static bool timer_active;

static void start_timer(alarm_pool_t *alarm_pool) {
  if (timer_active) {
    return;
  }

  if (alarm_pool != NULL) {
    alarm_pool_add_repeating_timer_us(alarm_pool, -1000, sof_timer, NULL,
                                      &sof_rt);
  } else {
    add_repeating_timer_us(-1000, sof_timer, NULL, &sof_rt);
  }

  timer_active = true;
}

static void stop_timer(void) {
  cancel_repeating_timer(&sof_rt);
  timer_active = false;
}

usb_device_t *pio_usb_init(const pio_usb_configuration_t *c) {
  pio_port_t *pp = &pio_port[0];
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  configure_tx_channel(c->tx_ch, pp->pio_usb_tx, c->sm_tx);

  configure_fullspeed_host(pp, c, &root_port[0]);

  start_timer(c->alarm_pool);

  current_config = *c;

  return &root_port[0].root_device;
}


static volatile bool cancel_timer_flag;
static volatile bool start_timer_flag;
static uint32_t int_stat;

void pio_usb_stop(void) {
  cancel_timer_flag = true;
  while (cancel_timer_flag) {
    continue;
  }
}

void pio_usb_restart(void) {
  start_timer_flag = true;
  while (start_timer_flag) {
    continue;
  }
}

void __no_inline_not_in_flash_func(pio_usb_task)(void) {
  usb_device_t * device = &root_port[0].root_device;

  if (root_port[0].event == EVENT_CONNECT) {
    root_port[0].event = EVENT_NONE;
    on_device_connect(&pio_port[0], &root_port[0]);
  }

  if (device->event == EVENT_CONNECT) {
    device->event = EVENT_NONE;
    printf("Connected\n");
    int res = enumerate_device(device);
    if (res == 0) {
      device->enumerated = true;
    } else {
      device->connected = false;
      device->event = EVENT_DISCONNECT;
    }
  } else if (device->event == EVENT_DISCONNECT) {
    device->event = EVENT_NONE;
    printf("Disconnect\n");
    memset(device, 0, sizeof(*device));
    device->enumerated = false;
  }

  if (cancel_timer_flag) {
    int_stat = save_and_disable_interrupts();
    stop_timer();
    memset(device, 0, sizeof(*device));
    cancel_timer_flag = false;
  }

  if (start_timer_flag) {
    start_timer(current_config.alarm_pool);
    restore_interrupts(int_stat);
    start_timer_flag = false;
  }
}

#pragma GCC pop_options
