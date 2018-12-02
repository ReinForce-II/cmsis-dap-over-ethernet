#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "wifi.h"

#include "DAP_Config.h"
#include "DAP.h"

#undef __ESP_FILE__
#define __ESP_FILE__ ""

#define LOG_TAG "CMSIS-DAP"

#define CONFIG_WIFI_SSID "FOO"
#define CONFIG_WIFI_PASSWORD "BAR"

#define PORT 2222

void dap_thread(void *argument);
TaskHandle_t h_dap_thread;
void recv_thread(void *argument);
TaskHandle_t h_recv_thread;
void send_thread(void *argument);
TaskHandle_t h_send_thread;

uint32_t flag_connected = 0;

xQueueHandle request_queue = NULL;
xQueueHandle response_queue = NULL;

int server_fd, client_fd;

/**
 *  main loop, accept connection from client, and allocate queues
 *  @param  param   <not used>
 *  @return <never return>
 */
_Noreturn void main_loop(void *param) {
    uint16_t port = PORT;
    struct sockaddr_in server, client;
    int err;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        ESP_LOGE(LOG_TAG, "Could not create socket");
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = htonl(INADDR_ANY);

    err = bind(server_fd, (struct sockaddr *)&server, sizeof(server));
    if (err < 0)
        ESP_LOGE(LOG_TAG, "Could not bind socket");

    err = listen(server_fd, 128);
    if (err < 0)
        ESP_LOGE(LOG_TAG, "Could not listen on socket");

    ESP_LOGI(LOG_TAG, "Server is listening on %d", port);

    ESP_LOGI(LOG_TAG, "main_loop thread ready");

    while (1) {
        socklen_t client_len = sizeof(client);
        client_fd = accept(server_fd, (struct sockaddr *)&client, &client_len);
        uint32_t client_addr = ntohl(client.sin_addr.s_addr);
        ESP_LOGI(
            LOG_TAG, "Accept conn from %u.%u.%u.%u",
            (client_addr & 0xff000000) >> 24, (client_addr & 0x00ff0000) >> 16,
            (client_addr & 0x0000ff00) >> 8, (client_addr & 0x000000ff) >> 0);

        if (client_fd < 0)
            ESP_LOGE(LOG_TAG, "Could not establish new connection");

        flag_connected = 1;
        request_queue = xQueueCreate(DAP_PACKET_COUNT, DAP_PACKET_SIZE);
        response_queue = xQueueCreate(DAP_PACKET_COUNT, DAP_PACKET_SIZE);

        while (flag_connected) {
            vTaskDelay(1);
        }

        vQueueDelete(request_queue);
        vQueueDelete(response_queue);
        request_queue = NULL;
        response_queue = NULL;
        close(client_fd);
    }
}

/**
 *  dap thread, process commands
 *  @param  argument    <not used>
 *  @return <never return>
 */
_Noreturn void dap_thread(void *argument) {
    int rc;
    (void)argument;
    uint8_t buffer_in[DAP_PACKET_SIZE];
    uint8_t buffer_out[DAP_PACKET_SIZE];
    ESP_LOGI(LOG_TAG, "dap_thread thread ready");

    while (1) {
        if (request_queue && response_queue &&
            uxQueueMessagesWaiting(request_queue) > 0 &&
            uxQueueSpacesAvailable(response_queue) > 0) {
            rc = xQueueReceive(request_queue, buffer_in, 0);
            if (rc != pdTRUE) {
                ESP_LOGE(LOG_TAG, "Unknown error when pop request queue item");
                continue;
            }
            if (buffer_in[0] == ID_DAP_QueueCommands) {
                buffer_in[0] = ID_DAP_ExecuteCommands;
            }
            ESP_LOGD(LOG_TAG, "Execute Cmd Start");
            // memcpy(buffer_out, buffer_in, DAP_PACKET_SIZE);
            DAP_ExecuteCommand(buffer_in, buffer_out);
            ESP_LOGD(LOG_TAG, "Execute Cmd End");
            rc = xQueueSend(response_queue, buffer_out, 0);
            if (rc != pdTRUE) {
                ESP_LOGE(LOG_TAG,
                         "Unknown error when push response queue item");
                continue;
            }
        }
    }
}

/**
 *  response send thread
 *  @param  param   pointer to client socket
 *  @return <never return>
 */
_Noreturn void send_thread(void *param) {
    int *socket = (int *)param;
    int rc;
    uint8_t buffer_in[DAP_PACKET_SIZE];
    ESP_LOGI(LOG_TAG, "send_thread thread ready");

    while (1) {
        if (response_queue && uxQueueMessagesWaiting(response_queue) > 0 &&
            flag_connected) {
            rc = xQueueReceive(response_queue, buffer_in, 0);
            if (rc != pdTRUE) {
                ESP_LOGE(LOG_TAG, "Unknown error when pop response queue item");
                continue;
            }
            ESP_LOGD(LOG_TAG, "Response Sending");
            rc = send(*socket, buffer_in, DAP_PACKET_SIZE, 0);
            if (rc < 0) {
                ESP_LOGE(LOG_TAG, "Client write failed");
            } else {
                ESP_LOGD(LOG_TAG, "Response Sent");
            }
        } else {
            vTaskDelay(1);
        }
    }
}

/**
 *  request recv thread
 *  @param  param   pointer to client socket
 *  @return <never return>
 */
_Noreturn void recv_thread(void *param) {
    int *socket = (int *)param; // process in
    int bytes_read;
    int rc;
    uint8_t buffer_in[DAP_PACKET_SIZE];
    uint32_t buffer_pos = 0;
    ESP_LOGI(LOG_TAG, "recv_thread thread ready");

    while (1) {
        if (request_queue && uxQueueSpacesAvailable(request_queue) > 0 &&
            flag_connected) {
            ESP_LOGD(LOG_TAG, "Request Receiving");
            bytes_read = recv(*socket, buffer_in + buffer_pos,
                              DAP_PACKET_SIZE - buffer_pos, 0);

            if (bytes_read == 0) {
                ESP_LOGI(LOG_TAG, "Disconnected");
                flag_connected = 0;
            } else if (bytes_read < 0) {
                ESP_LOGE(LOG_TAG, "Client read failed");
            } else {
                ESP_LOGD(LOG_TAG, "Request Received");
                buffer_pos += bytes_read;
                if (buffer_pos == DAP_PACKET_SIZE) {
                    buffer_pos = 0;
                    rc = xQueueSend(request_queue, buffer_in, 0);
                    if (rc != pdTRUE) {
                        ESP_LOGE(LOG_TAG,
                                 "Unknown error when push request queue item");
                        continue;
                    }
                }
            }
        } else {
            vTaskDelay(1);
        }
    }
}

#include "driver/gpio.h"

/**
 *  app main, initialize environments, start threads
 *  @param
 *  @return
 */
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    wait_for_ip();

    xTaskCreate(main_loop, "main_loop", 2048, 0, 0, 0);
    xTaskCreate(dap_thread, "dap_thread", 2048, 0, 0, &h_dap_thread);

    xTaskCreate(recv_thread, "recv_thread", 2048, &client_fd, 0,
                &h_recv_thread);
    xTaskCreate(send_thread, "send_thread", 2048, &client_fd, 0,
                &h_send_thread);

    gpio_config_t io_conf;
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO15/16
    io_conf.pin_bit_mask = 1ULL << 16;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);
    // gpio_set_level(16, 0);
}
