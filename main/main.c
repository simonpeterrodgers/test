#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "Sensor";
static const char *NAME = "Test1";
static const char *SERVER = "";

static const char *WIFI_PASSWORD = "";
static const char *WIFI_SSID = "";

static const uint64_t SLEEP_TIME_MS = 60 * 1000;

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static int retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  switch (event_id) {
    case WIFI_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      if (retry_num > 0) {
        ESP_LOGI(TAG, "Retry to connect to the AP");
        esp_wifi_connect();
        retry_num--;
      } else {
        ESP_LOGI(TAG, "Connect to the AP fail");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
      break;
    case IP_EVENT_STA_GOT_IP:
      ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
      ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
      retry_num = 5;
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      break;
  }
}

void wifi_connect(const char *ssid, const char *password, const char *name) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();

  s_wifi_event_group = xEventGroupCreate();
  if(s_wifi_event_group == NULL) {
      ESP_LOGI(TAG, "Failed to create event group.");
  }

  ESP_ERROR_CHECK(esp_netif_set_hostname(netif, name));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  wifi_config_t wifi_config = { .sta = {
     .listen_interval = 10,
  }};
  strcpy((char*) wifi_config.sta.ssid, ssid);
  strcpy((char*) wifi_config.sta.password, password);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  // MAX uses listen interval, MIN is every beacon
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM)); 

  xEventGroupWaitBits(s_wifi_event_group,
    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    /* xClearOnExit=*/ pdTRUE,
    /* xWaitForAllBits=*/ pdFALSE,
    /* xTicksToWait=*/ portMAX_DELAY);
}

static void setup_bat_read(adc_oneshot_unit_handle_t *adc_handle, adc_cali_handle_t *cali_handle) {
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_new_unit(
    &(adc_oneshot_unit_init_cfg_t) { .unit_id = ADC_UNIT_1, },
    adc_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_config_channel(
    *adc_handle,
    ADC_CHANNEL_0,
    &(adc_oneshot_chan_cfg_t) {
      .atten = ADC_ATTEN_DB_6,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    }));
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_cali_create_scheme_curve_fitting(
    &(adc_cali_curve_fitting_config_t) {
      .unit_id = ADC_UNIT_1,
      .atten = ADC_ATTEN_DB_6,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    },
    cali_handle));
}

static float read_battery(adc_oneshot_unit_handle_t adc_handle, adc_cali_handle_t cali_handle) {
  int adc_raw;
  int voltage;

  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &adc_raw));
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage));

  // 1M/1M Voltage divider
  return voltage * 2 / 1000.0;
}

static void deinit_bat(adc_oneshot_unit_handle_t adc_handle, adc_cali_handle_t cali_handle) {
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_del_unit(adc_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(adc_cali_delete_scheme_curve_fitting(cali_handle));
}

static void http_get(const char *url_format, ...) {
  va_list args;
  char url[256];

  va_start(args, url_format);
  vsnprintf(url, 256, url_format, args);
  va_end(args);

  ESP_LOGI(TAG, "Sending GET request: %s", url);
  esp_http_client_handle_t client = esp_http_client_init(&(esp_http_client_config_t) {
      .url = url,
    });
  esp_http_client_perform(client);
  esp_http_client_cleanup(client);
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());

  wifi_connect(WIFI_SSID, WIFI_PASSWORD, NAME);

  ESP_ERROR_CHECK(esp_pm_configure(&(esp_pm_config_t) {
    .max_freq_mhz = 80,
    .min_freq_mhz = 40,
    .light_sleep_enable = true
  }));

  int reset_reason = esp_reset_reason();
  http_get("http://%s/report?sensor=%s&reset=%d", SERVER, NAME, reset_reason);

  TickType_t last_wake_time = xTaskGetTickCount();
  for (;;) {
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    setup_bat_read(&adc_handle, &cali_handle);

    float bat_v = read_battery(adc_handle, cali_handle);
    http_get("http://%s/report?sensor=%s&bat_v=%.2f", SERVER, NAME, bat_v);

    deinit_bat(adc_handle, cali_handle);

    vTaskDelayUntil(&last_wake_time, SLEEP_TIME_MS / portTICK_PERIOD_MS);
  }
}
