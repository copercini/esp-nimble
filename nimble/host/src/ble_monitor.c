/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "host/ble_monitor.h"

#if BLE_MONITOR

#include <stdio.h>
#include <inttypes.h>
#include "os/os.h"
#include "uart/uart.h"
#include "ble_monitor_priv.h"

/* UTC Timestamp for Jan 2016 00:00:00 */
#define UTC01_01_2016    1451606400

struct os_mutex lock;

struct uart_dev *uart;

static uint8_t tx_ringbuf[64];
static uint8_t tx_ringbuf_head;
static uint8_t tx_ringbuf_tail;

static inline int
inc_and_wrap(int i, int max)
{
    return (i + 1) & (max - 1);
}

static int
monitor_uart_tx_char(void *arg)
{
    uint8_t ch;

    /* No more data */
    if (tx_ringbuf_head == tx_ringbuf_tail) {
        return -1;
    }

    ch = tx_ringbuf[tx_ringbuf_tail];
    tx_ringbuf_tail = inc_and_wrap(tx_ringbuf_tail, sizeof(tx_ringbuf));

    return ch;
}

static void
monitor_uart_queue_char(uint8_t ch)
{
    int sr;

    OS_ENTER_CRITICAL(sr);

    /* We need to try flush some data from ringbuffer if full */
    while (inc_and_wrap(tx_ringbuf_head, sizeof(tx_ringbuf)) ==
            tx_ringbuf_tail) {
        uart_start_tx(uart);
        OS_EXIT_CRITICAL(sr);
        OS_ENTER_CRITICAL(sr);
    }

    tx_ringbuf[tx_ringbuf_head] = ch;
    tx_ringbuf_head = inc_and_wrap(tx_ringbuf_head, sizeof(tx_ringbuf));

    OS_EXIT_CRITICAL(sr);
}

static void
monitor_write(const void *buf, size_t len)
{
    const uint8_t *ch = buf;

    while (len--) {
        monitor_uart_queue_char(*ch++);
    }

    uart_start_tx(uart);
}

static void
encode_monitor_hdr(struct ble_monitor_hdr *hdr, int64_t ts, uint16_t opcode,
                   uint16_t len)
{
    int rc;
    struct os_timeval tv;

    hdr->hdr_len  = sizeof(hdr->type) + sizeof(hdr->ts32);
    hdr->data_len = htole16(4 + hdr->hdr_len + len);
    hdr->opcode   = htole16(opcode);
    hdr->flags    = 0;

    /* Calculate timestamp if not present (same way as used in log module) */
    if (ts < 0) {
        rc = os_gettimeofday(&tv, NULL);
        if (rc || tv.tv_sec < UTC01_01_2016) {
            ts = os_get_uptime_usec();
        } else {
            ts = tv.tv_sec * 1000000 + tv.tv_usec;
        }
    }

    /* Extended header */
    hdr->type = BLE_MONITOR_EXTHDR_TS32;
    hdr->ts32 = htole32(ts / 100);
}

int
ble_monitor_init(void)
{
    struct uart_conf uc = {
        .uc_speed = MYNEWT_VAL(BLE_MONITOR_UART_BAUDRATE),
        .uc_databits = 8,
        .uc_stopbits = 1,
        .uc_parity = UART_PARITY_NONE,
        .uc_flow_ctl = UART_FLOW_CTL_NONE,
        .uc_tx_char = monitor_uart_tx_char,
        .uc_rx_char = NULL,
        .uc_cb_arg = NULL,
    };

    uart = (struct uart_dev *)os_dev_open("uart0", OS_TIMEOUT_NEVER,
                                             &uc);
    if (!uart) {
        return -1;
    }

    os_mutex_init(&lock);

    return 0;
}

int
ble_monitor_send(uint16_t opcode, const void *data, size_t len)
{
    struct ble_monitor_hdr hdr;

    encode_monitor_hdr(&hdr, -1, opcode, len);

    os_mutex_pend(&lock, OS_TIMEOUT_NEVER);

    monitor_write(&hdr, sizeof(hdr));
    monitor_write(data, len);

    os_mutex_release(&lock);

    return 0;
}

int
ble_monitor_send_om(uint16_t opcode, const struct os_mbuf *om)
{
    const struct os_mbuf *om_tmp;
    struct ble_monitor_hdr hdr;
    uint16_t length = 0;

    om_tmp = om;
    while (om_tmp) {
        length += om_tmp->om_len;
        om_tmp = SLIST_NEXT(om_tmp, om_next);
    }

    encode_monitor_hdr(&hdr, -1, opcode, length);

    os_mutex_pend(&lock, OS_TIMEOUT_NEVER);

    monitor_write(&hdr, sizeof(hdr));

    while (om) {
        monitor_write(om->om_data, om->om_len);
        om = SLIST_NEXT(om, om_next);
    }

    os_mutex_release(&lock);

    return 0;
}

int
ble_monitor_new_index(uint8_t bus, uint8_t *addr, const char *name)
{
    struct ble_monitor_new_index pkt;

    pkt.type = 0; /* Primary controller, we don't support other */
    pkt.bus = bus;
    memcpy(pkt.bdaddr, addr, 6);
    strncpy(pkt.name, name, sizeof(pkt.name) - 1);
    pkt.name[sizeof(pkt.name) - 1] = '\0';

    ble_monitor_send(BLE_MONITOR_OPCODE_NEW_INDEX, &pkt, sizeof(pkt));

    return 0;
}

#endif
