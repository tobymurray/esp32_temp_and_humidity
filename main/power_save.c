#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event_loop.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_event_loop.h"

#include "nvs_flash.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/apps/sntp.h"

#include "mqtt_client.h"

#include "driver/gpio.h"

#include "dht.h"

#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#include "cJSON.h"

#include "stepper.h"
#include "common.h"

/*set the ssid and password via "make menuconfig"*/
#define DEFAULT_SSID CONFIG_WIFI_SSID
#define DEFAULT_PWD CONFIG_WIFI_PASSWORD

#define DEFAULT_LISTEN_INTERVAL CONFIG_WIFI_LISTEN_INTERVAL

#if CONFIG_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

#define ESP_INTR_FLAG_DEFAULT 0

#define LOW 0
#define HIGH 1

static const char *TAG = "power_save";
static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

// Throttle sensor reads to avoid polling too frequently
const unsigned int MIN_SENSOR_READ_MILLIS = 2500;

int pins[4] = { GPIO_NUM_17, GPIO_NUM_5, GPIO_NUM_18, GPIO_NUM_19 };

typedef struct MqttMessage {
  char topic[128];
  char body[128];
  bool retained;
} MqttMessage;

MqttMessage mqttMessage;
TaskHandle_t stepperTask;

static void obtain_time(void);
static void initialize_sntp(void);


esp_mqtt_client_handle_t client;

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
	esp_mqtt_client_handle_t client = event->client;
	int msg_id;
	switch (event->event_id) {
	case MQTT_EVENT_CONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
		msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
		ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

		msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
		ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

		msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
		ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

		msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
		ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
		break;
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
		break;

	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
		ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_DATA:
		ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
		printf("DATA=%.*s\r\n", event->data_len, event->data);
		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
		break;
	default:
		ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		break;
	}
	return ESP_OK;
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch (event->event_id) {
	case SYSTEM_EVENT_STA_START:
		ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
		ESP_ERROR_CHECK(esp_wifi_connect());
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
		ESP_LOGI(TAG, "got IP:%s\n", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
		ESP_ERROR_CHECK(esp_wifi_connect());
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
	return ESP_OK;
}

/*init wifi as sta and set power save mode*/
static void wifi_power_save(void) {
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = {
			.sta = {
					.ssid = DEFAULT_SSID,
					.password =	DEFAULT_PWD,
					.listen_interval = DEFAULT_LISTEN_INTERVAL,
			},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

	ESP_LOGI(TAG, "start the Wi-Fi SSID:[%s]", CONFIG_WIFI_SSID);
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "Waiting for wifi");
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

	ESP_LOGI(TAG, "esp_wifi_set_ps().");
	esp_wifi_set_ps(DEFAULT_PS_MODE);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
        // .user_context = (void *)your_context
    };

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

/*
 * Didn't look too closely at what this stuff does, copied it from the power_save example
 */
void start_up_stuff() {
	// Initialize NVS
	esp_err_t storage_init = nvs_flash_init();
	if (storage_init == ESP_ERR_NVS_NO_FREE_PAGES || storage_init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		storage_init = nvs_flash_init();
	}
	ESP_ERROR_CHECK(storage_init);

#if CONFIG_PM_ENABLE
	// Configure dynamic frequency scaling:
	// maximum and minimum frequencies are set in sdkconfig,
	// automatic light sleep is enabled if tickless idle support is enabled.
	esp_pm_config_esp32_t pm_config = {
			.max_freq_mhz =	CONFIG_EXAMPLE_MAX_CPU_FREQ_MHZ,
			.min_freq_mhz =	CONFIG_EXAMPLE_MIN_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
			.light_sleep_enable = true
#endif
			};
	ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif // CONFIG_PM_ENABLE
}

static void obtain_time(void) {
	ESP_LOGI(TAG, "Waiting in 'obtain_time' for Wi-Fi to be connected");
	EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
	if ((bits & CONNECTED_BIT) == 0) {
		ESP_LOGE(TAG, "Wi-Fi is not connected, failed to obtain_time");
	} else {
		ESP_LOGI(TAG, "    Wi-Fi is connected!");
	}
	initialize_sntp();

	// wait for time to be set
	time_t now = 0;
	struct tm timeinfo = { 0 };
	int retry = 0;
	const int retry_count = 10;
	while (timeinfo.tm_year < (2019 - 1900) && ++retry < retry_count) {
		ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		time(&now);
		localtime_r(&now, &timeinfo);
	}

	// Set timezone to Eastern Standard Time and print local time
	setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
	tzset();
}

static void initialize_sntp(void) {
	ESP_LOGI(TAG, "Initializing SNTP");
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "ca.pool.ntp.org");
	sntp_init();
}

void get_time(struct tm * timeinfo) {
	time_t now;
	time(&now);
	localtime_r(&now, timeinfo);
	// Is time set? If not, tm_year will be (1970 - 1900).
	if (timeinfo->tm_year < (2019 - 1900)) {
		ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
		obtain_time();
		// update 'now' variable with current time
		time(&now);
	}

	localtime_r(&now, timeinfo);
}

void publish_mqtt_message(MqttMessage message) {
	esp_mqtt_client_publish(client, message.topic, message.body, 0, 1, message.retained);
}

// Task to be created.
void vTaskCode(void * pvParameters) {
	unsigned long currentMillis;
	unsigned long lastRotation = 0;
	MqttMessage message;
	strncpy(message.topic, "/stepper", sizeof("/stepper"));
	message.retained = true;

	struct tm timeinfo;
	char strftime_buf[64];

	cJSON * root = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "status", cJSON_CreateString("initialized"));
	cJSON_AddItemToObject(root, "timestamp", cJSON_CreateString("initialized"));

	while (1) {
		currentMillis = millis();
		if (currentMillis - lastRotation >= 10000) {
			lastRotation = currentMillis;

			get_time(&timeinfo);
			strftime(strftime_buf, sizeof(strftime_buf), "%FT%T%Z", &timeinfo);
			cJSON_ReplaceItemInObject(root, "timestamp", cJSON_CreateString(strftime_buf));

			cJSON_ReplaceItemInObject(root, "status", cJSON_CreateString("turning"));
			cJSON_PrintPreallocated(root, message.body, 128, false);
			publish_mqtt_message(message);

			rotate(pins);

			strncpy(message.topic, "/stepper", sizeof("/stepper"));
			cJSON_ReplaceItemInObject(root, "status", cJSON_CreateString("stopped"));
			cJSON_PrintPreallocated(root, message.body, 128, false);
			publish_mqtt_message(message);
			delay(10 * 1000);
			ESP_LOGI(TAG, "Delay over");
		}
	}
}

char * task_state_to_string(eTaskState taskState) {
	switch (taskState) {
	case eRunning: /*!< A task is querying the state of itself, so must be running. */
		return "running";
	case eReady: /*!< The task being queried is in a read or pending ready list. */
		return "ready";
	case eBlocked: /*!< The task being queried is in the Blocked state. */
		return "blocked";
	case eSuspended: /*!< The task being queried is in the Suspended state, or is in the Blocked state with an infinite time out. */
		return "suspended";
	case eDeleted: /*!< The task being queried has been deleted, but its TCB has not yet been freed. */
		return "deleted";
	}

	return "UNKNOWN STATE!";
}

void app_main() {
	ESP_LOGI(TAG, "[APP] Startup..");
	ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
	ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

	esp_log_level_set("*", ESP_LOG_INFO);
	esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
	esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
	esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
	esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
	esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

	start_up_stuff();
	wifi_power_save();
	mqtt_app_start();

	ESP_LOGI(TAG, "Everything is all set up.");

	struct tm timeinfo;

	set_up(pins);

	// Invoke initially to set up SNTP
	get_time(&timeinfo);
	char strftime_buf[64];

	char measurement[6];

	unsigned long currentMillis;
	unsigned long lastSensorReadMillis = 0;
	unsigned long lastMemoryReport = 0;
	unsigned long lastTaskReport = 0;

	xTaskCreate(vTaskCode, "ROTATE_EGGS", 8192, NULL, tskIDLE_PRIORITY, &stepperTask);

	while (1) {
		currentMillis = millis();
		if (currentMillis - lastSensorReadMillis >= MIN_SENSOR_READ_MILLIS) {
			lastSensorReadMillis = currentMillis;
			get_time(&timeinfo);
			ESP_LOGI(TAG, "The current date/time in New York is: %s", strftime_buf);

			Reading reading = readPin((uint8_t) 27);
			if (reading.status != -2) {
				cJSON * root = cJSON_CreateObject();
				strftime(strftime_buf, sizeof(strftime_buf), "%FT%T%Z", &timeinfo);
				cJSON_AddItemToObject(root, "timestamp", cJSON_CreateString(strftime_buf));

				strncpy(mqttMessage.topic, "/humidity", sizeof("/humidity"));
				snprintf(measurement, 6, "%.2f", reading.humidity);
				cJSON_AddItemToObject(root, "relative_humidity", cJSON_CreateString(measurement));
				cJSON_PrintPreallocated(root, mqttMessage.body, 128, false);
				cJSON_DeleteItemFromObject(root, "relative_humidity");
				publish_mqtt_message(mqttMessage);

				strncpy(mqttMessage.topic, "/temperature", sizeof("/temperature"));
				snprintf(measurement, 6, "%.2f", reading.temperature);
				cJSON_AddItemToObject(root, "temperature", cJSON_CreateString(measurement));
				cJSON_PrintPreallocated(root, mqttMessage.body, 128, false);
				publish_mqtt_message(mqttMessage);

				cJSON_Delete(root);
				ESP_LOGI(TAG, "Reading: Status %d Humidity: %f Temperature: %f", reading.status, reading.humidity, reading.temperature);
			}
		}

		if (currentMillis - lastMemoryReport >= 60000) {
			lastMemoryReport = currentMillis;
			strncpy(mqttMessage.topic, "/free_heap", sizeof("/free_heap"));
			sprintf(mqttMessage.body, "%u", xPortGetFreeHeapSize());

			publish_mqtt_message(mqttMessage);
		}

		if (currentMillis - lastTaskReport >= 60000) {
			lastTaskReport = currentMillis;
			eTaskState stepperTaskState = eTaskGetState(stepperTask);
			ESP_LOGI(TAG, "The task state is: %s", task_state_to_string(stepperTaskState));
		}

		TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
		TIMERG0.wdt_feed = 1;
		TIMERG0.wdt_wprotect = 0;
	}


}
