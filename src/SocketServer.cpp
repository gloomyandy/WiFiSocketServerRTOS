/*
 * SocketServer.cpp
 *
 *  Created on: 25 Mar 2017
 *      Author: David
 */

#include "ecv.h"
#undef yield
#undef array
#undef out

#include <cstring>
#include <algorithm>

extern "C"
{
	#include "esp_task_wdt.h"
	#include "lwip/stats.h"			// for stats_display()
}


#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if SUPPORT_ETHERNET
#include "esp_eth.h"
#if !OLD_SDK
#include "esp_eth_phy_lan87xx.h"
#endif
#endif
#include "esp_event.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_partition.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include "led_indicator.h"

#include "rom/ets_sys.h"
#include "driver/gpio.h"

#include "DNSServer.h"
#include "SocketServer.h"
#include "Config.h"
#include "PooledStrings.h"
#include "HSPI.h"
#include "WirelessConfigurationMgr.h"
#if SUPPORTS_TLS
#include "TlsServer.h"
#endif

#include "include/MessageFormats.h"
#include "Listener.h"
#include "Connection.h"
#include "Misc.h"
#include "Config.h"

#ifdef ESP8266
#include "esp8266/spi.h"
#include "esp8266/gpio.h"
#include "esp8266/rom_functions.h"
#include "esp8266/partition.h"

extern esp_rom_spiflash_chip_t g_rom_flashchip;
#include "priv/esp_spi_flash_raw.h"
#else
#include "esp32/spi.h"
#include "esp_flash.h"
#endif

#if OLD_SDK
#include "esp_wpa2.h"
#else
#include "esp_eap_client.h"
#endif

#if ESP8266
const ModuleType moduleType = ModuleType::esp8266;
#elif CONFIG_IDF_TARGET_ESP32C3
const ModuleType moduleType = ModuleType::esp32c3;
#elif CONFIG_IDF_TARGET_ESP32S3
const ModuleType moduleType = ModuleType::esp32s3;
#elif CONFIG_IDF_TARGET_ESP32
# if SUPPORT_ETHERNET
const ModuleType moduleType = ModuleType::esp32eth;
# else
const ModuleType moduleType = ModuleType::esp32;
# endif
#elif CONFIG_IDF_TARGET_ESP32C5
const ModuleType moduleType = ModuleType::esp32c5;
#else
#error "unknown target chip"
#endif


static const uint32_t MaxConnectTime = 40 * 1000;			// how long we wait for WiFi to connect in milliseconds
static const uint32_t TransferReadyTimeout = 10;			// how many milliseconds we allow for the Duet to set
													// TransferReady low after the end of a transaction,
													// before we assume that we missed seeing it
#define array _ecv_array

static const uint32_t StatusReportMillis = 200;
static const int DefaultWiFiChannel = 6;

static const int MaxAPConnections = 4;

static uint32_t numWifiReconnects = 0;
static bool usingDhcpc = false;

// Global data
static uint32_t flashSize = 0;
#if OLD_SDK
static tcpip_adapter_ip_info_t staIpInfo;
#else
static esp_netif_ip_info_t staIpInfo;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
#if SUPPORT_ETHERNET
static esp_netif_t *eth_netif = NULL;
#endif
#endif
static volatile int currentSsid = -1;

#if ESP8266
static_assert(HostNameLength <= CONFIG_TCPIP_ADAPTER_HOSTNAME_MAX_LENGTH);
#else
static_assert(HostNameLength <= CONFIG_ESP_NETIF_HOSTNAME_MAX_LENGTH);
#endif
static char webHostName[HostNameLength + 1] = "Duet-WiFi";

#ifdef DEBUG
static NetworkCommand lastCommand = NetworkCommand::nullCommand;
static uint32_t commandsProcessed = 0;
#endif

static DNSServer dns;

static volatile const char* lastError = nullptr;
static volatile const char* prevLastError = nullptr;
static volatile WiFiState currentState = WiFiState::idle,
				lastReportedState = WiFiState::disabled;

static HSPIClass hspi;
static uint32_t transferBuffer[NumDwords(MaxDataLength + 1)];

static TaskHandle_t mainTaskHdl;
static TaskHandle_t connPollTaskHdl;
static TimerHandle_t tfrReqExpTmr;

#ifdef ESP8266
// ESP8266 has no clean way for the application to be the sole feeder of the hardware task WDT:
// esp_internal_idle_hook (port.c) unconditionally calls esp_task_wdt_reset() from the idle task,
// so as long as idle runs, the WDT is fed regardless of whether main is wedged. Fall back to a
// software heartbeat checked by the FreeRTOS timer service task. Tradeoff: the timer task is
// itself software and could in principle also be wedged, but it sits on a different scheduling
// path from the main task so the common failure mode (main blocked, other tasks fine) is caught
static volatile uint32_t mainLoopHeartbeat = 0;
static uint32_t mainLoopHeartbeatLastSeen = 0;
static TimerHandle_t mainLoopWdtTmr = nullptr;
#endif

static const char* WIFI_EVENT_EXT = "wifi_event_ext";

static WirelessConfigurationMgr *wirelessConfigMgr;

static led_indicator_handle_t led;
constexpr led_indicator_blink_type_t ONBOARD_LED_RESET = BLINK_FACTORY_RESET;
constexpr led_indicator_blink_type_t ONBOARD_LED_IO = BLINK_IO;
constexpr led_indicator_blink_type_t ONBOARD_LED_CONNECTING = BLINK_PROVISIONING;
constexpr led_indicator_blink_type_t ONBOARD_LED_CONNECTED = BLINK_CONNECTED;
constexpr led_indicator_blink_type_t ONBOARD_LED_IDLE = BLINK_PROVISIONED;

#if SUPPORT_ETHERNET
enum class EthState : uint8_t
{
	disabled = 0,					// Hardware not yet initialised
	idle = 1,						// Hardware initialised but not connected
	started = 2,
	connected = 4,
};

static esp_eth_handle_t ethHandle = NULL;
static esp_eth_netif_glue_handle_t ethNetifGlue = NULL;
static EthState  ethState = EthState::disabled;
static const char * ethSSID = "ethernet";

static struct {
	const char *name;
	const uint32_t mode;
} ethModes[] = {{"ethernet", 0b111},
				{"ethernet-10h", 0b000},
				{"ethernet-10f", 0b001},
				{"ethernet-100h", 0b010},
				{"ethernet-100f", 0b011}};
#endif
// Workaround for https://github.com/espressif/esp-idf/issues/12315, remove once issue is fixed.
// To summarize: Initial connection attempt on some access points fail when the
// previous connection was not severed cleanly, for example when power is removed
// suddenly or a board reset without calling M552 S0.
static int firstConnectWorkaroundStage= 0;

typedef enum {
	WIFI_IDLE = 0,
	STATION_CONNECTING,
	STATION_WRONG_PASSWORD,
	STATION_NO_AP_FOUND,
	STATION_CONNECT_TIMEOUT,
	STATION_CONNECT_FAIL,
	STATION_GOT_IP,
	AP_STARTED,
} wifi_evt_t;

typedef enum {
	WIFI_SCAN_IDLE,
	WIFI_SCANNING,
	WIFI_SCAN_DONE
} wifi_scan_state_t;


typedef enum {
	TFR_REQUEST = 1,
	TFR_REQUEST_TIMEOUT = 2,
	SAM_TFR_READY = 4,
} main_task_evt_t;

typedef enum {
	WIFI_EVENT_STA_CONNECTING
} wifi_event_ext_t;

static volatile wifi_scan_state_t scanState = WIFI_SCAN_IDLE;
static wifi_ap_record_t *wifiScanAPs = nullptr;
static uint16_t wifiScanNum = 0;

// Signals that the station has finished starting after esp_wifi_start(), set from
// the WIFI_EVENT_STA_START handler and waited on before issuing a scan
static EventGroupHandle_t wifiEventGroup = nullptr;
static constexpr EventBits_t STA_STARTED_BIT = BIT0;
static constexpr uint32_t StaStartTimeoutMs = 5000;

// Reset to default settings
void FactoryReset()
{
	wirelessConfigMgr->Reset(true);
}

// Check socket number in range, returning true if yes. Otherwise, set lastError and return false;
bool ValidSocketNumber(uint8_t num)
{
	if (num < MaxConnections)
	{
		return true;
	}
	lastError = "socket number out of range";
	return false;
}

#ifdef DEBUG
static void StatePrintTask(void* data)
{
	while (true)
	{
		printf("----------------------diagnostics---------------------\n");
		printf("last_network_command: %u\n", static_cast<unsigned>(lastCommand));
		printf("network_commands_processed: %u\n", (unsigned)commandsProcessed);
		printf("uptime_ms: %lu\n", millis());
		printf("os_ticks: %u\n", (unsigned)xTaskGetTickCount());
		printf("free_heap: %u\n", (unsigned)esp_get_free_heap_size());
		printf("last_reset_reason: %u\n", (unsigned)esp_reset_reason());

		printf("wifi_state: %u\n", static_cast<unsigned>(currentState));

		if (currentState == WiFiState::connected || currentState == WiFiState::runningAsAccessPoint)
		{
#if OLD_SDK
					tcpip_adapter_ip_info_t ip_info;
#if SUPPORT_ETHERNET
					tcpip_adapter_get_ip_info((currentState == WiFiState::connected) ? (ethState >= EthState::started ? TCPIP_ADAPTER_IF_ETH : TCPIP_ADAPTER_IF_STA) : TCPIP_ADAPTER_IF_AP, &ip_info);
#else
					tcpip_adapter_get_ip_info((currentState == WiFiState::connected) ? TCPIP_ADAPTER_IF_STA : TCPIP_ADAPTER_IF_AP, &ip_info);
#endif
#else
					esp_netif_ip_info_t ip_info;
#if SUPPORT_ETHERNET
					esp_netif_get_ip_info((currentState == WiFiState::connected) ? (ethState >= EthState::started ? eth_netif : sta_netif) : ap_netif, &ip_info);
#else
					esp_netif_get_ip_info((currentState == WiFiState::connected) ? sta_netif : ap_netif, &ip_info);
#endif
#endif
			uint8_t *ip = reinterpret_cast<uint8_t*>(&ip_info.ip.addr);
			printf("ip_addr: %u.%u.%u.%u\n", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);

			ip = reinterpret_cast<uint8_t*>(&ip_info.netmask.addr);
			printf("netmask: %u.%u.%u.%u\n", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);

			ip = reinterpret_cast<uint8_t*>(&ip_info.gw.addr);
			printf("gateway: %u.%u.%u.%u\n", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
		}

		uint16_t connected, otherEndClosed;
		Connection::GetSummarySocketStatus(connected, otherEndClosed);
		printf("connected_sockets: 0x%x other_end_closed_sockets: 0x%x\n", connected, otherEndClosed);
		Connection::ReportConnections();
		printf("------------------------------------------------------\n");
		vTaskDelay(pdMS_TO_TICKS(250));
	}
}
#endif

static inline bool isFirstConnectWorkaround()
{
	return firstConnectWorkaroundStage == 1 || firstConnectWorkaroundStage == 2;
}

// uxTaskGetStackHighWaterMark returns the lowest free stack ever seen on this task, in StackType_t
// units (4 B on Xtensa). Multiply for a bytes figure. A zero or near-zero reading means the task is
// one deep call away from corrupting adjacent memory.
//
// Label note: "appMain" is the FreeRTOS app_main task that runs loop() -> ProcessRequest +
// Connection::PollAll (so mbedTLS cert parse + ssl_read live here). "wifiEvt" is the
// WiFiConnectionTask that only services WiFi event-loop callbacks (idle most of the time)
static void PrintStackWatermarks(const char *tag)
{
	const unsigned wordSize = sizeof(StackType_t);
	const unsigned appMainW = mainTaskHdl     ? uxTaskGetStackHighWaterMark(mainTaskHdl)            : 0;
	const unsigned wifiEvtW = connPollTaskHdl ? uxTaskGetStackHighWaterMark(connPollTaskHdl)        : 0;
	const unsigned listenW  = Listener::GetTaskHandle()
								? uxTaskGetStackHighWaterMark(Listener::GetTaskHandle())            : 0;
	const unsigned dnsW     = dns.GetTaskHandle()
								? uxTaskGetStackHighWaterMark(dns.GetTaskHandle())                  : 0;
	ets_printf("stack [%s] appMain=%u(%uB) wifiEvt=%u(%uB) tcpListen=%u(%uB) dns=%u(%uB)\n",
		tag,
		appMainW, appMainW * wordSize,
		wifiEvtW, wifiEvtW * wordSize,
		listenW,  listenW  * wordSize,
		dnsW,     dnsW     * wordSize);
}

static void HandleWiFiEvent(void* arg, esp_event_base_t event_base,
								int32_t event_id, void* event_data)
{
	wifi_evt_t wifiEvt = WIFI_IDLE;

	debugPrintf("wifi evt: %s id: %d\n", event_base, event_id);

	if (event_base == WIFI_EVENT_EXT && event_id == WIFI_EVENT_STA_CONNECTING) {
		wifiEvt = STATION_CONNECTING;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
#if OLD_SDK
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, webHostName);
#else
		esp_netif_set_hostname(sta_netif, webHostName);
#endif
		if (!usingDhcpc)
		{
#if OLD_SDK
			tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
			tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &staIpInfo);
#else
			esp_netif_dhcpc_stop(sta_netif);
			esp_netif_set_ip_info(sta_netif, &staIpInfo);
#endif
		}
		// Disable the first connect workaround.
		if (firstConnectWorkaroundStage != 3) // Not previously disabled.
		{
			firstConnectWorkaroundStage = 3;
			debugPrint("first connect workaround disabled due to connect success\n");
		}
		return;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
		debugPrintf("disconnect reason: %d\n", disconnected->reason);
		switch (disconnected->reason) {
			// include authentication failures in general
			case WIFI_REASON_AUTH_EXPIRE:
			case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
			case WIFI_REASON_AUTH_FAIL:
			case WIFI_REASON_ASSOC_FAIL:
#if OLD_SDK
			case WIFI_REASON_ASSOC_EXPIRE:
#endif
			case WIFI_REASON_HANDSHAKE_TIMEOUT:
			case WIFI_REASON_802_1X_AUTH_FAILED:
				wifiEvt = STATION_WRONG_PASSWORD;
				break;
			case WIFI_REASON_NO_AP_FOUND:
			case WIFI_REASON_BEACON_TIMEOUT:
				wifiEvt = STATION_NO_AP_FOUND;
				break;
			case WIFI_REASON_ASSOC_LEAVE:
				wifiEvt = WIFI_IDLE;
				break;
			default:
				wifiEvt = STATION_CONNECT_FAIL;
				break;
		}
		if (disconnected->reason != WIFI_REASON_ASSOC_LEAVE)
		{
			// The disconnection was not triggered by an explicit disconnection command
			// by RRF, and will cause reconnection attempts. Count them here.
			numWifiReconnects++;
		}

		// The workaround has following stages:
		// 	0 - before first connection attempt
		// 	1 - first disconnect code
		// 	2 - second disconnect code
		// 	3 - workaround disabled
		// The goal is to get to 3, which is when the workaround is disabled.
		switch (firstConnectWorkaroundStage)
		{
		case 0:
			if (wifiEvt == STATION_WRONG_PASSWORD)
			{
				// 0 -> 1
				// On our first connect failure, check for the disconnect code that triggers workaround.
				// Different codes might be emitted, depending on access point:
				//  - WIFI_REASON_AUTH_EXPIRE (RPi, reported by rechrtb)
				//  - WIFI_REASON_ASSOC_FAIL (RPi, reported by rechrtb)
				//  - WIFI_REASON_AUTH_FAIL (TP-LINK Archer C6, reported by gloomyandy)
				//  - WIFI_REASON_ASSOC_EXPIRE (Google Pixel, reported by gloomyandy)
				// To be safe, the all disconnect code under STATION_WRONG_PASSWORD triggers this workaround.
				// This should trigger another connection attempt.
				firstConnectWorkaroundStage = 1;
				debugPrint("first connect workaround 1st stage\n");
			}
			else
			{
				// 0 -> 3
				// First connect workaround is not applicable due to different disconnect code.
				firstConnectWorkaroundStage = 3;
				debugPrint("first connect workaround disabled at stage 0\n");
			}
			break;
		case 1:
			if (disconnected->reason == WIFI_REASON_CONNECTION_FAIL)
			{
				// 1 -> 2
				// The connection attempt from stage 1 should fail with WIFI_REASON_CONNECTION_FAIL.
				// This should trigger another connection attempt.
				firstConnectWorkaroundStage = 2;
				wifiEvt = STATION_WRONG_PASSWORD;
				debugPrint("first connect workaround 2nd stage\n");
			}
			else
			{
				// 1 -> 3
				// The disconnect code is not what is expected for the workaround 2nd stage.
				// Disable workaround and let connection attempt fail with the original disconnect reason.
				firstConnectWorkaroundStage = 3;
				debugPrint("first connect workaround disabled at stage 1\n");
			}
			break;

		case 2:
			// 2 -> 3
		 	// Connection attempt at stage 2 fails.
			// Disable workaround and let connection attempt fail with the original disconnect reason.
			debugPrint("first connect workaround 2nd stage failed\n");
			firstConnectWorkaroundStage = 3;
			break;

		case 3:
			// Do nothing. Workaround is already disabled.
			break;

		default:
			break;
		}

	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		xEventGroupSetBits(wifiEventGroup, STA_STARTED_BIT);
		return; // do not send an event
	} else if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_STA_STOP || event_id == WIFI_EVENT_AP_STOP)) {
		wifiEvt = WIFI_IDLE;
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		wifiEvt = STATION_GOT_IP;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
		wifiEvt = AP_STARTED;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
		// only respond to scans initiated from networkStartScan, and not from client connect
		if (scanState == WIFI_SCANNING) {
			esp_wifi_scan_get_ap_num(&wifiScanNum);
			wifiScanAPs = (wifi_ap_record_t*) calloc(wifiScanNum, sizeof(wifi_ap_record_t));
			esp_wifi_scan_get_ap_records(&wifiScanNum, wifiScanAPs);
			scanState = WIFI_SCAN_DONE;
		}
		return; // do not send an event
	}

	xTaskNotify(connPollTaskHdl, wifiEvt, eSetValueWithOverwrite);
}

static void ConfigureSTAMode()
{
	esp_wifi_restore();
	esp_wifi_set_mode(WIFI_MODE_STA);
#if SUPPORT_5G
	wifi_protocols_t protocols;
	protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
	protocols.ghz_5g = WIFI_PROTOCOL_11A|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_11AC|WIFI_PROTOCOL_11AX;
	ESP_ERROR_CHECK(esp_wifi_set_protocols(WIFI_IF_STA, &protocols));
#else
# if ESP8266
	// Note: On ESP8266 builds the first call to esp_wifi_set_protocol seems to always fail. I've
	// no idea why. To avoid the issue on that device we set the options twice.
	esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
# endif
	esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N
# if defined(WIFI_PROTOCOL_11AX)
	| WIFI_PROTOCOL_11AX
#endif
					);
#endif
	esp_wifi_set_ps(WIFI_PS_NONE);
}

// Start the station and block until it is ready. esp_wifi_start() brings the station up
// asynchronously; the radio cannot scan until WIFI_EVENT_STA_START has fired. Scanning
// before then returns zero APs, so the caller must wait for the station to come up first.
static bool StartStation()
{
	xEventGroupClearBits(wifiEventGroup, STA_STARTED_BIT);
	if (esp_wifi_start() != ESP_OK)
	{
		return false;
	}
	return (xEventGroupWaitBits(wifiEventGroup, STA_STARTED_BIT, pdFALSE, pdTRUE,
								pdMS_TO_TICKS(StaStartTimeoutMs)) & STA_STARTED_BIT) != 0;
}

// Rebuild the mDNS services
void RebuildServices()
{
	static const char * const MdnsServiceStrings[3] = { "_http", "_ftp", "_telnet" };
	static const mdns_txt_item_t MdnsTxtRecords[2] = { {"version", VERSION_MAIN}, {"product", "DuetWiFi"}, };

	mdns_service_remove_all();
	mdns_hostname_set(webHostName);
	for (size_t protocol = 0; protocol < 3; protocol++)
	{
		const uint16_t port = Listener::GetPortByProtocol(protocol);
		if (port != 0)
		{
			mdns_service_add(webHostName, MdnsServiceStrings[protocol], "_tcp", port,
							(protocol == 0/*HttpProtocol*/) ? (mdns_txt_item_t*)MdnsTxtRecords : nullptr,
							(protocol == 0/*HttpProtocol*/) ? 2 : 0);
		}
	}
}

void RemoveMdnsServices()
{
	mdns_service_remove_all();
	mdns_free();
}

// Try to connect using the specified SSID and password
void ConnectToAccessPoint()
{
	esp_wifi_connect();
	esp_event_post(WIFI_EVENT_EXT, WIFI_EVENT_STA_CONNECTING, NULL, 0, portMAX_DELAY);
}

void WiFiConnectionTask(void* data)
{
	led_indicator_start(led, ONBOARD_LED_IDLE);

	TimerHandle_t connExpTmr = xTimerCreate("connExpTmr", MaxConnectTime, pdFALSE, NULL,
		[](TimerHandle_t data) {
			xTaskNotify(connPollTaskHdl, STATION_CONNECT_TIMEOUT, eSetBits);
		});

	while(true)
	{
		uint32_t event = 0;
		xTaskNotifyWait(0, UINT_MAX, &event, portMAX_DELAY);

		WiFiState prevCurrentState = currentState;

		bool connectErrorChanged = false;
		bool retry = false;

		switch (currentState)
		{
		case WiFiState::connecting:
		case WiFiState::reconnecting:
			{
				static char lastConnectError[100];
				const char *error = nullptr;
				// We are trying to connect or reconnect, so check for success or failure
				switch (event)
				{
				case WIFI_IDLE:
					currentState = WiFiState::idle;	// cancelled connection/reconnection
					break;

				case STATION_CONNECT_TIMEOUT:
					error = "Timed out";
					break;

				case STATION_WRONG_PASSWORD:
					retry = isFirstConnectWorkaround();
					error = retry ? nullptr : "Authentication failed";
					break;

				case STATION_NO_AP_FOUND:
					error = "Didn't find access point";
					retry = (currentState == WiFiState::reconnecting);
					break;

				case STATION_CONNECT_FAIL:
					error = "Failed";
					retry = (currentState == WiFiState::reconnecting);
					break;

				case STATION_GOT_IP:
					xTimerStop(connExpTmr, portMAX_DELAY);
					if (currentState == WiFiState::reconnecting)
					{
						lastError = "Reconnect succeeded";
					}
					debugPrint("Connected to AP\n");
					currentState = WiFiState::connected;
					break;

				case STATION_CONNECTING:
					// Do nothing
					break;

				default:
					error = "Unknown WiFi state";
					break;
				}

				if (error != nullptr)
				{
					strcpy(lastConnectError, error);
					SafeStrncat(lastConnectError, " while trying to connect to ", ARRAY_SIZE(lastConnectError));
					WirelessConfigurationData wp;
					wirelessConfigMgr->GetSsid(currentSsid, wp);
					SafeStrncat(lastConnectError, wp.ssid, ARRAY_SIZE(lastConnectError));
					lastError = error;
					debugPrint("Failed to connect to AP\n");

					if (!retry) {
						esp_wifi_stop();
					}
				}
			}
			break;

		case WiFiState::connected:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;							// disconnected/stopped Wi-Fi
			} else if (event == STATION_WRONG_PASSWORD ||
						event == STATION_NO_AP_FOUND ||
						event == STATION_CONNECT_FAIL)
			{
				currentState = WiFiState::autoReconnecting;
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the auto reconnect timer
				esp_wifi_connect();
				lastError = "Lost connection, auto reconnecting";
				debugPrint("Lost connection to AP\n");
				break;
			}
			break;

		case WiFiState::autoReconnecting:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;							// disconnected/stopped Wi-Fi
			} else if (event == STATION_GOT_IP) {
				xTimerStop(connExpTmr, portMAX_DELAY);
				lastError = "Auto reconnect succeeded";
				currentState = WiFiState::connected;
			} else if (event != STATION_CONNECTING) {
				if (event == STATION_CONNECT_TIMEOUT) {
					lastError = "Timed out trying to auto-reconnect";
				} else {
					lastError = "Auto reconnect failed, trying manual reconnect";
				}
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the reconnect timer
				retry = true;
			}
			break;

		case WiFiState::idle:
			if (event == AP_STARTED) {
				currentState = WiFiState::runningAsAccessPoint;
			} else if (event == STATION_CONNECTING) {
				currentState = WiFiState::connecting;
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the econnect timer
			}
			break;

		case WiFiState::runningAsAccessPoint:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;
			}

			break;
		default:
			break;
		}

		if (retry)
		{
			WirelessConfigurationData wp;
			wirelessConfigMgr->GetSsid(currentSsid, wp);
			currentState = isFirstConnectWorkaround() ? WiFiState::connecting : WiFiState::reconnecting;
			debugPrintf("Trying to reconnect to ssid \"%s\" with password \"%s\"\n", wp.ssid, wp.password);
			ConnectToAccessPoint();
		}

		if (currentState != prevCurrentState) {
			led_indicator_blink_type_t new_blink;

			if (currentState == WiFiState::autoReconnecting ||
				currentState == WiFiState::connecting ||
				currentState == WiFiState::reconnecting)
			{
				new_blink = ONBOARD_LED_CONNECTING;
			} else if (currentState == WiFiState::connected ||
				currentState == WiFiState::runningAsAccessPoint) {
				new_blink = ONBOARD_LED_CONNECTED;
			} else {
				new_blink = ONBOARD_LED_IDLE;
			}
			led_indicator_stop(led, ONBOARD_LED_IDLE);
			led_indicator_stop(led, ONBOARD_LED_CONNECTING);
			led_indicator_stop(led, ONBOARD_LED_CONNECTED);
			led_indicator_start(led, new_blink);
		}

		if (lastError != prevLastError || currentState != prevCurrentState ||
			connectErrorChanged)
		{
			xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
		}
	}
}

static void debugPrintNetwork(const char *desc, wifi_ap_record_t *apr)
{
	debugPrintfAlways("%s '%s' on channel=%d, rssi=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n", desc,
					apr->ssid, apr->primary, apr->rssi,
					apr->bssid[0], apr->bssid[1], apr->bssid[2], apr->bssid[3], apr->bssid[4], apr->bssid[5]);
}

int ScanForNetworks(const char *reqSsid, uint8_t mac[6], int8_t &channel, WirelessConfigurationData &wp)
{
	ConfigureSTAMode();
	if (!StartStation())
	{
		esp_wifi_stop();
		lastError = "failed to start WiFi";
		return -1;
	}
#if SUPPORT_5G
	ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#endif

	wifi_scan_config_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.show_hidden = true;

	cfg.ssid = (uint8_t*)reqSsid;

	esp_err_t res = esp_wifi_scan_start(&cfg, true);

	if (res != ESP_OK) {
		esp_wifi_stop();
		lastError = "network scan failed";
		return -1;
	}

	uint16_t num_ssids = 0;
	esp_wifi_scan_get_ap_num(&num_ssids);

	wifi_ap_record_t *ap_records = (wifi_ap_record_t*) calloc(num_ssids, sizeof(wifi_ap_record_t));

	esp_wifi_scan_get_ap_records(&num_ssids, ap_records);
	esp_wifi_stop();

	// Find the strongest network that we know about
	int32_t strongestNetwork = -1;
	int32_t strongest5GNetwork = -1;
	for (int32_t i = 0; i < num_ssids; ++i)
	{
		debugPrintNetwork("found network", &ap_records[i]);
		if (ap_records[i].primary >= MIN_5G_CHANNEL)
		{
			if (strongest5GNetwork < 0 || ap_records[i].rssi > ap_records[strongest5GNetwork].rssi)
			{
				WirelessConfigurationData temp;
				if (wirelessConfigMgr->GetSsid((const char*)ap_records[i].ssid, ap_records[i].primary, temp) > 0)
				{
					strongest5GNetwork = i;
				}
			}
		}
		else
		{
			if (strongestNetwork < 0 || ap_records[i].rssi > ap_records[strongestNetwork].rssi)
			{
				WirelessConfigurationData temp;
				if (wirelessConfigMgr->GetSsid((const char*)ap_records[i].ssid, ap_records[i].primary, temp) > 0)
				{
					strongestNetwork = i;
				}
			}
		}
	}

	char ssid[SsidLength + 1] = { 0 };
	if (strongestNetwork >= 0)
	{
		debugPrintNetwork("strongest 2G network", &ap_records[strongestNetwork]);
	}
	if (strongest5GNetwork >= 0)
	{
		debugPrintNetwork("strongest 5G network", &ap_records[strongest5GNetwork]);
		if (strongestNetwork < 0 || ap_records[strongest5GNetwork].rssi > MIN_5G_THRESHOLD || ap_records[strongest5GNetwork].rssi >= ap_records[strongestNetwork].rssi)
		{
			strongestNetwork = strongest5GNetwork;
		}
	}

	if (strongestNetwork >= 0) {
		SafeStrncpy(ssid, (const char*)ap_records[strongestNetwork].ssid,
					std::min(sizeof(ssid), sizeof(ap_records[strongestNetwork].ssid)));
		debugPrintNetwork("selected network", &ap_records[strongestNetwork]);
		memcpy(mac, ap_records[strongestNetwork].bssid, sizeof(ap_records[strongestNetwork].bssid));
		channel = ap_records[strongestNetwork].primary;
	}

	free(ap_records);

	if (strongestNetwork < 0)
	{
		return -1;
	}

	return wirelessConfigMgr->GetSsid(ssid, 0, wp);
}


void StartClient(const char * array ssid)
pre(currentState == WiFiState::idle)
{
	mdns_init();

	WirelessConfigurationData wp;
	esp_wifi_stop();

	int8_t channel = -1;
	wifi_config_t wifi_config;
	memset(&wifi_config, 0, sizeof(wifi_config));

	int ssidIdx = ScanForNetworks(ssid, wifi_config.sta.bssid, channel, wp);

	if (ssidIdx <= 0)
	{
		if (ssid != nullptr)
		{
			static char requestedNetworkError[SsidLength + 40];
			SafeStrncpy(requestedNetworkError, "requested network '", ARRAY_SIZE(requestedNetworkError));
			SafeStrncat(requestedNetworkError, ssid, ARRAY_SIZE(requestedNetworkError));
			SafeStrncat(requestedNetworkError, "' not found", ARRAY_SIZE(requestedNetworkError));
			lastError = requestedNetworkError;
		}
		else
		{
			lastError = "no known networks found";
		}
		return;
	}

	currentSsid = ssidIdx;
	ConfigureSTAMode();

	SafeStrncpy((char*)wifi_config.sta.ssid, (char*)wp.ssid,
		std::min(sizeof(wifi_config.sta.ssid), sizeof(wp.ssid)));

#ifndef ESP8266
#if OLD_SDK
	if (channel >= 0 && channel <= 13)
	{
		wifi_config.sta.channel = channel;
	}
	else
	{
		wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	}
#else
	wifi_config.sta.channel = channel;
	wifi_config.sta.bssid_set = true;
#endif
#else
	// Workaround for ESP8266, which seems to ignore the channel argument,
	// instead preferring to connect to the previously connected to channel.
	wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
#endif

	if (wp.eap.protocol == EAPProtocol::NONE)
	{
		SafeStrncpy((char*)wifi_config.sta.password, (char*)wp.password,
			std::min(sizeof(wifi_config.sta.password), sizeof(wp.password)));
	}

	esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

	// Clear all credentials, even if requested network is not WPA2-Enterprise.
	// Without this, connection to the same WPA2-Enterprise network with
	// PSK credentials will succeed.
#if OLD_SDK
	esp_wifi_sta_wpa2_ent_disable();
	esp_wifi_sta_wpa2_ent_clear_identity();
	esp_wifi_sta_wpa2_ent_clear_ca_cert();
	esp_wifi_sta_wpa2_ent_clear_cert_key();
	esp_wifi_sta_wpa2_ent_clear_username();
	esp_wifi_sta_wpa2_ent_clear_password();
#ifndef ESP8266
	esp_wifi_sta_wpa2_ent_clear_new_password();
#endif
#else
	esp_wifi_sta_enterprise_disable();
	esp_eap_client_clear_identity();
	esp_eap_client_clear_ca_cert();
	esp_eap_client_clear_certificate_and_key();
	esp_eap_client_clear_username();
	esp_eap_client_clear_password();

#ifndef ESP8266
	esp_eap_client_clear_new_password();
#endif
#endif
	if (wp.eap.protocol != EAPProtocol::NONE)
	{
		CredentialsInfo offsets;
		CredentialsInfo &sizes = wp.eap.credSizes;

		const uint8_t* base = wirelessConfigMgr->GetEnterpriseCredentials(currentSsid, sizes, offsets);

		if (base == nullptr)
		{
			lastError = "Failed to load credentials";
			return;
		}

		if (sizes.asMemb.anonymousId)
		{
#if OLD_SDK
			esp_wifi_sta_wpa2_ent_set_identity(base + offsets.asMemb.anonymousId, sizes.asMemb.anonymousId);
#else
			esp_eap_client_set_identity(base + offsets.asMemb.anonymousId, sizes.asMemb.anonymousId);
#endif
		}

		if (sizes.asMemb.caCert)
		{
#if OLD_SDK
			esp_wifi_sta_wpa2_ent_set_ca_cert(base + offsets.asMemb.caCert, sizes.asMemb.caCert);
#else
			esp_eap_client_set_ca_cert(base + offsets.asMemb.caCert, sizes.asMemb.caCert);
#endif
		}

		if (wp.eap.protocol == EAPProtocol::EAP_TLS)
		{
			const uint8_t *privateKeyPswd = nullptr;
			if (sizes.asMemb.tls.privateKeyPswd)
			{
				privateKeyPswd = base + offsets.asMemb.tls.privateKeyPswd;
			}
#if OLD_SDK
			esp_wifi_sta_wpa2_ent_set_cert_key(base + offsets.asMemb.tls.userCert, sizes.asMemb.tls.userCert,
											base + offsets.asMemb.tls.privateKey, sizes.asMemb.tls.privateKey,
											privateKeyPswd, sizes.asMemb.tls.privateKeyPswd);
#else
			esp_eap_client_set_certificate_and_key(base + offsets.asMemb.tls.userCert, sizes.asMemb.tls.userCert,
											base + offsets.asMemb.tls.privateKey, sizes.asMemb.tls.privateKey,
											privateKeyPswd, sizes.asMemb.tls.privateKeyPswd);
#endif
		}
		else if (wp.eap.protocol == EAPProtocol::EAP_PEAP_MSCHAPV2 || wp.eap.protocol == EAPProtocol::EAP_TTLS_MSCHAPV2)
		{
#if OLD_SDK
			esp_wifi_sta_wpa2_ent_set_username(base + offsets.asMemb.peapttls.identity, sizes.asMemb.peapttls.identity);
			esp_wifi_sta_wpa2_ent_set_password(base + offsets.asMemb.peapttls.password, sizes.asMemb.peapttls.password);
#else
			esp_eap_client_set_username(base + offsets.asMemb.peapttls.identity, sizes.asMemb.peapttls.identity);
			esp_eap_client_set_password(base + offsets.asMemb.peapttls.password, sizes.asMemb.peapttls.password);
#endif
#ifndef ESP8266
			if (wp.eap.protocol == EAPProtocol::EAP_TTLS_MSCHAPV2)
			{
#if OLD_SDK
				esp_wifi_sta_wpa2_ent_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
#else
				esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
#endif
			}
#endif
		}
		else
		{
			lastError = "Invalid 802.1x protocol";
			return;
		}

#if OLD_SDK
		esp_wifi_sta_wpa2_ent_enable();
#else
		esp_wifi_sta_enterprise_enable();
#endif
	}

	memset(&staIpInfo, 0, sizeof(staIpInfo));

	// On Arduino core, gateway and subnet is ignored
	// if IP address is not specified.
	if (wp.ip)
	{
		usingDhcpc = false;
		staIpInfo.ip.addr = wp.ip;
		staIpInfo.gw.addr = wp.gateway;

		if(!wp.netmask) {
			IP4_ADDR(&staIpInfo.netmask, 255, 255, 255, 0); // default to 255.255.255.0
		} else {
			staIpInfo.netmask.addr = wp.netmask;
		}
	}
	else
	{
		usingDhcpc = true;
#if OLD_SDK
		tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
#else
		esp_netif_dhcpc_start(sta_netif);
#endif
	}

	if (!StartStation())
	{
		esp_wifi_stop();
		lastError = "failed to start WiFi";
		return;
	}
#if SUPPORT_5G
	ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#endif

	// ssidData contains the details of the strongest known access point
	debugPrintf("Trying to connect to ssid \"%s\" with password \"%s\"\n", wp.ssid, wp.password);
	debugPrintfAlways("using channel=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
					wifi_config.sta.channel,
					wifi_config.sta.bssid[0], wifi_config.sta.bssid[1], wifi_config.sta.bssid[2],
					wifi_config.sta.bssid[3], wifi_config.sta.bssid[4], wifi_config.sta.bssid[5]);

	ConnectToAccessPoint();
}

bool CheckValidSSID(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == SsidLength)
		{
			return false;					// ESP8266 core requires strlen(ssid) <= 31
		}
	}
	return len != 0;
}

bool CheckValidPassword(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == PasswordLength)
		{
			return false;					// ESP8266 core requires strlen(password) <= 63
		}
	}
	return len == 0 || len >= 8;			// password must be empty or at least 8 characters (WPA2 restriction)
}

// Check that the access point data is valid
bool ValidApData(const WirelessConfigurationData &apData)
{
	// Check the IP address
	if (apData.ip == 0 || apData.ip == 0xFFFFFFFF)
	{
		return false;
	}

	// Check the channel. 0 means auto so it OK.
	if (apData.channel > 13)
	{
		return false;
	}

	return CheckValidSSID(apData.ssid) && CheckValidPassword(apData.password);
}

void StartAccessPoint()
{
	esp_wifi_stop();
	WirelessConfigurationData apData;
	if (wirelessConfigMgr->GetSsid(WirelessConfigurationMgr::AP, apData) && ValidApData(apData))
	{
		esp_wifi_restore();
		esp_err_t res = esp_wifi_set_mode(WIFI_MODE_AP);

		if (res == ESP_OK)
		{
			wifi_config_t wifi_config;
			memset(&wifi_config, 0, sizeof(wifi_config));
			SafeStrncpy((char*)wifi_config.ap.ssid, apData.ssid,
				std::min(sizeof(wifi_config.ap.ssid), sizeof(apData.ssid)));
			SafeStrncpy((char*)wifi_config.ap.password, (char*)apData.password,
				std::min(sizeof(wifi_config.ap.password), sizeof(apData.password)));
#if OLD_SDK
			wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
#else
			wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
#endif
			wifi_config.ap.channel = (apData.channel == 0) ? DefaultWiFiChannel : apData.channel;
			wifi_config.ap.max_connection = MaxAPConnections;

			res = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

			if (res == ESP_OK)
			{
#if OLD_SDK
				tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
				tcpip_adapter_ip_info_t ip_info;
#else
				esp_netif_dhcps_stop(ap_netif);
				esp_netif_ip_info_t ip_info;
#endif

				ip_info.ip.addr = apData.ip;
				ip_info.gw.addr = apData.ip;
				IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
#if OLD_SDK
				res = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
				tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
#else
				res = esp_netif_set_ip_info(ap_netif, &ip_info);
				esp_netif_dhcps_start(ap_netif);
#endif

				if (res == ESP_OK) {
					debugPrintf("Starting AP %s with password \"%s\"\n", apData.ssid, apData.password);
					currentSsid = WirelessConfigurationMgr::AP;
					res = esp_wifi_start();
#if SUPPORT_5G
					ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#endif

				}

				if (res != ESP_OK)
				{
					debugPrintAlways("Failed to start AP\n");
				}
			}
			else
			{
				debugPrintAlways("Failed to set AP config\n");
			}
		}
		else
		{
			debugPrintAlways("Failed to set AP mode\n");
		}

		if (res == ESP_OK)
		{
			debugPrintAlways("AP started\n");
			dns.setErrorReplyCode(DNSReplyCode::NoError);
			ip_addr_t addr;
			addr.u_addr.ip4.addr = apData.ip;
			if (!dns.start(53, "*", addr))
			{
				lastError = "Failed to start DNS\n";
				debugPrintf("%s\n", lastError);
			}
			mdns_init();
		}
		else
		{
			lastError = "Failed to start access point";
			debugPrintf("%s\n", lastError);
		}
	}
	else
	{
		lastError = "invalid access point configuration";
		debugPrintf("%s\n", lastError);
	}
}

#if SUPPORT_ETHERNET
/** Event handler for Ethernet events */
static void HandleEthEvent(void *arg, esp_event_base_t event_base,
							int32_t event_id, void *event_data)
{
	uint8_t mac_addr[6] = {0};
	/* we can get the ethernet driver handle from event data */
	esp_eth_handle_t ethHandle = *(esp_eth_handle_t *)event_data;
	switch (event_id) {
	case ETHERNET_EVENT_CONNECTED:
		mdns_init();
#if OLD_SDK
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_ETH, webHostName);
		if (usingDhcpc)
		{
			tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_ETH);
		}
		else
		{
			tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_ETH);
			tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_ETH, &staIpInfo);
		}
#else
		esp_netif_set_hostname(eth_netif, webHostName);
		if (usingDhcpc)
		{
			esp_netif_dhcpc_start(eth_netif);
		}
		else
		{
			esp_netif_dhcpc_stop(eth_netif);
			esp_netif_set_ip_info(eth_netif, &staIpInfo);
		}
#endif
		esp_eth_ioctl(ethHandle, ETH_CMD_G_MAC_ADDR, mac_addr);
		debugPrint("Ethernet Link Up\n");
		debugPrintf("Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
		break;
	case ETHERNET_EVENT_DISCONNECTED:
		debugPrint("Ethernet Link Down\n");
		if (usingDhcpc)
		{
#if OLD_SDK
			tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_ETH);
#else
			esp_netif_dhcpc_stop(eth_netif);
#endif
		}
		break;
	case ETHERNET_EVENT_START:
		debugPrint("Ethernet Started\n");
		ethState = EthState::started;
		break;
	case ETHERNET_EVENT_STOP:
		debugPrint("Ethernet Stopped\n");
		ethState = EthState::idle;
		currentState = WiFiState::idle;
		led_indicator_stop(led, ONBOARD_LED_CONNECTED);
		led_indicator_stop(led, ONBOARD_LED_CONNECTING);
		led_indicator_start(led, ONBOARD_LED_IDLE);
		// We don't seem to be able to uninstall the driver
		//ESP_ERROR_CHECK(esp_eth_driver_uninstall(ethHandle));
		xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
		break;
	default:
		break;
	}
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void GotEthIP(void *arg, esp_event_base_t event_base,
						int32_t event_id, void *event_data)
{
	ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
	const esp_netif_ip_info_t *ip_info = &event->ip_info;

	debugPrint("Ethernet Got IP Address\n");
	debugPrint("~~~~~~~~~~~\n");
	debugPrintf("ETHIP:" IPSTR, IP2STR(&ip_info->ip));
	debugPrintf("ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
	debugPrintf("ETHGW:" IPSTR, IP2STR(&ip_info->gw));
	debugPrint("~~~~~~~~~~~\n");
	currentState = WiFiState::connected;
	xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
	led_indicator_stop(led, ONBOARD_LED_IDLE);
	led_indicator_stop(led, ONBOARD_LED_CONNECTING);
	led_indicator_start(led, ONBOARD_LED_CONNECTED);
}

//Nasty hack to let us set the operating mode
extern uint32_t lan87xxOperatingMode;

void EthInit(uint32_t mode)
{
	esp_wifi_deinit();
	debugPrintf("Start eth init mode %x\n", mode);
	lan87xxOperatingMode = mode;
#if OLD_SDK
	ESP_ERROR_CHECK(tcpip_adapter_set_default_eth_handlers());
#else
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);
#endif
	debugPrintf("Current core is %x\n", xPortGetCoreID());
	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
	eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
	phy_config.phy_addr = 1;
	phy_config.reset_gpio_num = -1;
#if OLD_SDK
	mac_config.smi_mdc_gpio_num = 23;
	mac_config.smi_mdio_gpio_num = 18;
	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
	esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phy_config);
#else
	eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
	esp32_emac_config.smi_gpio.mdc_num = 23;
	esp32_emac_config.smi_gpio.mdio_num = 18;
	esp32_emac_config.dma_burst_len = ETH_DMA_BURST_LEN_16;
	esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
	esp32_emac_config.clock_config.rmii.clock_gpio = 17;
	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
	esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
#endif
	debugPrint("Install driver\n");
	esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
	ESP_ERROR_CHECK(esp_eth_driver_install(&config, &ethHandle));
#if !OLD_SDK
	ethNetifGlue = esp_eth_new_netif_glue(ethHandle);
	// Attach Ethernet driver to TCP/IP stack
	ESP_ERROR_CHECK(esp_netif_attach(eth_netif, ethNetifGlue));
#endif
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &HandleEthEvent, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &GotEthIP, NULL));
	ethState = EthState::idle;
}

void EthStartClient(int32_t mode)
pre(currentState == WiFiState::idle)
{
	led_indicator_stop(led, ONBOARD_LED_IDLE);
	led_indicator_start(led, ONBOARD_LED_CONNECTING);
	debugPrint("Starting ethernet\n");
	if (ethState == EthState::disabled)
	{
		EthInit(mode);
	}
	currentState = WiFiState::connecting;

	// Look to see if we have any ethernet specific IP configuration
	memset(&staIpInfo, 0, sizeof(staIpInfo));
	WirelessConfigurationData wp;
	int idx = wirelessConfigMgr->GetSsid(ethSSID, 0, wp);
	if (idx > 0)
	{
		debugPrintf("Found ethernet config in slot %d\n", idx);
	}
	if (idx > 0 && wp.ip)
	{
		usingDhcpc = false;
		staIpInfo.ip.addr = wp.ip;
		staIpInfo.gw.addr = wp.gateway;

		if(!wp.netmask) {
			IP4_ADDR(&staIpInfo.netmask, 255, 255, 255, 0); // default to 255.255.255.0
		} else {
			staIpInfo.netmask.addr = wp.netmask;
		}
	}
	else
	{
		usingDhcpc = true;
		esp_netif_dhcpc_stop(sta_netif);
		//tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_ETH);
	}
	ESP_ERROR_CHECK(esp_eth_start(ethHandle));
	//mdns_init();
	debugPrint("Ethernet start complete\n");
}

static int32_t EthGetMode(const char *target)
{
	for(size_t i = 0; i < ARRAY_SIZE(ethModes); i++)
	{
		if (strcmp(target, ethModes[i].name) == 0)
		{
			return ethModes[i].mode;
		}
	}
	return -1;
}

static uint32_t EthGetConnectionSpeed()
{
	uint32_t ret = 0;
	eth_speed_t speed = ETH_SPEED_100M;

	esp_eth_ioctl(ethHandle, ETH_CMD_G_SPEED, &speed);
	ret = static_cast<uint32_t>(speed == ETH_SPEED_10M ? EspWiFiPhyMode::ETH10F : EspWiFiPhyMode::ETH100F);
	eth_duplex_t duplex = ETH_DUPLEX_FULL;
	esp_eth_ioctl(ethHandle, ETH_CMD_G_DUPLEX_MODE, &duplex);
	ret = ret + (duplex != ETH_DUPLEX_FULL ? 1 : 0);
	return ret;
}

#endif

static union
{
	MessageHeaderSamToEsp hdr;			// the actual header
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderIn;

static union
{
	MessageHeaderEspToSam hdr;
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderOut;


// Send a response.
// 'response' is the number of byes of response if positive, or the error code if negative.
// Use only to respond to commands which don't include a data block, or when we don't want to read the data block.
void SendResponse(int32_t response)
{
	(void)hspi.transfer32(response);
	if (response > 0)
	{
		hspi.transferDwords(transferBuffer, nullptr, NumDwords((size_t)response));
	}
}

WiFiAuth EspAuthModeToWiFiAuth(wifi_auth_mode_t authmode)
{
	WiFiAuth res = WiFiAuth::UNKNOWN;

	switch (authmode)
	{
	case WIFI_AUTH_OPEN:
		res = WiFiAuth::OPEN;
		break;

	case WIFI_AUTH_WEP:
		res = WiFiAuth::WEP;
		break;

	case WIFI_AUTH_WPA_PSK:
		res = WiFiAuth::WPA_PSK;
		break;

	case WIFI_AUTH_WPA2_PSK:
		res = WiFiAuth::WPA2_PSK;
		break;

	case WIFI_AUTH_WPA_WPA2_PSK:
		res = WiFiAuth::WPA_WPA2_PSK;
		break;

	case WIFI_AUTH_WPA2_ENTERPRISE:
		res = WiFiAuth::WPA2_ENTERPRISE;
		break;

	case WIFI_AUTH_WPA3_PSK:
		res = WiFiAuth::WPA3_PSK;
		break;

	case WIFI_AUTH_WPA2_WPA3_PSK:
		res = WiFiAuth::WPA2_WPA3_PSK;
		break;

#ifndef ESP8266
	case WIFI_AUTH_WAPI_PSK:
		res = WiFiAuth::WAPI_PSK;
		break;
#endif

	default:
		break;
	}

	return res;
}

// This is called when the SAM is asking to transfer data
void ProcessRequest()
{
	// State for the multi-phase networkAddEnterpriseSsid command. Lives at function scope so
	// both the in-transaction switch and the deferred switch can read/update it across
	// successive SPI transactions
	static bool enterpriseAddPending = false;
	static int32_t enterpriseAddErr = ResponseEmpty;

	// Set up our own headers
	messageHeaderIn.hdr.formatVersion = InvalidFormatVersion;
	messageHeaderIn.hdr.command = NetworkCommand::nullCommand;
	messageHeaderOut.hdr.formatVersion = MyFormatVersion;
	/* When using a ST32 based main board we can sometimes see the first byte of an spi transfer be
	   set to zero. This may now be fixed by adjustments to the spi configuration. However just in case
	   we send a second signature word that is used by RRF on the ST32 to verify that the received 
	   packet looks valid even though the first byte may be incorrect.
	*/
	messageHeaderOut.hdr.dummy32 = 0xdeadbeef;
	messageHeaderOut.hdr.state = currentState;
	bool deferCommand = false;

#ifdef DEBUG
	lastCommand = NetworkCommand::nullCommand;
#endif

	// Begin the transaction
	gpio_set_level(SamSSPin, 0);		// assert CS to SAM
	hspi.beginTransaction();

	// Exchange headers, except for the last dword which will contain our response
	hspi.transferDwords(messageHeaderOut.asDwords, messageHeaderIn.asDwords, headerDwords - 1);

	if (messageHeaderIn.hdr.formatVersion != MyFormatVersion)
	{
		debugPrintf("Bad header wanted %x got %x cmd %d data len %d\n", MyFormatVersion, messageHeaderIn.hdr.formatVersion, messageHeaderIn.hdr.command, messageHeaderIn.hdr.dataLength);
		delay(10);
		debugPrintf("Bad header2 wanted %x got %x cmd %d data len %d\n", MyFormatVersion, messageHeaderIn.hdr.formatVersion, messageHeaderIn.hdr.command, messageHeaderIn.hdr.dataLength);
		SendResponse(ResponseBadRequestFormatVersion);
	}
	else if (messageHeaderIn.hdr.dataLength > MaxDataLength)
	{
		SendResponse(ResponseBadDataLength);
	}
	else
	{
		const size_t dataBufferAvailable = std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength);

#ifdef DEBUG
		lastCommand = messageHeaderIn.hdr.command;
		commandsProcessed++;
#endif

		// See what command we have received and take appropriate action
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::nullCommand:					// no command being sent, SAM just wants the network status
			SendResponse(ResponseEmpty);
			break;

		case NetworkCommand::networkStartClient:			// connect to an access point
			if (currentState == WiFiState::idle && scanState != WIFI_SCANNING)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				if (messageHeaderIn.hdr.dataLength != 0 && messageHeaderIn.hdr.dataLength <= SsidLength + 1)
				{
					hspi.transferDwords(nullptr, transferBuffer, NumDwords(messageHeaderIn.hdr.dataLength));
					reinterpret_cast<char *>(transferBuffer)[messageHeaderIn.hdr.dataLength] = 0;
				}
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			if (currentState == WiFiState::idle && scanState != WIFI_SCANNING)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			FactoryReset();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			deferCommand = true;
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			break;

		case NetworkCommand::networkGetStatus:				// get the network connection status
			{
				NetworkStatusResponse * const response = reinterpret_cast<NetworkStatusResponse*>(transferBuffer);
				memset(response, 0, sizeof(*response));

				response->flashSize = flashSize;
				SafeStrncpy(response->versionText, firmwareVersion, sizeof(response->versionText));
				response->moduleType = static_cast<int>(moduleType);
debugPrintf("set module type %d\n", response->moduleType);
				switch (esp_reset_reason())
				{
				case ESP_RST_POWERON:
					response->resetReason = 0; // Power-on
					break;
				case ESP_RST_WDT:
					response->resetReason = 1; // Hardware watchdog
					break;
				case ESP_RST_PANIC:
					response->resetReason = 2; // Exception
					break;
				case ESP_RST_TASK_WDT:
				case ESP_RST_INT_WDT:
					response->resetReason = 3; // Software watchdog
					break;
				case ESP_RST_SW:
#ifdef ESP8266
				case ESP_RST_FAST_SW:
#endif
					response->resetReason = 4; // Software-initiated reset
					break;
				case ESP_RST_DEEPSLEEP:
					response->resetReason = 5; // Wake from deep-sleep
					break;
				case ESP_RST_EXT:
					response->resetReason = 6; // External reset
					break;
				case ESP_RST_BROWNOUT:
					response->resetReason = 7; // Brownout
					break;
				case ESP_RST_SDIO:
					response->resetReason = 8; // SDIO
					break;
				case ESP_RST_UNKNOWN:
				default:
					response->resetReason = 9; // Out-of-range, translates to 'Unknown' in RRF
					break;
				}

				SafeStrncpy(response->hostName, webHostName, sizeof(response->hostName));

#ifdef ESP8266
				response->clockReg = REG(SPI_CLOCK(MSPI));
#else
				response->clockReg = SPI_LL_GET_HW(MSPI)->clock.val;
#endif

				wifi_ps_type_t ps = WIFI_PS_NONE;
				esp_wifi_get_ps(&ps);

				switch (ps)
				{
				case WIFI_PS_NONE:
					response->sleepMode = 1;
					break;
				case WIFI_PS_MIN_MODEM:
					response->sleepMode = 3;
					break;
				default:
					// sleepMode = 2 (light sleep) is not set by firmware.
					break;
				}

				const bool runningAsAp = (currentState == WiFiState::runningAsAccessPoint);
				const bool runningAsStation = (currentState == WiFiState::connected);

				response->rssi = INT8_MIN;
				response->numReconnects = numWifiReconnects;
				response->usingDhcpc = usingDhcpc;

				if (runningAsAp || runningAsStation)
				{
#if SUPPORT_ETHERNET
					if (ethState >= EthState::started)
					{
						esp_eth_ioctl(ethHandle, ETH_CMD_G_MAC_ADDR, response->macAddress);
					}
					else
#endif
					{
						esp_wifi_get_mac(runningAsStation ? WIFI_IF_STA : WIFI_IF_AP, response->macAddress);
					}
					if (runningAsStation)
					{
						wifi_ap_record_t ap_info;
						memset(&ap_info, 0, sizeof(ap_info));
#if SUPPORT_ETHERNET
						if (ethState >= EthState::started)
						{
							SafeStrncpy(response->ssid, ethSSID, strlen(ethSSID)+1);
							response->phyMode = EthGetConnectionSpeed();
						}
						else							
#endif
						{
							esp_wifi_sta_get_ap_info(&ap_info);
							response->rssi = ap_info.rssi;
							response->auth = EspAuthModeToWiFiAuth(ap_info.authmode);
							SafeStrncpy(response->ssid, (const char*)ap_info.ssid, sizeof(response->ssid));
							memcpy(reinterpret_cast<char*>(response->apMac), (const char*)ap_info.bssid, sizeof(response->apMac));
#if SUPPORT_5G
							if (ap_info.phy_11ax)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::AX);
							else if (ap_info.phy_11ac)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::AC);
							else if (ap_info.phy_11a)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::A);
							else
#endif
							if (ap_info.phy_11n)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::N);
							else if (ap_info.phy_11g)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::G);
							else if (ap_info.phy_11b)
								response->phyMode = static_cast<int>(EspWiFiPhyMode::B);
						}
					}
					else
					{
						wifi_sta_list_t sta_list;
						memset(&sta_list, 0, sizeof(sta_list));
						esp_wifi_ap_get_sta_list(&sta_list);
						response->numClients = sta_list.num;

						wifi_config_t ap_cfg;
						esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
						response->auth = EspAuthModeToWiFiAuth(ap_cfg.ap.authmode);
						SafeStrncpy(response->ssid, (const char*)ap_cfg.ap.ssid, sizeof(response->ssid));
#if SUPPORT_5G
						response->phyMode = static_cast<int>(EspWiFiPhyMode::AX);
#else
						response->phyMode = static_cast<int>(EspWiFiPhyMode::N);
#endif
					}
#if OLD_SDK
					tcpip_adapter_ip_info_t ip_info;
#if SUPPORT_ETHERNET
					tcpip_adapter_get_ip_info(runningAsStation ? (ethState >= EthState::started ? TCPIP_ADAPTER_IF_ETH : TCPIP_ADAPTER_IF_STA) : TCPIP_ADAPTER_IF_AP, &ip_info);
#else
					tcpip_adapter_get_ip_info(runningAsStation ? TCPIP_ADAPTER_IF_STA : TCPIP_ADAPTER_IF_AP, &ip_info);
#endif
#else
					esp_netif_ip_info_t ip_info;
#if SUPPORT_ETHERNET
					esp_netif_get_ip_info(runningAsStation ? (ethState >= EthState::started ? eth_netif : sta_netif) : ap_netif, &ip_info);
#else
					esp_netif_get_ip_info(runningAsStation ? sta_netif : ap_netif, &ip_info);
#endif
#endif
					response->ipAddress = ip_info.ip.addr;
					response->netmask = ip_info.netmask.addr;
					response->gateway = ip_info.gw.addr;
#if SUPPORT_ETHERNET
					if (ethState < EthState::started)
#endif
					{
						uint8_t pChan;
						wifi_second_chan_t sChan;
						esp_wifi_get_channel(&pChan, &sChan);
						if (pChan <= 15)
							response->channel = pChan;
						else
							response->channel5G = pChan;

						switch (sChan)
						{
						case WIFI_SECOND_CHAN_NONE:
							response->ht = static_cast<uint8_t>(HTMode::HT20);
							break;

						case WIFI_SECOND_CHAN_ABOVE:
							response->ht = static_cast<uint8_t>(HTMode::HT40_ABOVE);
							break;

						case WIFI_SECOND_CHAN_BELOW:
							response->ht = static_cast<uint8_t>(HTMode::HT40_BELOW);
							break;

						default:
							break;
						}
					}
				}

				response->freeHeap = esp_get_free_heap_size();

#ifdef ESP8266
				response->vcc = esp_wifi_get_vdd33();
#else
				response->vcc = 0;
#endif

				SendResponse(sizeof(NetworkStatusResponse));
			}
			break;

		case NetworkCommand::networkAddSsid:				// add to our known access point list
		case NetworkCommand::networkConfigureAccessPoint:	// configure our own access point details
			if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(sizeof(WirelessConfigurationData)));
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkAddEnterpriseSsid:		// add an enterprise access point
			{
				AddEnterpriseSsidFlag flag = static_cast<AddEnterpriseSsidFlag>(messageHeaderIn.hdr.flags);
				if (flag == AddEnterpriseSsidFlag::SSID) // add ssid info
				{
					if (!enterpriseAddPending)
					{
						if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
						{
							EAPProtocol protocol = static_cast<EAPProtocol>(hspi.transfer32(ResponseEmpty));

							if (protocol == EAPProtocol::EAP_TTLS_MSCHAPV2
								|| protocol == EAPProtocol::EAP_PEAP_MSCHAPV2
								|| protocol == EAPProtocol::EAP_TLS
								)
							{
								hspi.transferDwords(nullptr, transferBuffer, NumDwords(sizeof(WirelessConfigurationData)));
								WirelessConfigurationData *newSsid = reinterpret_cast<WirelessConfigurationData*>(transferBuffer);
								newSsid->eap.protocol = protocol;
								deferCommand = true;
							}
							else
							{
								enterpriseAddErr = ResponseBadParameter;
							}
						}
						else
						{
							SendResponse(ResponseBadDataLength);
						}
					}
					else
					{
						SendResponse(ResponseWrongState);
					}
				}
				else if (flag == AddEnterpriseSsidFlag::CREDENTIAL)
				{
					if (enterpriseAddPending)
					{
						messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
						memset(transferBuffer, 0, sizeof(transferBuffer));
						hspi.transferDwords(nullptr, transferBuffer, NumDwords(messageHeaderIn.hdr.dataLength));
						deferCommand = true;
					}
					else
					{
						if (enterpriseAddErr)
						{
							SendResponse(enterpriseAddErr);
							enterpriseAddErr = ResponseEmpty;
						}
						else
						{
							SendResponse(ResponseWrongState);
						}
					}
				}
				else if (flag == AddEnterpriseSsidFlag::COMMIT || flag == AddEnterpriseSsidFlag::CANCEL)
				{
					bool cancel = (flag == AddEnterpriseSsidFlag::CANCEL);

					if (cancel || enterpriseAddPending)
					{
						messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
						deferCommand = true;
					}
					else
					{
						if (enterpriseAddErr)
						{
							SendResponse(enterpriseAddErr);
							enterpriseAddErr = ResponseEmpty;
						}
						else
						{
							SendResponse(ResponseWrongState);
						}
					}
				}
				else
				{
					SendResponse(ResponseBadParameter);
				}
			}
			break;

		case NetworkCommand::networkDeleteSsid:				// delete a network from our access point list
			if (messageHeaderIn.hdr.dataLength == SsidLength)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(SsidLength));
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkRetrieveSsidData:	// list the access points we know about, including our own access point details
			if (dataBufferAvailable < ReducedWirelessConfigurationDataSize)
			{
				SendResponse(ResponseBufferTooSmall);
			}
			else
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks && (i + 1) * ReducedWirelessConfigurationDataSize <= dataBufferAvailable; ++i)
				{

					WirelessConfigurationData tempData;
					wirelessConfigMgr->GetSsid(i, tempData);
					if (tempData.ssid[0] != 0xFF)
					{
						memcpy(p, &tempData, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
					else if (i == 0)
					{
						memset(p, 0, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
				}
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				SendResponse(numBytes);
			}
			break;

		case NetworkCommand::networkListSsids_deprecated:	// list the access points we know about, plus our own access point details
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks; ++i)
				{
					WirelessConfigurationData tempData;
					wirelessConfigMgr->GetSsid(i, tempData);
					if (tempData.ssid[0] != 0xFF)
					{
						for (size_t j = 0; j < SsidLength && tempData.ssid[j] != 0; ++j)
						{
							*p++ = tempData.ssid[j];
						}
						*p++ = '\n';
					}
					else if (i == 0)
					{
						// Include an empty entry for our own access point SSID
						*p++ = '\n';
					}
				}
				*p++ = 0;
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				if (numBytes <= dataBufferAvailable)
				{
					SendResponse(numBytes);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
			}
			break;

		case NetworkCommand::networkSetHostName:			// set the host name
			if (messageHeaderIn.hdr.dataLength == HostNameLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(HostNameLength));
				memcpy(webHostName, transferBuffer, HostNameLength);
				webHostName[HostNameLength] = 0;			// ensure null terminator
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkGetLastError:
			if (lastError == nullptr)
			{
				SendResponse(0);
			}
			else
			{
				const size_t len = strlen((const char*)lastError) + 1;
				if (dataBufferAvailable >= len)
				{
					strcpy(reinterpret_cast<char*>(transferBuffer), (const char*)lastError);		// copy to 32-bit aligned buffer
					SendResponse(len);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
				lastError = nullptr;
			}
			lastReportedState = currentState;
			break;

		case NetworkCommand::networkStartScan:
			if ((scanState == WIFI_SCAN_IDLE || scanState == WIFI_SCAN_DONE) &&
				(currentState == WiFiState::idle || currentState == WiFiState::connected))
			{
				// Defer scan execution, as this can take a long time and cause a timeout
				// on RRF's side.
				SendResponse(ResponseEmpty);
				deferCommand = true;
			} else if (scanState == WIFI_SCANNING &&
					(currentState == WiFiState::idle || currentState == WiFiState::connected)) {
				SendResponse(ResponseScanInProgress);
			} else {
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkGetScanResult:
			if (scanState == WIFI_SCAN_DONE) {
				size_t data_sz = 0;

				if (wifiScanNum > 0) {
					// By default the records are sorted by signal strength, so just
					// send all ap records that fit the transfer buffer.
					for (int i = 0; i < wifiScanNum && data_sz <= sizeof(transferBuffer); i++, data_sz += sizeof(WiFiScanData))
					{
						const wifi_ap_record_t& ap = wifiScanAPs[i];
						WiFiScanData &d = reinterpret_cast<WiFiScanData*>(transferBuffer)[i];
						SafeStrncpy((char*)(d.ssid), (const char*)ap.ssid, std::min(sizeof(d.ssid), sizeof(ap.ssid)));
						d.rssi = ap.rssi;
						d.primaryChannel = ap.primary;
						memcpy(d.mac, ap.bssid, sizeof(d.mac));
						memset(d.spare, 0, sizeof(d.spare));

						if (ap.phy_11n) {
							d.phymode = EspWiFiPhyMode::N;
						} else if (ap.phy_11g) {
							d.phymode = EspWiFiPhyMode::G;
						} else if (ap.phy_11b) {
							d.phymode = EspWiFiPhyMode::B;
						}

						d.auth = EspAuthModeToWiFiAuth(ap.authmode);
					}

				}

				SendResponse(data_sz);

				if (currentState == WiFiState::idle) {
					esp_wifi_stop();
				}

				free(wifiScanAPs);
				wifiScanNum = 0;
				wifiScanAPs = nullptr;
				scanState = WIFI_SCAN_IDLE;
			} else if (scanState == WIFI_SCANNING) {
				SendResponse(ResponseScanInProgress);
			} else if (scanState == WIFI_SCAN_IDLE) {
				SendResponse(ResponseNoScanStarted);
			} else {
				SendResponse(ResponseUnknownError);
			}
			break;

		case NetworkCommand::networkListen:				// listen for incoming connections
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				const bool wantTls = (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagTls) != 0;
#if SUPPORTS_TLS
				const bool tlsReady = wantTls && TlsServer::GetInstance()->IsEnabled();
				const int32_t initialResponse = (wantTls && !tlsReady) ? ResponseWrongState : ResponseEmpty;
#else
				const bool tlsReady = false;
				const int32_t initialResponse = wantTls ? ResponseWrongState : ResponseEmpty;
#endif
				messageHeaderIn.hdr.param32 = hspi.transfer32(initialResponse);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				if (wantTls && !tlsReady)
				{
					break;
				}
				const bool ok = Listener::Start(lcData.port, lcData.remoteIp, lcData.protocol, lcData.maxConnections, wantTls);
				if (ok)
				{
					if (lcData.protocol < 3)			// if it's FTP, HTTP or Telnet protocol
					{
						RebuildServices();				// update the MDNS services
					}
					debugPrintf("%sListening on port %u\n", (lcData.maxConnections == 0) ? "Stopped " : "", lcData.port);
				}
				else
				{
					lastError = "Listen failed";
					debugPrint("Listen failed\n");
				}
			}
			break;

#if 0	// We don't use the following command, instead we use networkListen with maxConnections = 0
		case NetworkCommand::unused_networkStopListening:
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				Listener::Stop(lcData.port);
				RebuildServices();						// update the MDNS services
				debugPrintf("Stopped listening on port %u\n", lcData.port);
			}
			break;
#endif

		case NetworkCommand::connAbort:					// terminate a socket rudely
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Terminate(true);
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connClose:					// close a socket gracefully
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Close();
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connRead:					// read data from a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t amount = conn.Read(reinterpret_cast<uint8_t *>(transferBuffer), std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength));
				messageHeaderIn.hdr.param32 = hspi.transfer32(amount);
				hspi.transferDwords(transferBuffer, nullptr, NumDwords(amount));
				led_indicator_start(led, ONBOARD_LED_IO);
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connWrite:					// write data to a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t requestedlength = messageHeaderIn.hdr.dataLength;
				const size_t acceptedLength = std::min<size_t>(conn.CanWrite(), std::min<size_t>(requestedlength, MaxDataLength));
				const bool closeAfterSending = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagCloseAfterWrite) != 0;
				const bool push = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagPush) != 0;
				messageHeaderIn.hdr.param32 = hspi.transfer32(acceptedLength);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(acceptedLength));
				const size_t written = conn.Write(reinterpret_cast<uint8_t *>(transferBuffer), acceptedLength, push, closeAfterSending);
				if (written != acceptedLength)
				{
					lastError = "incomplete write";
				}
				led_indicator_start(led, ONBOARD_LED_IO);
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connGetStatus:				// get the status of a socket, and summary status for all sockets
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(sizeof(ConnStatusResponse));
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				ConnStatusResponse resp;
				conn.GetStatus(resp);
				Connection::GetSummarySocketStatus(resp.connectedSockets, resp.otherEndClosedSockets);

				// Evaluate RSSI here, since the WiFi connection is managed here.
				resp.rssi = INT8_MIN;
				if (currentState == WiFiState::connected)
				{
					wifi_ap_record_t ap_info;
					ap_info.rssi = 0;
					esp_wifi_sta_get_ap_info(&ap_info);
					resp.rssi = ap_info.rssi;
				}

				hspi.transferDwords(reinterpret_cast<const uint32_t *>(&resp), nullptr, NumDwords(sizeof(resp)));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::diagnostics:					// print some debug info over the UART line
			SendResponse(ResponseEmpty);
			deferCommand = true;							// we need to send the diagnostics after we have sent the response, so the SAM is ready to receive them

			break;

		case NetworkCommand::networkSetTxPower:
			{
				const uint8_t txPower = messageHeaderIn.hdr.flags;
				if (txPower <= 82)
				{
					esp_wifi_set_max_tx_power(txPower);
					SendResponse(ResponseEmpty);
				}
				else
				{
					SendResponse(ResponseBadParameter);
				}
			}
			break;

		case NetworkCommand::networkSetClockControl:
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			deferCommand = true;
			break;

		case NetworkCommand::connCreate:					// create a connection
			{
				Connection * const conn = Connection::Allocate();
				if (conn)
				{
					uint32_t connNum = conn->GetNum();
					messageHeaderIn.hdr.param32 = hspi.transfer32(connNum);
					ListenOrConnectData lcData;
					hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));

					if (!conn->Connect(lcData.protocol, lcData.remoteIp, lcData.port))
					{
						lastError = "Connection creation failed";
					}
				}
				else
				{
					// No available connection
					SendResponse(ResponseBusy);
				}
			}
			break;

#if SUPPORTS_TLS
		case NetworkCommand::networkEnableTls:
			// Probe + (re)build the mbedTLS server config from stored cert/key
			if (TlsServer::GetInstance()->Enable())
			{
				SendResponse(ResponseEmpty);
			}
			else
			{
				SendResponse(ResponseNoTlsCert);
			}
			break;

		case NetworkCommand::networkSetTlsCert:
		case NetworkCommand::networkSetTlsKey:
			{
				const uint16_t dataLen = messageHeaderIn.hdr.dataLength;
				if (dataLen == 0 || dataLen > MaxDataLength)
				{
					SendResponse(ResponseBadDataLength);
					break;
				}
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(transferBuffer), NumDwords(dataLen));
				WirelessConfigurationMgr *mgr = WirelessConfigurationMgr::GetInstance();
				const bool ok = (messageHeaderIn.hdr.command == NetworkCommand::networkSetTlsCert)
					? mgr->SetTlsCert(transferBuffer, dataLen)
					: mgr->SetTlsKey(transferBuffer, dataLen);
				if (!ok)
				{
					lastError = "Failed to store TLS material";
				}
				// Storing new material invalidates any cached config - caller will re-probe via networkEnableTls
				TlsServer::GetInstance()->Disable();
			}
			break;

		case NetworkCommand::networkClearTls:
			TlsServer::GetInstance()->Disable();
			(void)WirelessConfigurationMgr::GetInstance()->ClearTls();
			SendResponse(ResponseEmpty);
			break;
#endif // SUPPORTS_TLS

		default:
			SendResponse(ResponseUnknownCommand);
			break;
		}
	}

	//gpio_set_level(SamSSPin, 1);			// de-assert CS to SAM to end the transaction and tell SAM the transfer is complete
	hspi.endTransaction();
	gpio_set_level(SamSSPin, 1);			// de-assert CS to SAM to end the transaction and tell SAM the transfer is complete

	// If we deferred the command until after sending the response (e.g. because it may take some time to execute), complete it now
	if (deferCommand)
	{
		// The following functions must set up lastError if an error occurs
		lastError = nullptr;								// assume no error
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::networkStartClient:			// connect to an access point
			if (messageHeaderIn.hdr.dataLength == 0 || reinterpret_cast<const char*>(transferBuffer)[0] == 0)
			{
				StartClient(nullptr);						// connect to strongest known access point
			}
#if SUPPORT_ETHERNET
			else if (EthGetMode(reinterpret_cast<const char*>(transferBuffer)) >= 0)
			{
				EthStartClient(EthGetMode(reinterpret_cast<const char*>(transferBuffer)));
			}
#endif
			else
			{
				StartClient(reinterpret_cast<const char*>(transferBuffer));		// connect to specified access point
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			StartAccessPoint();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			Connection::TerminateAll();						// terminate all connections
			Listener::Stop(0);								// stop listening on all ports
			RebuildServices();								// remove the MDNS services
			switch (currentState)
			{
			case WiFiState::connected:
			case WiFiState::connecting:
			case WiFiState::reconnecting:
				RemoveMdnsServices();
				delay(20);									// try to give lwip time to recover from stopping everything
#if SUPPORT_ETHERNET
				if (ethState >= EthState::started)
				{
					esp_eth_stop(ethHandle);
				}
				else
#endif
				{
					esp_wifi_stop();
				}
				break;

			case WiFiState::runningAsAccessPoint:
				dns.stop();
				delay(20);									// try to give lwip time to recover from stopping everything
				esp_wifi_stop();
				break;

			default:
				break;
			}

			while (currentState != WiFiState::idle)
			{
				delay(100);
			}
			usingDhcpc = false;
			numWifiReconnects = 0;
			currentSsid = -1;
			break;

		case NetworkCommand::networkStartScan:
			if (scanState == WIFI_SCAN_DONE)
			{
				// Previous results were still not retrieved
				free(wifiScanAPs);
				wifiScanNum = 0;
				wifiScanAPs = nullptr;
				scanState = WIFI_SCAN_IDLE;
			}

			wifi_scan_config_t cfg;
			memset(&cfg, 0, sizeof(cfg));
			cfg.show_hidden = true;

			// If currently idle, start Wi-Fi in STA mode
			if (currentState == WiFiState::idle) {
				ConfigureSTAMode();
				if (!StartStation()) {
					lastError = "failed to start WiFi";
					break;
				}
			}

			if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
				scanState = WIFI_SCANNING;
			} else {
				// Since a response has already been sent, hopefully this
				// does not happen.
				lastError = "failed to start scan";
			}
			break;

		case NetworkCommand::diagnostics:
			Connection::ReportConnections();
			PrintStackWatermarks("M122");
			delay(20);										// give the Duet main processor time to digest that
			stats_display();
			break;

		case NetworkCommand::networkSetClockControl:
			// Reinitialize with new clock config
			hspi.end();
			hspi.InitMaster(SPI_MODE1, messageHeaderIn.hdr.param32, true);
			break;

		case NetworkCommand::networkAddSsid:				// add to our known access point list
		case NetworkCommand::networkConfigureAccessPoint:	// configure our own access point details
			{
				const WirelessConfigurationData *receivedClientData = reinterpret_cast<const WirelessConfigurationData *>(transferBuffer);
				const int ssid = wirelessConfigMgr->SetSsid(*receivedClientData,
							messageHeaderIn.hdr.command == NetworkCommand::networkConfigureAccessPoint);
				if (ssid < 0)
				{
					lastError = "SSID table full";
				}
			}
			break;

		case NetworkCommand::networkDeleteSsid:
			if (!wirelessConfigMgr->EraseSsid(reinterpret_cast<const char*>(transferBuffer)))
			{
				lastError = "SSID not found";
			}
			break;

		case NetworkCommand::networkAddEnterpriseSsid:
			{
				AddEnterpriseSsidFlag flag = static_cast<AddEnterpriseSsidFlag>(messageHeaderIn.hdr.flags);
				if (flag == AddEnterpriseSsidFlag::SSID)
				{
					const WirelessConfigurationData *newSsid = reinterpret_cast<const WirelessConfigurationData *>(transferBuffer);
					if (wirelessConfigMgr->BeginEnterpriseSsid(*newSsid))
					{
						enterpriseAddPending = true;
					}
					else
					{
						enterpriseAddErr = ResponseTooManySsids;
						lastError = "SSID table full";
					}
				}
				else if (flag == AddEnterpriseSsidFlag::CREDENTIAL)
				{
					if (!wirelessConfigMgr->SetEnterpriseCredential(messageHeaderIn.hdr.param32,
							transferBuffer, messageHeaderIn.hdr.dataLength))
					{
						enterpriseAddPending = false;
					}
				}
				else if (flag == AddEnterpriseSsidFlag::COMMIT || flag == AddEnterpriseSsidFlag::CANCEL)
				{
					const bool cancel = (flag == AddEnterpriseSsidFlag::CANCEL);
					const bool ok = wirelessConfigMgr->EndEnterpriseSsid(cancel);
					enterpriseAddPending = false;
					if (!ok || cancel)
					{
						lastError = "enterprise SSID not saved";
					}
				}
			}
			break;

		default:
			lastError = "bad deferred command";
			break;
		}
	}

	if (lastError != prevLastError) {
		xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
	}
}


void IRAM_ATTR TransferReadyIsr(void* p)
{
	BaseType_t woken = pdFALSE;
	xTaskNotifyFromISR(mainTaskHdl, SAM_TFR_READY, eSetBits, &woken);
	if (woken == pdTRUE)
	{
		portYIELD_FROM_ISR();
	}
}

void setup()
{
#if ESP8266
	uint32_t flashId = spi_flash_get_id_raw(&g_rom_flashchip);
	debugPrintf("flash id is: 0x%0x\n", flashId);
	flashSize = 1u << ((flashId >> 16) & 0xFF);
#else
	esp_flash_get_physical_size(NULL, &flashSize);
#endif
	mainTaskHdl = xTaskGetCurrentTaskHandle();
	debugPrintfAlways("\r\nESP Starting setup free heap %u\n", (unsigned)esp_get_free_heap_size());
	led_indicator_config_t ledCfg;
	ledCfg.off_level = LedOffLevel;
	ledCfg.mode = LED_GPIO_MODE;

	led = led_indicator_create(OnboardLedPin, &ledCfg);
	led_indicator_start(led, ONBOARD_LED_RESET);

	// Setup Wi-Fi
#if OLD_SDK
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	tcpip_adapter_init();
#pragma GCC diagnostic pop
	esp_event_loop_create_default();
#else
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
	ap_netif = esp_netif_create_default_wifi_ap();
#endif
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	debugPrintfAlways("After WiFi init %u\n", (unsigned)esp_get_free_heap_size());

	wifiEventGroup = xEventGroupCreate();

	esp_event_handler_register(WIFI_EVENT_EXT, WIFI_EVENT_STA_CONNECTING, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &HandleWiFiEvent, NULL);

	wirelessConfigMgr = WirelessConfigurationMgr::GetInstance();


	xTaskCreatePinnedToCore(WiFiConnectionTask, "wifiConnection", WIFI_CONNECTION_STACK, NULL,
		WIFI_CONNECTION_PRIO, &connPollTaskHdl, NET_TASK_CPU);

#ifdef DEBUG
	//xTaskCreate(StatePrintTask, "statePrint", STATE_PRINT_STACK, NULL, tskIDLE_PRIORITY, NULL);
#endif

	esp_log_level_set("wifi", ESP_LOG_NONE);
#if SUPPORTS_TLS
	// "Dynamic Impl" is ESP-IDF's mbedTLS dynamic-buffer wrapper. It logs every fetch_input failure
	// at ERROR level, including the CONN_RESET that fires on benign peer disconnects. We already log
	// handshake/read outcomes from our own code with proper context, so silence the wrapper noise
	esp_log_level_set("Dynamic Impl", ESP_LOG_NONE);
#endif

	wirelessConfigMgr = WirelessConfigurationMgr::GetInstance();
	wirelessConfigMgr->Init();
	debugPrintfAlways("After config mgr init %u\n", (unsigned)esp_get_free_heap_size());

#if SUPPORT_ETHERNET
# if ETH_V0
	// Make sure that we tristate the connection to GPIO0 to prevent conflicts
	// with the eth clock.
	gpio_reset_pin(ProgramDisable);
	gpio_set_direction(ProgramDisable, GPIO_MODE_OUTPUT);
	gpio_set_level(ProgramDisable, 0);
# endif
#endif
	// Set up SPI hardware and request handling
	gpio_reset_pin(SamTfrReadyPin);
	gpio_set_direction(SamTfrReadyPin, GPIO_MODE_INPUT);

	gpio_reset_pin(EspReqTransferPin);
	gpio_set_direction(EspReqTransferPin, GPIO_MODE_OUTPUT);
	gpio_set_level(EspReqTransferPin, 0);

	gpio_reset_pin(SamSSPin);
	gpio_set_direction(SamSSPin, GPIO_MODE_OUTPUT);
	gpio_set_level(SamSSPin, 1);
	hspi.InitMaster(SPI_MODE1, defaultClockControl, true);

	gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(SamTfrReadyPin, TransferReadyIsr, nullptr);
	gpio_set_intr_type(SamTfrReadyPin, GPIO_INTR_POSEDGE);

	tfrReqExpTmr = xTimerCreate("tfrReqExpTmr", StatusReportMillis, pdFALSE, NULL,
		[](TimerHandle_t data) {
			xTaskNotify(mainTaskHdl, TFR_REQUEST_TIMEOUT, eSetBits);
		});
	xTimerStart(tfrReqExpTmr, portMAX_DELAY);

#ifdef ESP8266
	mainLoopWdtTmr = xTimerCreate("mainLoopWdt", pdMS_TO_TICKS(MAIN_TASK_WDT_MS), pdTRUE, NULL,
		[](TimerHandle_t data) {
			const uint32_t current = mainLoopHeartbeat;
			if (current == mainLoopHeartbeatLastSeen)
			{
				debugPrintfAlways("Main loop watchdog timeout (no tick in %u ms), restarting\n",
					(unsigned)MAIN_TASK_WDT_MS);
				esp_restart();
			}
			mainLoopHeartbeatLastSeen = current;
		});
	xTimerStart(mainLoopWdtTmr, portMAX_DELAY);
#else
	// Hardware-backed task WDT with main task as the only subscriber. Idle-task subscription is
	// disabled in sdkconfig.defaults.<target> so a healthy idle task cannot mask a wedged main
#if OLD_SDK
	esp_task_wdt_init(MAIN_TASK_WDT_MS / 1000, true);
#else
	// If the TWDT was not initialized automatically on startup, manually intialize it now
	esp_task_wdt_config_t twdt_config = {
		.timeout_ms = MAIN_TASK_WDT_MS,
		.idle_core_mask = 0, // No idle tasks subscribed
		.trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    printf("TWDT initialized\n");
#endif
	esp_task_wdt_add(NULL);
#endif

	// Setup networking
	Connection::Init();

	Listener::Init();

	lastError = nullptr;
	debugPrintfAlways("Init completed %u\n", (unsigned)esp_get_free_heap_size());
	gpio_set_level(EspReqTransferPin, 1);					// tell the SAM we are ready to receive a command
	led_indicator_stop(led, ONBOARD_LED_RESET);
	led_indicator_start(led, ONBOARD_LED_IDLE);
}

void loop()
{
#ifdef ESP8266
	++mainLoopHeartbeat;	// fed to mainLoopWdtTmr, see setup()
#else
	esp_task_wdt_reset();
#endif

	// See whether there is a request from the SAM.
	// Duet WiFi 1.04 and earlier have hardware to ensure that TransferReady goes low when a transaction starts.
	// Duet 3 Mini doesn't, so we need to see TransferReady go low and then high again. In case that happens so fast that we dn't get the interrupt, we have a timeout.
	uint32_t flags = 0;
	xTaskNotifyWait(0, UINT_MAX, &flags, TransferReadyTimeout);

	if ((flags & TFR_REQUEST) || ((flags & TFR_REQUEST_TIMEOUT) &&
		(lastError != nullptr || currentState != lastReportedState) ))
	{
		ets_delay_us(2);									// make sure the pin stays high for long enough for the SAM to see it
		gpio_set_level(EspReqTransferPin, 0);			// force a low to high transition to signal that an error message is available
		ets_delay_us(2);									// make sure it is low enough to create an interrupt when it goes high
		gpio_set_level(EspReqTransferPin, 1);			// tell the SAM we are ready to receive a command
		prevLastError = lastError;
		xTimerReset(tfrReqExpTmr, portMAX_DELAY);
	}

	Connection::PollAll();

	if (gpio_get_level(SamTfrReadyPin) == 1 &&
		(flags == 0 || (flags & SAM_TFR_READY))) {
		ProcessRequest();
	}
}

// End
