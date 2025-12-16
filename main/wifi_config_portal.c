#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "wifi_portal";

// WiFi scan result buffer
#define MAX_SCAN_RESULTS 20
static wifi_ap_record_t ap_info[MAX_SCAN_RESULTS];
static uint16_t ap_count = 0;

// WiFi credentials
static char saved_ssid[33] = "";
static char saved_pass[65] = "";
static bool wifi_connected = false;

// Scan WiFi and return JSON
static esp_err_t scan_handler(httpd_req_t *req) {
    ESP_LOGI(TAG,"scan_handler");

    // Đảm bảo WiFi ở mode APSTA
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA && mode != WIFI_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    ap_count = 0; // reset trước khi scan

    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_num(&ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_info);

    ESP_LOGI(TAG,"Ap_count: %d",ap_count);
    for (int i = 0; i < ap_count && i < MAX_SCAN_RESULTS; i++) {
        ESP_LOGI(TAG, "SSID %d: %s", i, ap_info[i].ssid);
    }

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count && i < MAX_SCAN_RESULTS; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_info[i].ssid);
        int level = 1;
        if (ap_info[i].rssi > -60) level = 4;
        else if (ap_info[i].rssi > -70) level = 3;
        else if (ap_info[i].rssi > -80) level = 2;
        cJSON_AddNumberToObject(item, "level", level);
        cJSON_AddNumberToObject(item, "dbm", ap_info[i].rssi);
        cJSON_AddItemToArray(root, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

// Receive SSID & password, try to connect
static esp_err_t connect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG,"connect_handler");
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[len] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (!ssid || !pass) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    strncpy(saved_ssid, ssid->valuestring, sizeof(saved_ssid)-1);
    strncpy(saved_pass, pass->valuestring, sizeof(saved_pass)-1);
    cJSON_Delete(root);
    // Chỉ cấu hình STA, giữ nguyên AP mode
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, saved_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, saved_pass, sizeof(wifi_config.sta.password));
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_APSTA); // Đảm bảo vẫn giữ AP
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
    // Đánh dấu đang chờ kết quả kết nối
    wifi_connected = false;
    httpd_resp_sendstr(req, "{\"result\":true}");
    ESP_LOGI(TAG, "Connecting to SSID: %s", saved_ssid);
    // Việc tắt AP sẽ được xử lý sau 10s hoặc khi nhấn nút restart ở phía client (cần thêm API riêng nếu muốn)
    return ESP_OK;
}

// Return WiFi status
static esp_err_t status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", saved_ssid);
    cJSON_AddBoolToObject(root, "connected", wifi_connected);
    // Optionally add IP address
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

// Thêm event handler cập nhật wifi_connected và tắt AP nếu cần
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected!");
        wifi_connected = true;
        // Sau 10s tắt AP
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Tắt AP mode sau 10s thành công!");
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi connect failed or disconnected");
        wifi_connected = false;
        // Giữ AP mode để user cấu hình lại
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
}

// Thêm API để tắt AP mode ngay khi client yêu cầu
static esp_err_t ap_off_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Tắt AP mode ngay theo yêu cầu client");
    esp_wifi_set_mode(WIFI_MODE_STA);
    httpd_resp_sendstr(req, "{\"result\":true}");
    return ESP_OK;
}

// Register URI handlers
void wifi_portal_register_handlers(httpd_handle_t server) {
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_handler,
        .user_ctx = NULL
    };
    httpd_uri_t connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_handler,
        .user_ctx = NULL
    };
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &connect);
    httpd_register_uri_handler(server, &status);
    httpd_uri_t apoff = {
        .uri = "/apoff",
        .method = HTTP_POST,
        .handler = ap_off_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &apoff);
    // Đăng ký event handler nếu chưa đăng ký
    static bool handler_registered = false;
    if (!handler_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        handler_registered = true;
    }
}

// Call wifi_portal_register_handlers(server) sau khi start_webserver
// Cập nhật biến wifi_connected trong event handler WIFI_EVENT_STA_CONNECTED/DISCONNECTED
