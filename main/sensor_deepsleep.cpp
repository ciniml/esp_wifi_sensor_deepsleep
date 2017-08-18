/**
 * @file sensor_deepsleep.cpp
 * @brief Main module of sensor_deepsleep application
 */
// Copyright 2017 Kenta IDA
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

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
#include <esp_deep_sleep.h>
#include <esp32/ulp.h>

#include <soc/sens_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#include <soc/rtc.h>
#include <soc/i2c_reg.h>

#include <driver/adc.h>
#include <driver/rtc_io.h>

#include "tls_client.hpp"
#include "http_client.hpp"

// ULPの定義をインクルード
// ULPのプログラムのビルド時に生成される。
#include <ulp_main.h>

// ULPプログラムエリア
extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");

// ログ出力のタグ
static const char TAG[] = "APP";

using namespace std;
static TlsClient tls_client;					// TLS通信用のインスタンス
static HttpClient http_client(tls_client);		// HTTP通信用インスタンス
static EventGroupHandle_t event_wifi_got_ip;	// IPアドレス確定を通知するイベント

// HTTPレスポンスを解析するIHttpResponseReceiverの実装
class ResponseReceiver : public IHttpResponseReceiver
{
private:
	bool is_success;

	virtual int on_message_begin(const HttpClient&) { ESP_LOGI(TAG, "Message begin"); return 0; }
	virtual int on_message_complete(const HttpClient&) { ESP_LOGI(TAG, "Message end"); return 0; }
	virtual int on_header_complete(const HttpClient&, const HttpResponseInfo& info) { 
		ESP_LOGI(TAG, "Header end. status code = %d", info.status_code); 
		ESP_LOGI(TAG, "Content-length = 0x%x", info.content_length);
		this->is_success = info.status_code == 200;	// ステータスコードが200かどうかで成功・失敗を判定。
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

// WiFiを含むESP32全般のイベント・ハンドラ
esp_err_t wifi_event_handler(void* ctx, system_event_t* event) 
{
	switch (event->event_id)
	{
	case SYSTEM_EVENT_STA_GOT_IP: {	// IPアドレス取得できた
		xEventGroupSetBits(event_wifi_got_ip, 1);	// イベント通知
		break;
	}
	default:
		break;
	}
	return ESP_OK; 
}

// WiFi接続成功までのタイムアウト(Tick単位)
static constexpr TickType_t WIFI_CONNECTION_TIMEOUT_TICK = pdMS_TO_TICKS(CONFIG_WIFI_CONNECTION_TIMEOUT_MS);

/***
 * @brief WiFi周りを初期化し、アクセスポイントに接続する
 * @return 接続してIP取得できたらtrue。失敗したらfalse。
 */
static bool initialize_wifi()
{
	tcpip_adapter_init();

	event_wifi_got_ip = xEventGroupCreate();
	
	// イベント・ハンドラを設定
	ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, nullptr));

	// WiFiをSTAモード(アクセスポイントに接続するモード)で初期化
	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MODEM));
	// SSID/パスワードを設定してアクセスポイントに接続
	wifi_config_t wifi_config = { 0 };
	strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_WIFI_SSID);
	strcpy(reinterpret_cast<char*>(wifi_config.sta.password), CONFIG_WIFI_PASSWORD);
	wifi_config.sta.bssid_set = false;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_connect());

	// IPアドレスを取得するまで待つ
	return xEventGroupWaitBits(event_wifi_got_ip, 1, pdTRUE, pdTRUE, WIFI_CONNECTION_TIMEOUT_TICK) != 0;
}

// センサーの測定間隔[秒]
static constexpr uint32_t SENSOR_MEASUREMENTS_INTERVAL_S = CONFIG_MEASUREMENTS_INTERVAL_S;

// ULP(低消費電力コプロセッサ)起動する
static void start_ulp()
{
	// RTC側のGPIOを設定する。
	// GPIO_0 (RTC_GPIO_11) : DHT11のDATAピンに接続、データ通信用
	rtc_gpio_init(GPIO_NUM_0);
	rtc_gpio_set_level(GPIO_NUM_0, 1);
	rtc_gpio_pullup_en(GPIO_NUM_0);
	rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_OUTUT);
	// GPIO_4 (RTC_GPIO_10) : 消費電力測定のトリガ用
	rtc_gpio_init(GPIO_NUM_4);
	rtc_gpio_set_level(GPIO_NUM_4, 0);
	rtc_gpio_set_direction(GPIO_NUM_4, RTC_GPIO_MODE_OUTPUT_ONLY);
	
	// ULPで使う変数をクリア 
	// ULPのツールが生成するulp_main.hに定義されている
	ulp_state = 0;
	ulp_byte_index = 0;
	ulp_bit_index = 0;
	ulp_last_temperature = 0;
	ulp_last_humidity = 0;
	ulp_force_wakeup_counter = 0;
	memset(&ulp_sensor_data, 0, 5 * 4);
	
	// ULPのプログラムをロード
	ESP_ERROR_CHECK(ulp_load_binary(0, ulp_main_bin_start, (ulp_main_bin_end - ulp_main_bin_start)/sizeof(uint32_t)));
	
	// ULPの起動タイマをセンサの測定間隔に設定
	static_assert(SENSOR_MEASUREMENTS_INTERVAL_S > 0, "Measurement interval must be greater than 0");
	ulp_set_wakeup_period(0, SENSOR_MEASUREMENTS_INTERVAL_S * 1000000UL);
	//ulp_set_wakeup_period(0, 5000000UL);
	ESP_LOGI(TAG, "SENS_ULP_CP_SLEEP_CYC0_REG = %d", READ_PERI_REG(SENS_ULP_CP_SLEEP_CYC0_REG));
	// ULPの実行を開始
	ESP_ERROR_CHECK( ulp_run((&ulp_entry - RTC_SLOW_MEM) / sizeof(uint32_t)) );
}

extern "C" void app_main();

/**
 * @brief アプリケーションのエントリ・ポイント
 */
void app_main()
{
	
	// 不揮発性メモリを初期化
	ESP_ERROR_CHECK(nvs_flash_init());

	// 起動の要因を調べる
	if (esp_deep_sleep_get_wakeup_cause() == ESP_DEEP_SLEEP_WAKEUP_ULP) {	// ULPから起動された
		ESP_LOGI(TAG, "Waken up by the ULP.");
	
		uint8_t cert;
		tls_client.initialize(&cert, 0);

		uint8_t sensor_data_sum = 0;
		bool is_sensor_data_valid = false;
		for (size_t index = 0; index < 5; index++) {
			ESP_LOGI(TAG, "DATA[%d]: %08x", index, *(&ulp_sensor_data + index));
			if (index != 4) {
				sensor_data_sum += *(&ulp_sensor_data + index);
			}
			else {
				// 最終バイトはそれまでの各バイトの和になっているので、確認する。
				is_sensor_data_valid = sensor_data_sum == (*(&ulp_sensor_data + index) & 0xff);
			}
		}

		// センサデータが有効なら送信する。
		if (is_sensor_data_valid) {
			// WiFiを初期化する
			uint16_t temperature = *(&ulp_sensor_data + 2) & 0xff;
			uint16_t humidity = *(&ulp_sensor_data + 0) & 0xff;

			ESP_LOGI(TAG, "ULP debug = %x", ulp_debug);
			ESP_LOGI(TAG, "temperature: %d, prev: %d", temperature, ulp_last_temperature & 0xffff);
			ESP_LOGI(TAG, "humidity   : %d, prev: %d", humidity, ulp_last_humidity & 0xffff);
			if (!initialize_wifi()) {
				ESP_LOGE(TAG, "WiFi AP connection timed out.");
			}
			else {
				// センサ情報をホストに送信
				

				ResponseReceiver response_parser;
				unique_ptr<char> request_string(new char[2048]);
				sprintf(request_string.get(), "%s&tmp=%d&hum=%d",
					"/test?name=" CONFIG_SENSOR_NAME,
					temperature,
					humidity);

				if (http_client.get(CONFIG_TESTSERVER_ADDRESS, "443", request_string.get(), response_parser)) {
					if (!response_parser.get_is_success()) {
						ESP_LOGE(TAG, "Failed to send sensor data.");
					}
				}
			}
		}
		else {
			ESP_LOGE(TAG, "Sensor data is invalid");
		}
	}
	
	ESP_LOGI(TAG, "Entering to deepsleep.");
	
	// RTCの周辺回路の電源が強制的にオフにならないようにする。
	// 現状、I2C通信後にULPから起動するのに必要
	//esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

	ESP_LOGI(TAG, "RTC_CNTL_STATE0_REG = %08x", READ_PERI_REG(RTC_CNTL_STATE0_REG));

	// ULPを起動
	start_ulp();
	ESP_ERROR_CHECK( esp_deep_sleep_enable_ulp_wakeup() );	// Enable ULP wakeup.
	ESP_LOGI(TAG, "RTC_CNTL_STATE0_REG = %08x", READ_PERI_REG(RTC_CNTL_STATE0_REG));

	// deep sleepに移行
	esp_deep_sleep_start();

	// ここには到達しない
}
 