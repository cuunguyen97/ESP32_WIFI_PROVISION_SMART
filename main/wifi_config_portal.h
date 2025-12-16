#ifndef WIFI_CONFIG_PORTAL_H
#define WIFI_CONFIG_PORTAL_H

#include "esp_http_server.h"

void wifi_portal_register_handlers(httpd_handle_t server);

#endif // WIFI_CONFIG_PORTAL_H
