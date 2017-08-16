#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>
#include <map>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_system.h>
#include <esp_event.h>
#include <esp_event_loop.h>
#include <esp_spi_flash.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_wifi.h>

#include "tls_client.hpp"
#include "http_client.hpp"

static const char TAG[] = "APP";

using namespace std;
static TlsClient tls_client;
static HttpClient http_client(tls_client);
static EventGroupHandle_t event_wifi_got_ip;

class ResponseReceiver : public IHttpResponseReceiver
{
private:
	bool is_success;

	virtual int on_message_begin(const HttpClient&) { ESP_LOGI(TAG, "Message begin"); return 0; }
	virtual int on_message_complete(const HttpClient&) { ESP_LOGI(TAG, "Message end"); return 0; }
	virtual int on_header_complete(const HttpClient&, const HttpResponseInfo& info) { 
		ESP_LOGI(TAG, "Header end. status code = %d", info.status_code); 
		ESP_LOGI(TAG, "Content-length = 0x%x", info.content_length);
		this->is_success = info.status_code == 200;	// Check status code to determinse the request has been completed successfully or not.
		return 0; 
	}
	virtual int on_header(const HttpClient&, const std::string& name, const std::string& value) {
		ESP_LOGI(TAG, "%s: %s", name.c_str(), value.c_str());
		return 0; 
	}
	virtual int on_body(const HttpClient&, const std::uint8_t* buffer, std::size_t length) {
		ESP_LOGI(TAG, "on_body: addr=%p, len=0x%x", buffer, length);
		return 0; 
	}
public:
	ResponseReceiver() : is_success(false) {}

	bool get_is_success() const { return this->is_success; }
};
esp_err_t wifi_event_handler(void* ctx, system_event_t* event) 
{
	switch (event->event_id)
	{
	case SYSTEM_EVENT_STA_GOT_IP: {
		xEventGroupSetBits(event_wifi_got_ip, 1);
		break;
	}
	default:
		break;
	}
	return ESP_OK; 
}

static void initialize_wifi()
{
	tcpip_adapter_init();

	event_wifi_got_ip = xEventGroupCreate();
	
	ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, nullptr));
	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	wifi_config_t wifi_config = { 0 };
	strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_WIFI_SSID);
	strcpy(reinterpret_cast<char*>(wifi_config.sta.password), CONFIG_WIFI_PASSWORD);
	wifi_config.sta.bssid_set = false;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_connect());

	xEventGroupWaitBits(event_wifi_got_ip, 1, pdTRUE, pdTRUE, portMAX_DELAY);
}

static volatile int32_t die_temp_sensor_value = 0;

extern "C" void app_main();

void app_main()
{
	system_event_t hoge;


	ESP_ERROR_CHECK(nvs_flash_init());

	uint8_t cert;
	tls_client.initialize(&cert, 0);

	initialize_wifi();
	



		
	ResponseReceiver response_parser;
	unique_ptr<char> request_string(new char[2048]);
	while (true) {
		sprintf(request_string.get(), "%s&tmp=%d&hum=NaN",
			"/test?name=ESP32", //"/api/HttpTriggerCSharp1?code=vICHv/UvPxv4SlwHcp78rPlKK8Cm6Fz47GpbwtpCbjoa2zpa/A3LRw==&name=ESP32",
			die_temp_sensor_value);
			
		if (http_client.get(CONFIG_TESTSERVER_ADDRESS, "443", request_string.get(), response_parser)) {
			if (!response_parser.get_is_success()) {
				ESP_LOGE(TAG, "Failed to send sensor data.");
			}
		}
	}
}
