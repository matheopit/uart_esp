/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "driver/uart.h"
#if !CONFIG_BT_LE_HCI_INTERFACE_USE_RAM
#include "driver/uhci.h"
#endif
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_log.h"

static const char *tag = "UHCI";

#if CONFIG_BT_LE_HCI_INTERFACE_USE_RAM
/*
 * ESP32-C6 (LE controller): the HCI UART transport is bridged by the
 * application through VHCI, so that HCI traffic can be filtered.
 *
 * The LE Extended Advertising and LE Periodic Advertising bits are cleared
 * from the LE Read Local Supported Features response. The Linux kernel then
 * only uses legacy advertising/scanning commands, so applications sending
 * legacy advertising commands on a raw HCI socket (e.g. Qt Bluetooth) no
 * longer get "Command Disallowed" because of mixed legacy/extended commands.
 */

#define UART_HCI_NUM       (1)

#define UART_RX_THRS       (100)

#define UART_RX_BUF_SIZE   (4096)
#define UART_TX_BUF_SIZE   (4096)

#define GPIO_UART_TXD_OUT  (4)
#define GPIO_UART_RXD_IN   (5)
#define GPIO_UART_RTS_OUT  (6)
#define GPIO_UART_CTS_IN   (7)

#define H4_TYPE_CMD        (0x01)
#define H4_TYPE_ACL        (0x02)
#define H4_TYPE_EVT        (0x04)

#define HCI_EVT_CMD_COMPLETE            (0x0E)
#define HCI_OP_LE_READ_LOCAL_FEATURES   (0x2003)
#define HCI_LE_FEAT1_EXT_ADV            (1 << 4)    /* LE features bit 12 */
#define HCI_LE_FEAT1_PER_ADV            (1 << 5)    /* LE features bit 13 */

/* H4 type + ACL header + ACL payload */
#define HCI_MAX_PKT_SIZE   (1 + 4 + 1024)

static void hci_filter_controller_pkt(uint8_t *data, uint16_t len)
{
    /* H4 type, event code, param len, num packets, opcode (2), status, features (8) */
    if (len >= 15 && data[0] == H4_TYPE_EVT && data[1] == HCI_EVT_CMD_COMPLETE &&
        (data[4] | (data[5] << 8)) == HCI_OP_LE_READ_LOCAL_FEATURES && data[6] == 0) {
        data[8] &= ~(HCI_LE_FEAT1_EXT_ADV | HCI_LE_FEAT1_PER_ADV);
    }
}

static void vhci_send_available(void)
{
}

static int vhci_host_recv(uint8_t *data, uint16_t len)
{
    hci_filter_controller_pkt(data, len);
    uart_write_bytes(UART_HCI_NUM, data, len);
    return 0;
}

static const esp_vhci_host_callback_t s_vhci_host_cb = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv = vhci_host_recv,
};

static void uart_read_exact(uint8_t *buf, size_t len)
{
    while (len) {
        int n = uart_read_bytes(UART_HCI_NUM, buf, len, portMAX_DELAY);
        if (n > 0) {
            buf += n;
            len -= n;
        }
    }
}

static void hci_uart_rx_task(void *arg)
{
    static uint8_t pkt[HCI_MAX_PKT_SIZE];
    size_t hdr_len, payload_len;

    while (1) {
        uart_read_exact(pkt, 1);
        if (pkt[0] == H4_TYPE_CMD) {
            hdr_len = 3;
            uart_read_exact(&pkt[1], hdr_len);
            payload_len = pkt[3];
        } else if (pkt[0] == H4_TYPE_ACL) {
            hdr_len = 4;
            uart_read_exact(&pkt[1], hdr_len);
            payload_len = pkt[3] | (pkt[4] << 8);
        } else {
            ESP_LOGW(tag, "Unsupported H4 packet type 0x%02x, dropped", pkt[0]);
            continue;
        }

        if (1 + hdr_len + payload_len > sizeof(pkt)) {
            ESP_LOGW(tag, "HCI packet too long (%u bytes), dropped", (unsigned)payload_len);
            while (payload_len) {
                size_t n = payload_len < sizeof(pkt) ? payload_len : sizeof(pkt);
                uart_read_exact(pkt, n);
                payload_len -= n;
            }
            continue;
        }

        uart_read_exact(&pkt[1 + hdr_len], payload_len);
        esp_vhci_host_send_packet(pkt, 1 + hdr_len + payload_len);
    }
}

static void hci_uart_install(void)
{
    uart_config_t uart_config = {
        .baud_rate = CONFIG_EXAMPLE_HCI_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
#ifdef CONFIG_EXAMPLE_HCI_UART_FLOW_CTRL_ENABLE
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
#else
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#endif
        .rx_flow_ctrl_thresh = UART_RX_THRS,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_HCI_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_HCI_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_HCI_NUM, GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, GPIO_UART_RTS_OUT, GPIO_UART_CTS_IN));
}

void app_main(void)
{
    esp_err_t ret;

    /* Initialize NVS — it is used to store PHY calibration data */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    hci_uart_install();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Bluetooth Controller initialize failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Bluetooth Controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_vhci_host_register_callback(&s_vhci_host_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "VHCI callback register failed: %s", esp_err_to_name(ret));
        return;
    }

    xTaskCreate(hci_uart_rx_task, "hci_uart_rx", 3072, NULL, 10, NULL);

    ESP_LOGI(tag, "HCI messages can be communicated over UART%d:\n"
             "--PINs: TxD %d, RxD %d, RTS %d, CTS %d\n"
             "--Baudrate: %d\n"
             "--LE Extended/Periodic Advertising hidden from the host", UART_HCI_NUM,
             GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, GPIO_UART_RTS_OUT, GPIO_UART_CTS_IN,
             CONFIG_EXAMPLE_HCI_UART_BAUDRATE);
}

#else /* ESP32-C3 / ESP32-S3: custom HCI transport over UHCI */

#define UART_HCI_NUM       (1)

#define UART_RX_THRS       (120)

#define UHCI_MAX_RX_BUFFER_SIZE (512)
#define UHCI_MAX_TX_BUFFER_SIZE (512)

#define GPIO_UART_TXD_OUT  (4)
#define GPIO_UART_RXD_IN   (5)
#define GPIO_UART_RTS_OUT  (6)
#define GPIO_UART_CTS_IN   (7)

#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_UART_TXD_OUT) | (1ULL<<GPIO_UART_RTS_OUT))
#define GPIO_INPUT_PIN_SEL   ((1ULL<<GPIO_UART_RXD_IN) | (1ULL<<GPIO_UART_CTS_IN))

// Operation functions for HCI UART Transport Layer
static bool hci_uart_tl_init(void);
static void hci_uart_tl_deinit(void);
static void hci_uart_tl_recv_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg);
static void hci_uart_tl_send_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg);
static void hci_uart_tl_flow_on(void);
static bool hci_uart_tl_flow_off(void);
static void hci_uart_tl_finish_transfers(void);

struct uart_txrxchannel {
    esp_bt_hci_tl_callback_t callback;
    void *arg;
    uint8_t *buf;
    uint32_t size;
};

struct uart_env_tag {
    struct uart_txrxchannel tx;
    struct uart_txrxchannel rx;
    uhci_controller_handle_t uhci_handle;
    SemaphoreHandle_t rx_process_sema;
    RingbufHandle_t ringbufhandle;
    volatile bool rx_eof;
    uint8_t rx_data_buffer[UHCI_MAX_RX_BUFFER_SIZE];
};

static struct uart_env_tag uart_env = {0};

static esp_bt_hci_tl_t s_hci_uart_tl_funcs = {
    ._magic = ESP_BT_HCI_TL_MAGIC_VALUE,
    ._version = ESP_BT_HCI_TL_VERSION,
    ._reserved = 0,
    ._open = (void *)hci_uart_tl_init,
    ._close = (void *)hci_uart_tl_deinit,
    ._finish_transfers = (void *)hci_uart_tl_finish_transfers,
    ._recv = (void *)hci_uart_tl_recv_async,
    ._send = (void *)hci_uart_tl_send_async,
    ._flow_on = (void *)hci_uart_tl_flow_on,
    ._flow_off = (void *)hci_uart_tl_flow_off,
};

static bool hci_uart_tl_init(void)
{
    return true;
}

static void hci_uart_tl_deinit(void)
{
}

static IRAM_ATTR void hci_uart_tl_recv_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg)
{
    assert(buf != NULL);
    assert(size != 0);
    assert(callback != NULL);

    uart_env.rx.callback = callback;
    uart_env.rx.arg = arg;
    uart_env.rx.size = size;
    uart_env.rx.buf = buf;

    xSemaphoreGive(uart_env.rx_process_sema);
}

static IRAM_ATTR void hci_uart_tl_send_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg)
{
    assert(buf != NULL);
    assert(size != 0);
    assert(callback != NULL);

    uart_env.tx.callback = callback;
    uart_env.tx.arg = arg;
    uart_env.tx.size = size;

    if (uhci_transmit(uart_env.uhci_handle, buf, size) != ESP_OK) {
        assert(0);
    }
}

static void hci_uart_tl_flow_on(void)
{
}

static bool hci_uart_tl_flow_off(void)
{
    return true;
}

static void hci_uart_tl_finish_transfers(void)
{
}

static IRAM_ATTR bool uhci_tx_done_cb(uhci_controller_handle_t uhci_ctrl,
                                          const uhci_tx_done_event_data_t *edata, void *user_ctx)
{
    assert(uart_env.tx.callback != NULL);
    assert(uart_env.tx.size == edata->sent_size);
    esp_bt_hci_tl_callback_t callback = uart_env.tx.callback;
    void *arg = uart_env.tx.arg;

    // clear callback pointer
    uart_env.tx.callback = NULL;
    uart_env.tx.arg = NULL;
    uart_env.tx.size = 0;

    // call handler
    callback(arg, ESP_BT_HCI_TL_STATUS_OK);

    // send notification to Bluetooth Controller task
    esp_bt_h4tl_eif_io_event_notify(1);

    return true;
}

static IRAM_ATTR bool uhci_rx_event_cb(uhci_controller_handle_t uhci_ctrl, const uhci_rx_event_data_t *edata, void *user_ctx)
{
    BaseType_t done = xRingbufferSendFromISR(uart_env.ringbufhandle, edata->data, edata->recv_size, 0);
    if (!done) {
        assert(0);
    }
    if (edata->flags.totally_received) {
        uart_env.rx_eof = true;
    }

    return true;
}

static void uhci_rx_done_process_task(void *arg)
{
    size_t item_size = 0;
    uint8_t *data = NULL;
    size_t items_waiting = 0;

    xSemaphoreTake(uart_env.rx_process_sema, portMAX_DELAY);
    while (1) {
        data = xRingbufferReceiveUpTo(uart_env.ringbufhandle, &item_size, portMAX_DELAY, uart_env.rx.size);
        // prepare next receive
        if (uart_env.rx_eof) {
            uhci_receive(uart_env.uhci_handle, uart_env.rx_data_buffer, UHCI_MAX_RX_BUFFER_SIZE);
            uart_env.rx_eof = false;
        }

        memcpy(uart_env.rx.buf, data, item_size);
        vRingbufferReturnItem(uart_env.ringbufhandle, data);
        uart_env.rx.size -= item_size;
        uart_env.rx.buf += item_size;
        if (uart_env.rx.size) {
            vRingbufferGetInfo(uart_env.ringbufhandle, NULL, NULL, NULL, NULL, &items_waiting);
            if (items_waiting < uart_env.rx.size) {
                ESP_LOGW(tag, "Ring buffer items size less than expected");
            }
            continue;
        }

        assert(uart_env.rx.callback != NULL);
        esp_bt_hci_tl_callback_t callback = uart_env.rx.callback;
        void *arg = uart_env.rx.arg;

        // clear callback pointer
        uart_env.rx.callback = NULL;
        uart_env.rx.arg = NULL;

        // call handler
        callback(arg, ESP_BT_HCI_TL_STATUS_OK);

        // send notification to Bluetooth Controller task
        esp_bt_h4tl_eif_io_event_notify(1);

        xSemaphoreTake(uart_env.rx_process_sema, portMAX_DELAY);
    }
}

static void uhci_rx_prepare(void)
{
    uhci_receive(uart_env.uhci_handle, uart_env.rx_data_buffer, UHCI_MAX_RX_BUFFER_SIZE);

    uart_env.ringbufhandle = xRingbufferCreate(UHCI_MAX_RX_BUFFER_SIZE * 4, RINGBUF_TYPE_BYTEBUF);
    if (uart_env.ringbufhandle == NULL) {
        ESP_LOGE(tag, "Ringbuffer create failed");
        return;
    }

    uart_env.rx_process_sema = xSemaphoreCreateBinary();
    xTaskCreate(uhci_rx_done_process_task, "trigger controller task", 4096, NULL, 5, NULL);
}


void uhci_uart_install(void)
{
    // configure UART1
    uart_config_t uart_config = {
        .baud_rate = CONFIG_EXAMPLE_HCI_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
#ifdef CONFIG_EXAMPLE_HCI_UART_FLOW_CTRL_ENABLE
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
#else
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#endif
        .rx_flow_ctrl_thresh = UART_RX_THRS,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_HCI_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_HCI_NUM, GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, GPIO_UART_RTS_OUT, GPIO_UART_CTS_IN));

    uhci_controller_config_t uhci_cfg = {
        .uart_port = UART_HCI_NUM,
        .tx_trans_queue_depth = 1,
        .max_receive_internal_mem = UHCI_MAX_RX_BUFFER_SIZE,
        .max_transmit_size = UHCI_MAX_TX_BUFFER_SIZE,
        .dma_burst_size = 32,
        .rx_eof_flags.idle_eof = 1,
    };

    ESP_ERROR_CHECK(uhci_new_controller(&uhci_cfg, &uart_env.uhci_handle));

    uhci_event_callbacks_t uhci_cbs = {
        .on_rx_trans_event = uhci_rx_event_cb,
        .on_tx_trans_done = uhci_tx_done_cb,
    };

    ESP_ERROR_CHECK(uhci_register_event_callbacks(uart_env.uhci_handle, &uhci_cbs, NULL));

    uhci_rx_prepare();
}


void app_main(void)
{
    esp_err_t ret;

    /* Initialize NVS — it is used to store PHY calibration data */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    uhci_uart_install();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.hci_tl_funcs = &s_hci_uart_tl_funcs;

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Bluetooth Controller initialize failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Bluetooth Controller initialize failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(tag, "HCI messages can be communicated over UART%d:\n"
             "--PINs: TxD %d, RxD %d, RTS %d, CTS %d\n"
             "--Baudrate: %d", UART_HCI_NUM,
             GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, GPIO_UART_RTS_OUT, GPIO_UART_CTS_IN,
             CONFIG_EXAMPLE_HCI_UART_BAUDRATE);

}

#endif /* CONFIG_BT_LE_HCI_INTERFACE_USE_RAM */
