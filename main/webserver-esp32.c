#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <esp_http_server.h>
#include "driver/gpio.h"
#include "esp_spiffs.h"
#include "wifi_config_portal.h"

#define ESP_WIFI_SSID      "ESP32_WebServer"   // Tên WiFi ESP32 phát ra
#define ESP_WIFI_PASS      "12345678"          // Mật khẩu (tối thiểu 8 ký tự)
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       4                   // Số thiết bị kết nối cùng lúc

#define LED_GPIO           2                   // LED onboard hầu hết ESP32

static const char *TAG = "ap_webserver";

// Trang HTML đẹp (nhúng thẳng vào firmware)
// static const char index_html[] = 
// "<!DOCTYPE html><html lang=\"vi\">"
// "<head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
// "<title>ESP32 Web Control</title>"
// "<style>"
// "body{font-family:Arial;background:linear-gradient(135deg,#667eea,#764ba2);color:white;text-align:center;padding:50px;}"
// "h1{font-size:3em;margin-bottom:20px;}"
// ".container{max-width:600px;margin:auto;background:rgba(255,255,255,0.1);padding:30px;border-radius:20px;}"
// ".btn{display:inline-block;padding:20px 40px;margin:15px;font-size:24px;border:none;border-radius:15px;cursor:pointer;transition:0.3s;}"
// ".on{background:#4CAF50;}"
// ".off{background:#f44336;}"
// ".btn:active{transform:scale(0.95);}"
// "#state{font-size:2em;margin:30px;}"
// "</style></head>"
// "<body>"
// "<div class=\"container\">"
// "<h1>ESP32 Web Server</h1>"
// "<p>Trạng thái LED: <span id=\"state\">Đang tải...</span></p>"
// "<button class=\"btn on\" onclick=\"send('/on')\">BẬT LED</button>"
// "<button class=\"btn off\" onclick=\"send('/off')\">TẮT LED</button>"
// "</div>"
// "<script>"
// "function send(cmd){fetch(cmd).then(()=>updateState());}"
// "function updateState(){"
// "  fetch('/state').then(r=>r.text()).then(s=>{document.getElementById('state').innerText=s;});"
// "}"
// // Cập nhật trạng thái mỗi 2 giây
// "setInterval(updateState,2000); updateState();"
// "</script>"
// "</body></html>";

#define INDEX_HTML_PATH "/spiffs/index.html"

char index_html[20*1024];
static uint8_t led_state = 0;
// Khởi tạo SPIFFS và nạp trang HTML từ hệ thống file
static void init_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    memset((void *)index_html, 0, sizeof(index_html));
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st))
    {
        ESP_LOGE(TAG, "index.html not found");
        return;
    }

    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open index.html");
        return;
    }
    if (fread(index_html, st.st_size, 1, fp) == 0)
    {
        ESP_LOGE(TAG, "fread failed");
    }
    fclose(fp);
}

/* Handler trang chủ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* Bật LED */
static esp_err_t led_on_handler(httpd_req_t *req)
{
    gpio_set_level(LED_GPIO, 1);
    led_state = 1;
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Tắt LED */
static esp_err_t led_off_handler(httpd_req_t *req)
{
    gpio_set_level(LED_GPIO, 0);
    led_state = 0;
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Trả về trạng thái hiện tại */
static esp_err_t state_handler(httpd_req_t *req)
{
    const char *state = led_state ? "BẬT" : "TẮT";
    
    ESP_LOGI(TAG, "LED state requested: %s", state);
    httpd_resp_send(req, state, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    // Đăng ký các URI
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t on = {
        .uri       = "/on",
        .method    = HTTP_GET,
        .handler   = led_on_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t off = {
        .uri       = "/off",
        .method    = HTTP_GET,
        .handler   = led_off_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t state = {
        .uri       = "/state",
        .method    = HTTP_GET,
        .handler   = state_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &on);
        httpd_register_uri_handler(server, &off);
        httpd_register_uri_handler(server, &state);
        wifi_portal_register_handlers(server); // Đăng ký các API WiFi portal chỉ ở đây
        ESP_LOGI(TAG, "Web server started");
    }
    return server;
}

static void wifi_init_apsta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
        // .sta sẽ được cấu hình sau khi user nhập thông tin
    };
    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP+STA started");
    ESP_LOGI(TAG, "SSID: %s  Password: %s", ESP_WIFI_SSID, ESP_WIFI_PASS);
    ESP_LOGI(TAG, "Mở trình duyệt → gõ: 192.168.4.1");
}

static void init_led(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(LED_GPIO, GPIO_FLOATING);
    gpio_set_level(LED_GPIO, 0);
    led_state = 0;
}

void app_main(void)
{
    // Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    // LED + Web server
    init_led();
    wifi_init_apsta();
    init_web_page_buffer();
    httpd_handle_t server = start_webserver();
    // KHÔNG gọi wifi_portal_register_handlers(server) ở đây nữa
}