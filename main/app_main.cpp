#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "rom/gpio.h"

extern "C" {
#include "app_esp32.h"

#include "app_wifi.h"
#include "app_mqtt.h"
#include "app_nvs.h"
}
#if CONFIG_MQTT_SWITCHES_NB
extern "C" {
#include "app_switch.h"
}
#endif //CONFIG_MQTT_SWITCHES_NB

#ifdef CONFIG_MQTT_SENSOR
extern "C" {
#include "app_sensors.h"
}
#endif //CONFIG_MQTT_SENSOR
extern "C" {
#include "app_thermostat.h"
}
#if CONFIG_MQTT_RELAYS_NB
extern "C" {
#include "app_relay.h"
}
QueueHandle_t relayQueue;
#endif//CONFIG_MQTT_RELAYS_NB

#ifdef CONFIG_MQTT_OTA
extern "C" {
#include "app_ota.h"
}
QueueHandle_t otaQueue;
#endif //CONFIG_MQTT_OTA

#ifdef CONFIG_MQTT_OPS
extern "C" {
#include "app_ops.h"
}
#endif // CONFIG_MQTT_OPS


extern "C" {
#include "app_smart_config.h"
}
QueueHandle_t smartconfigQueue;

extern "C" const char * smartconfigTAG;
extern "C" int smartconfigFlag;

extern "C" int targetTemperature;
extern "C" int targetTemperatureSensibility;
extern "C" const char * targetTemperatureTAG;
extern "C" const char * targetTemperatureSensibilityTAG;

extern "C" EventGroupHandle_t mqtt_event_group;
extern "C" const int MQTT_CONNECTED_BIT;
extern "C" EventGroupHandle_t wifi_event_group;
extern "C" const int WIFI_CONNECTED_BIT;


QueueHandle_t thermostatQueue;
QueueHandle_t mqttQueue;

static const char *TAG = "MQTT(S?)_MAIN";


void blink_task(void *pvParameter)
{
  /* Set the GPIO as a push/pull output */
  /* gpio_config_t io_conf; */
  /* io_conf.pin_bit_mask = (1ULL << CONFIG_MQTT_STATUS_LED_GPIO); */
  /* gpio_config(&io_conf); */

  gpio_pad_select_gpio(CONFIG_MQTT_STATUS_LED_GPIO);

  gpio_set_direction((gpio_num_t)CONFIG_MQTT_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);

  int interval;
  while(1) {
    EventBits_t bits;
    if(smartconfigFlag) {
      interval=150;
    } else {
      interval=250;
      bits = xEventGroupGetBits(wifi_event_group);
      if( ( bits & WIFI_CONNECTED_BIT ) != 0 ) {
        interval=500;
      }
    }
    gpio_set_level((gpio_num_t)CONFIG_MQTT_STATUS_LED_GPIO, LED_ON);

    bits = xEventGroupGetBits(mqtt_event_group);
    while ( bits & MQTT_CONNECTED_BIT ) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelay(interval / portTICK_PERIOD_MS);
    gpio_set_level((gpio_num_t)CONFIG_MQTT_STATUS_LED_GPIO, LED_OFF);
    vTaskDelay(interval / portTICK_PERIOD_MS);
  }
}

extern "C" void app_main()
{
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);


  mqtt_event_group = xEventGroupCreate();
  wifi_event_group = xEventGroupCreate();

  thermostatQueue = xQueueCreate(1, sizeof(struct ThermostatMessage) );
#if CONFIG_MQTT_RELAYS_NB
  relayQueue = xQueueCreate(32, sizeof(struct RelayMessage) );
  relays_init();
#endif //CONFIG_MQTT_RELAYS_NB


#ifdef CONFIG_MQTT_OTA
  otaQueue = xQueueCreate(1, sizeof(struct OtaMessage) );
#endif //CONFIG_MQTT_OTA
  mqttQueue = xQueueCreate(1, sizeof(void *) );

  xTaskCreate(blink_task, "blink_task", configMINIMAL_STACK_SIZE * 3, NULL, 3, NULL);

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK( err );

  ESP_LOGI(TAG, "nvs_flash_init done");

  err=read_thermostat_nvs(targetTemperatureTAG, &targetTemperature);
  ESP_ERROR_CHECK( err );

  err=read_thermostat_nvs(targetTemperatureSensibilityTAG, &targetTemperatureSensibility);
  ESP_ERROR_CHECK( err );

  smartconfigQueue = xQueueCreate(1, sizeof(int) );
  err=read_nvs_integer(smartconfigTAG, &smartconfigFlag);
  ESP_ERROR_CHECK( err );

  xTaskCreate(smartconfig_cmd_task, "smartconfig_cmd_task", configMINIMAL_STACK_SIZE * 3, (void *)NULL, 5, NULL);

  if (smartconfigFlag) {
    ESP_ERROR_CHECK(write_nvs_integer(smartconfigTAG, ! smartconfigFlag));
  } else {

    esp_mqtt_client_handle_t client = mqtt_init();

#ifdef CONFIG_MQTT_SENSOR
    xTaskCreate(sensors_read, "sensors_read", configMINIMAL_STACK_SIZE * 3, (void *)client, 10, NULL);
#endif //CONFIG_MQTT_SENSOR


#if CONFIG_MQTT_RELAYS_NB
    xTaskCreate(handle_relay_cmd_task, "handle_relay_cmd_task", configMINIMAL_STACK_SIZE * 3, (void *)client, 5, NULL);
#endif //CONFIG_MQTT_RELAYS_NB

#if CONFIG_MQTT_SWITCHES_NB
    gpio_switch_init(NULL);
#endif //CONFIG_MQTT_SWITCHES_NB

#ifdef CONFIG_MQTT_OTA
   xTaskCreate(handle_ota_update_task, "handle_ota_update_task", configMINIMAL_STACK_SIZE * 7, (void *)client, 5, NULL);
#endif //CONFIG_MQTT_OTA

  xTaskCreate(handle_thermostat_cmd_task, "handle_thermostat_cmd_task", configMINIMAL_STACK_SIZE * 3, (void *)client, 5, NULL);
    xTaskCreate(handle_mqtt_sub_pub, "handle_mqtt_sub_pub", configMINIMAL_STACK_SIZE * 3, (void *)client, 5, NULL);

    wifi_init();
    mqtt_start(client);
#ifdef CONFIG_MQTT_OPS
    xTaskCreate(ops_pub_task, "ops_pub_task", configMINIMAL_STACK_SIZE * 3, (void *)client, 5, NULL);
#endif // CONFIG_MQTT_OPS

  }
}
