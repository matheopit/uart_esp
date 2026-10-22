/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_log.h"

#if !CONFIG_BT_LE_HCI_INTERFACE_USE_RAM
#error "ESP32-C6 only: set CONFIG_BT_LE_HCI_INTERFACE_USE_RAM (see sdkconfig.defaults.esp32c6)"
#endif

static const char *tag = "HCI_UART";

/*
 * ESP32-C6 (LE controller): the HCI UART transport is bridged by the
 * application through VHCI, so that HCI traffic can be filtered.
 *
 * With CONFIG_EXAMPLE_HCI_HIDE_BLE5_ADV, the LE Extended Advertising and LE
 * Periodic Advertising bits are cleared from the LE Read Local Supported
 * Features response. The Linux kernel then only uses legacy advertising/
 * scanning commands, so applications sending legacy advertising commands on a
 * raw HCI socket (e.g. Qt Bluetooth) no longer get "Command Disallowed"
 * because of mixed legacy/extended commands. Drawback: devices using only
 * extended advertising are no longer seen by the host.
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

#if CONFIG_EXAMPLE_HCI_HIDE_BLE5_ADV
#define BLE5_ADV_STATE     "hidden from the host"
#else
#define BLE5_ADV_STATE     "visible to the host"
#endif

/* H4 type + ACL header + ACL payload */
#define HCI_MAX_PKT_SIZE   (1 + 4 + 1024)

static void hci_filter_controller_pkt(uint8_t *data, uint16_t len)
{
#if CONFIG_EXAMPLE_HCI_HIDE_BLE5_ADV
    /* H4 type, event code, param len, num packets, opcode (2), status, features (8) */
    if (len >= 15 && data[0] == H4_TYPE_EVT && data[1] == HCI_EVT_CMD_COMPLETE &&
        (data[4] | (data[5] << 8)) == HCI_OP_LE_READ_LOCAL_FEATURES && data[6] == 0) {
        data[8] &= ~(HCI_LE_FEAT1_EXT_ADV | HCI_LE_FEAT1_PER_ADV);
    }
#endif
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
             "--LE Extended/Periodic Advertising: %s", UART_HCI_NUM,
             GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, GPIO_UART_RTS_OUT, GPIO_UART_CTS_IN,
             CONFIG_EXAMPLE_HCI_UART_BAUDRATE, BLE5_ADV_STATE);
}
