/* ===========================================================================
 * Zenith edge 设备固件样例（ESP-IDF v5.x，ESP32 系列）
 * 协议：TCP + COBS+CRC32 帧 + JSON 载荷（与 edge_esp32 插件对齐）
 *   上行：hello / hb（心跳）/ resp（RPC 应答）/ event
 *   下行：rpc（内置演示：led / ping / echo）
 * 烧录：idf.py build flash monitor（CONFIG 里填 WiFi 与网关地址）
 * 依赖：仅 ESP-IDF 自带组件（lwip / FreeRTOS），无三方库。
 * =========================================================================== */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define GATEWAY_IP "192.168.1.10" /* 运行 Zenith edge 网关的主机 */
#define GATEWAY_PORT 7788
#define DEVICE_ID "esp32-demo-1"
#define LED_GPIO 2

static const char* TAG = "zenith-dev";

/* ---- WiFi 连接（精简 station 模式；完整版用 esp_wifi + event loop） ---- */
extern void app_wifi_init(const char* ssid, const char* pass); // 用户侧实现

/* ---- CRC32（与 zenith::io::crc32 相同：反射 0xEDB88320） ---- */
static uint32_t crc32_calc(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ---- COBS 编码（输出不含分隔符；调用方追加 0x00） ---- */
static size_t cobs_encode(const uint8_t* in, size_t n, uint8_t* out) {
    size_t o = 0, block = 0, run = 0;
    out[o++] = 1; /* 占位 */
    block = 0;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] == 0) {
            out[block] = (uint8_t)(run + 1);
            block = o;
            out[o++] = 1;
            run = 0;
        } else {
            out[o++] = in[i];
            if (++run == 254) {
                out[block] = 255;
                block = o;
                out[o++] = 1;
                run = 0;
            }
        }
    }
    out[block] = (uint8_t)(run + 1);
    return o;
}

/* ---- 帧发送：payload(+crc) → COBS → 0x00 ---- */
static void send_frame(int sock, const char* json) {
    uint8_t body[512];
    size_t jl = strlen(json);
    if (jl + 4 > sizeof(body)) return;
    memcpy(body, json, jl);
    const uint32_t crc = crc32_calc((const uint8_t*)json, jl);
    memcpy(body + jl, &crc, 4);

    uint8_t wire[600];
    const size_t n = cobs_encode(body, jl + 4, wire);
    wire[n] = 0x00;
    send(sock, wire, n + 1, 0);
}

/* ---- RPC 处理（演示：led / ping / echo） ---- */
static void handle_rpc(int sock, const char* json) {
    /* 极简字段抽取（rid / method / on） */
    const char* p = strstr(json, "\"rid\":");
    int rid = p ? atoi(p + 6) : 0;
    char method[32] = {0};
    if ((p = strstr(json, "\"method\":\"")) != NULL) {
        const char* q = strchr(p + 10, '"');
        size_t len = q ? (size_t)(q - (p + 10)) : 0;
        if (len < sizeof(method)) memcpy(method, p + 10, len);
    }

    char resp[256];
    if (strcmp(method, "led") == 0) {
        const bool on = strstr(json, "\"on\":true") != NULL;
        gpio_set_level(LED_GPIO, on ? 1 : 0);
        snprintf(resp, sizeof(resp),
                 "{\"t\":\"resp\",\"rid\":%d,\"ok\":true,\"data\":{\"led\":%s}}",
                 rid, on ? "true" : "false");
    } else if (strcmp(method, "ping") == 0) {
        snprintf(resp, sizeof(resp), "{\"t\":\"resp\",\"rid\":%d,\"ok\":true,\"data\":{\"ms\":%lld}}",
                 rid, (long long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    } else {
        snprintf(resp, sizeof(resp), "{\"t\":\"resp\",\"rid\":%d,\"ok\":false,\"data\":{\"error\":\"unknown\"}}", rid);
    }
    send_frame(sock, resp);
}

/* ---- COBS 解码 + CRC 校验 ---- */
static size_t cobs_decode(const uint8_t* in, size_t n, uint8_t* out) {
    size_t o = 0, i = 0;
    while (i < n) {
        const uint8_t code = in[i++];
        if (code == 0) return 0;
        const size_t take = code - 1;
        if (i + take > n) return 0;
        memcpy(out + o, in + i, take);
        o += take;
        i += take;
        if (i < n && code != 255) out[o++] = 0;
    }
    return o;
}

static void device_task(void* arg) {
    (void)arg;
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    char rxbuf[1024];
    size_t rxlen = 0;
    uint8_t decoded[768];

    for (;;) {
        const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(GATEWAY_PORT)};
        inet_pton(AF_INET, GATEWAY_IP, &dst.sin_addr);

        if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) != 0) {
            close(sock);
            ESP_LOGW(TAG, "gateway unreachable, retry");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "connected to gateway");
        send_frame(sock, "{\"t\":\"hello\",\"id\":\"" DEVICE_ID "\"}");

        uint32_t hbCount = 0;
        for (;;) {
            /* 心跳（5s） */
            char hb[128];
            snprintf(hb, sizeof(hb),
                     "{\"t\":\"hb\",\"id\":\"" DEVICE_ID "\",\"ms\":%u,\"c\":%u}",
                     (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS), ++hbCount);
            send_frame(sock, hb);

            for (int wait = 0; wait < 50; ++wait) { /* 5 秒内收 RPC */
                const int n = recv(sock, rxbuf + rxlen, sizeof(rxbuf) - rxlen, MSG_DONTWAIT);
                if (n > 0) {
                    rxlen += (size_t)n;
                    /* 按 0x00 拆帧 */
                    size_t start = 0;
                    for (size_t i = 0; i < rxlen; ++i) {
                        if (rxbuf[i] == 0) {
                            if (i > start) {
                                const size_t dlen =
                                    cobs_decode((const uint8_t*)(rxbuf + start), i - start, decoded);
                                if (dlen >= 5) {
                                    const size_t pl = dlen - 4;
                                    uint32_t crc = 0;
                                    memcpy(&crc, decoded + pl, 4);
                                    if (crc32_calc(decoded, pl) == crc) {
                                        decoded[pl] = 0;
                                        handle_rpc(sock, (const char*)decoded);
                                    }
                                }
                            }
                            start = i + 1;
                        }
                    }
                    rxlen -= start;
                    memmove(rxbuf, rxbuf + start, rxlen);
                } else if (n == 0) {
                    goto reconnect;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    reconnect:
        close(sock);
        ESP_LOGW(TAG, "disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void) {
    app_wifi_init(WIFI_SSID, WIFI_PASS); /* 阻塞至拿到 IP */
    xTaskCreate(device_task, "zenith_dev", 8192, NULL, 5, NULL);
}
