/*
This is example code for the esphttpd library. It's a small-ish demo showing off
the server, including WiFi connection management capabilities, some IO etc.
*/
#include "sdkconfig.h"

#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "io.h"


#include "espfs.h"
#include "espfs_image.h"
#include "libesphttpd/httpd-espfs.h"


#include "cgi.h"
#include "libesphttpd/cgiwifi.h"
#include "libesphttpd/cgiflash.h"
#include "libesphttpd/auth.h"

#include "libesphttpd/cgiwebsocket.h"
#include "libesphttpd/httpd-freertos.h"
#include "libesphttpd/route.h"
#include "cgi-test.h"

#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"


#include "freertos/event_groups.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "esp_err.h"

#include "esp_event.h"
#include "tcpip_adapter.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"


int num_devices = 0;
OneWireBus *owb;
OneWireBus_ROMCode device_rom_codes[8] = {0};

char my_hostname[16] = "esphttpd";

#define EXAMPLE_WIFI_SSID      CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_WIFI_PASS      CONFIG_EXAMPLE_WIFI_PASSWORD


#define TAG "user_main"

#define LISTEN_PORT     80u
#define MAX_CONNECTIONS 32u

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define PORT_ENTER_CRITICAL portENTER_CRITICAL(&mux)
#define PORT_EXIT_CRITICAL  portEXIT_CRITICAL(&mux)

static char connectionMemory[sizeof(RtosConnType) * MAX_CONNECTIONS];
static HttpdFreertosInstance httpdFreertosInstance;

#define OTA_FLASH_SIZE_K 1024
#define OTA_TAGNAME "generic"




//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen) {
	if (no == 0) {
		strcpy(user, "admin");
		strcpy(pass, "s3cr3t");
		return 1;
//Add more users this way. Check against incrementing no for each user added.
//	} else if (no==1) {
//		strcpy(user, "user1");
//		strcpy(pass, "something");
//		return 1;
	}
	return 0;
}




//Broadcast the uptime in seconds every second over connected websockets
/*static void websocketBcast(void *arg) {
	static int ctr=0;
	char buff[128];
	while(1) {
		ctr++;
		sprintf(buff, "Up for %d minutes %d seconds!\n", ctr/60, ctr%60);
		cgiWebsockBroadcast(&httpdFreertosInstance.httpdInstance,
		                    "/websocket/ws.cgi", buff, strlen(buff),
		                    WEBSOCK_FLAG_NONE);

		vTaskDelay(1000/portTICK_RATE_MS);
	}
}*/

//On reception of a message, send "You sent: " plus whatever the other side sent
static void myWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	int i;
	char buff[128];
	sprintf(buff, "You sent: ");
	for (i=0; i<len; i++) buff[i+10]=data[i];
	buff[i+10]=0;
	printf(buff);
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, buff, strlen(buff), WEBSOCK_FLAG_NONE);
}

//Websocket connected. Install reception handler and send welcome message.
static void myWebsocketConnect(Websock *ws) {
	ws->recvCb=myWebsocketRecv;
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, "Hi, Websocket!", 14, WEBSOCK_FLAG_NONE);
}

//On reception of a message, echo it back verbatim
void myEchoWebsocketRecv(Websock *ws, char *data, int len, int flags) {
	printf("EchoWs: echo, len=%d\n", len);
	cgiWebsocketSend(&httpdFreertosInstance.httpdInstance,
	                 ws, data, len, flags);
}

//Echo websocket connected. Install reception handler.
void myEchoWebsocketConnect(Websock *ws) {
	printf("EchoWs: connect\n");
	ws->recvCb=myEchoWebsocketRecv;
}

static esp_err_t app_event_handler(void *ctx, system_event_t *event)
{
	switch(event->event_id) {
	case SYSTEM_EVENT_ETH_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_ETH, my_hostname);
		break;
	case SYSTEM_EVENT_STA_START:
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, my_hostname);
		// esp_wifi_connect(); /* Calling this unconditionally would interfere with the WiFi CGI. */
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		{
			tcpip_adapter_ip_info_t sta_ip_info;
			wifi_config_t sta_conf;
			printf("~~~~~STA~~~~~" "\n");
			if (esp_wifi_get_config(TCPIP_ADAPTER_IF_STA, &sta_conf) == ESP_OK) 
			{
				printf("ssid: %s" "\n", sta_conf.sta.ssid);
			}

			if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &sta_ip_info) == ESP_OK) 
			{
				printf("IP:" IPSTR "\n", IP2STR(&sta_ip_info.ip));
				printf("MASK:" IPSTR "\n", IP2STR(&sta_ip_info.netmask));
				printf("GW:" IPSTR "\n", IP2STR(&sta_ip_info.gw));
			}
			printf("~~~~~~~~~~~~~" "\n");
		}
		set_status_ind_wifi(WIFI_STATE_CONN);
		break;
	case SYSTEM_EVENT_STA_CONNECTED:
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
		/* Skip reconnect if disconnect was deliberate or authentication      *\
        \* failed.                                                            */
		switch(event->event_info.disconnected.reason){
		case WIFI_REASON_ASSOC_LEAVE:
		case WIFI_REASON_AUTH_FAIL:
			break;
		default:
			esp_wifi_connect();
			break;
		}
		break;
		case SYSTEM_EVENT_AP_START:
		{
			tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, my_hostname);
			tcpip_adapter_ip_info_t ap_ip_info;
			wifi_config_t ap_conf;
			printf("~~~~~AP~~~~~" "\n");
			if (esp_wifi_get_config(TCPIP_ADAPTER_IF_AP, &ap_conf) == ESP_OK) {
				printf("ssid: %s" "\n", ap_conf.ap.ssid);
				if (ap_conf.ap.authmode != WIFI_AUTH_OPEN) printf("pass: %s" "\n", ap_conf.ap.password);
			}

			if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info) == ESP_OK) {
				printf("IP:" IPSTR "\n", IP2STR(&ap_ip_info.ip));
				printf("MASK:" IPSTR "\n", IP2STR(&ap_ip_info.netmask));
				printf("GW:" IPSTR "\n", IP2STR(&ap_ip_info.gw));
			}
			printf("~~~~~~~~~~~~" "\n");
			set_status_ind_wifi(WIFI_STATE_AP);
		}
		break;
		case SYSTEM_EVENT_AP_STACONNECTED:
			ESP_LOGI(TAG, "station:" MACSTR" join,AID=%d",
					MAC2STR(event->event_info.sta_connected.mac),
					event->event_info.sta_connected.aid);

			set_status_ind_wifi(WIFI_STATE_CONN);
			break;
		case SYSTEM_EVENT_AP_STADISCONNECTED:
			ESP_LOGI(TAG, "station:" MACSTR"leave,AID=%d",
					MAC2STR(event->event_info.sta_disconnected.mac),
					event->event_info.sta_disconnected.aid);

			wifi_sta_list_t sta_list;
			ESP_ERROR_CHECK( esp_wifi_ap_get_sta_list(&sta_list));
			if (sta_list.num == 0) set_status_ind_wifi(WIFI_STATE_AP); // no clients left, change blink indication to AP
			break;
		case SYSTEM_EVENT_SCAN_DONE:

			break;
		default:
			break;
	}

	/* Forward event to to the WiFi CGI module */
	cgiWifiEventCb(event);

	return ESP_OK;
}

//Simple task to connect to an access point
void init_wifi(bool modeAP) 
{
	printf("Task run in core: %d\n", xPortGetCoreID());
	esp_err_t result;

	result = nvs_flash_init();
	if(   result == ESP_ERR_NVS_NO_FREE_PAGES
	   || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_LOGI(TAG, "Erasing NVS");
		nvs_flash_erase();
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	// Initialise wifi configuration CGI
	result = initCgiWifi();
	ESP_ERROR_CHECK(result);

	ESP_ERROR_CHECK( esp_event_loop_init(app_event_handler, NULL) );

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

	//Go to station mode
	esp_wifi_disconnect();

	if(modeAP) {
		ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_AP) );

		wifi_config_t ap_config;
		strcpy((char*)(&ap_config.ap.ssid), "ESP");
		ap_config.ap.ssid_len = 3;
		ap_config.ap.channel = 1;
		ap_config.ap.authmode = WIFI_AUTH_OPEN;
		ap_config.ap.ssid_hidden = 0;
		ap_config.ap.max_connection = 3;
		ap_config.ap.beacon_interval = 100;

		esp_wifi_set_config(WIFI_IF_AP, &ap_config);
	}
	else {
		esp_wifi_set_mode(WIFI_MODE_STA);

		//Connect to the defined access point.
		wifi_config_t config;
		memset(&config, 0, sizeof(config));
		sprintf((char*)config.sta.ssid, EXAMPLE_WIFI_SSID);
		sprintf((char*)config.sta.password, EXAMPLE_WIFI_PASS);
		esp_wifi_set_config(WIFI_IF_STA, &config);
		esp_wifi_connect();
	}

	ESP_ERROR_CHECK( esp_wifi_start() );
}

void read_temperature(void *pvParametr)
{
	printf("Task run in core: %d\n", xPortGetCoreID());
	owb_rmt_driver_info rmt_driver_info;
	owb = owb_rmt_initialize(&rmt_driver_info, 15, RMT_CHANNEL_1, RMT_CHANNEL_0);
	owb_use_crc(owb, true);
	printf("Find devices:\n");
	
	
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found)
    {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        printf("  %d : %s\n", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    printf("Found %d device%s\n", num_devices, num_devices == 1 ? "" : "s");

	DS18B20_Info * devices[8] = {0};

	for (int i = 0; i < num_devices; ++i)
    {
        DS18B20_Info * ds18b20_info = ds18b20_malloc();  // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1)
        {
            printf("Single device optimisations enabled\n");
            ds18b20_init_solo(ds18b20_info, owb);          // only one device on bus
        }
        else
        {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true);           // enable CRC check for temperature readings
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION_12_BIT);
    }

	int errors_count[8] = {0};
    int sample_count = 0;
    if (num_devices > 0)
    {
        TickType_t last_wake_time = xTaskGetTickCount();

        while (1)
        {
            last_wake_time = xTaskGetTickCount();

            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may fail
            // (using printf before reading may take too long)
            float readings[8] = { 0 };
            DS18B20_ERROR errors[8] = { 0 };

            for (int i = 0; i < num_devices; ++i)
            {
                errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
            }

            // Print results in a separate loop, after all have been read
            printf("\nTemperature readings (degrees C): sample %d\n", ++sample_count);
            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] != DS18B20_OK)
                {
                    ++errors_count[i];
                }

                printf("  %d: %.1f    %d errors\n", i, readings[i], errors_count[i]);
            }

            vTaskDelayUntil(&last_wake_time, 1000 / portTICK_PERIOD_MS);
        }
    }

	vTaskDelete(NULL);
	
	//sensor_count = ds18x20_scan_devices(15, addrs, 8);
	//ESP_ERROR_CHECK(ds18x20_measure(15, ds18x20_ANY, true));
	//ESP_ERROR_CHECK(ds18x20_read_temperature(15,ds18x20_ANY, &temps));
	//printf("Sensor reports %f deg C\n", temps);
	//vTaskDelay(5000/portTICK_RATE_MS);
	
}

char tag[] = "espfs_main";
static void * flashAddress = (void *)(4*1024*1024 - 10 *64*1024);

void print_string(void *pvParametr)
{
	char buff[5*1024];
	ESP_LOGD(tag, "Flash address is 0x%x", (int)flashAddress);
	if (espFsInit(flashAddress) != ESPFS_INIT_RESULT_OK) {
		ESP_LOGD(tag, "Failed to initialize espfs");
		return;
	}

	EspFsFile *fh = espFsOpen("files/test3.txt");;

	if (fh != NULL) {
		int sizeRead = 0;
		sizeRead = espFsRead(fh, buff, sizeof(buff));
		ESP_LOGD(tag, "Result: %.*s", sizeRead, buff);

		size_t fileSize;
		char *data;
		sizeRead = espFsSeek(fh, (void **)&data, &fileSize);
		ESP_LOGD(tag, "Result from access: %.*s", fileSize, data);

		espFsClose(fh);
		vTaskDelete(NULL);
	}
}

CgiUploadFlashDef uploadParams={
	.type=CGIFLASH_TYPE_FW,
	.fw1Pos=0x1000,
	.fw2Pos=((OTA_FLASH_SIZE_K*1024)/2)+0x1000,
	.fwSize=((OTA_FLASH_SIZE_K*1024)/2)-0x1000,
	.tagName=OTA_TAGNAME
};

/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones. Authorization things (like authBasic) act as a 'barrier' and
should be placed above the URLs they protect.
*/

HttpdBuiltInUrl builtInUrls[] = {
	ROUTE_CGI_ARG("*", cgiRedirectApClientToHostname, "esp8266.nonet"),
	ROUTE_REDIRECT("/", "/index.tpl"),

	
	ROUTE_TPL("/index.tpl", tplCounter),

	ROUTE_TPL("/led.tpl", tplLed),
	ROUTE_CGI("/led.cgi", cgiLed),

	ROUTE_REDIRECT("/graph", "/graph/graph.html"),
	ROUTE_CGI("/graph/graphinfo.json", GraphInfo),

	ROUTE_REDIRECT("/flash", "/flash/index.html"),
	ROUTE_REDIRECT("/flash/", "/flash/index.html"),
	ROUTE_CGI("/flash/flashinfo.json", cgiGetFlashInfo),
	ROUTE_CGI("/flash/setboot", cgiSetBoot),
	ROUTE_CGI_ARG("/flash/upload", cgiUploadFirmware, &uploadParams),
	ROUTE_CGI_ARG("/flash/erase", cgiEraseFlash, &uploadParams),
	ROUTE_CGI("/flash/reboot", cgiRebootFirmware),

	//Routines to make the /wifi URL and everything beneath it work.
	//Enable the line below to protect the WiFi configuration with an username/password combo.
	{"/wifi/*", authBasic, myPassFn},

	ROUTE_REDIRECT("/wifi", "/wifi/wifi.tpl"),
	ROUTE_REDIRECT("/wifi/", "/wifi/wifi.tpl"),
	ROUTE_CGI("/wifi/wifiscan.cgi", cgiWiFiScan),
	ROUTE_TPL("/wifi/wifi.tpl", tplWlan),
	ROUTE_CGI("/wifi/connect.cgi", cgiWiFiConnect),
	ROUTE_CGI("/wifi/connstatus.cgi", cgiWiFiConnStatus),
	ROUTE_CGI("/wifi/setmode.cgi", cgiWiFiSetMode),
	ROUTE_CGI("/wifi/startwps.cgi", cgiWiFiStartWps),
	ROUTE_CGI("/wifi/ap", cgiWiFiAPSettings),

	ROUTE_REDIRECT("/websocket", "/websocket/index.html"),
	ROUTE_WS("/websocket/ws.cgi", myWebsocketConnect),
	ROUTE_WS("/websocket/echo.cgi", myEchoWebsocketConnect),

	ROUTE_REDIRECT("/httptest", "/httptest/index.html"),
	ROUTE_REDIRECT("/httptest/", "/httptest/index.html"),
	ROUTE_CGI("/httptest/test.cgi", cgiTestbed),

	ROUTE_FILESYSTEM(),

	ROUTE_END()
};


//Main routine. Initialize stdout, the I/O, filesystem and the webserver and we're done.
void app_main(void) {

	printf("Task run in core: %d\n", xPortGetCoreID());

	//ioInit();
	//captdnsInit();

	espFsInit((void*)(image_espfs_start)); // CONFIG_ESPHTTPD_USE_ESPFS

	tcpip_adapter_init();
	httpdFreertosInit(&httpdFreertosInstance,
	                  builtInUrls,
	                  LISTEN_PORT,
	                  connectionMemory,
	                  MAX_CONNECTIONS,
	                  HTTPD_FLAG_NONE);

	httpdFreertosStart(&httpdFreertosInstance);
	init_wifi(true); // Supply false for STA mode
	
	
	printf("\nReady\n");

	

	//xTaskCreate(read_temperature, "Read Temperature", 4*configMINIMAL_STACK_SIZE, NULL, 2*tskIDLE_PRIORITY, NULL);
	xTaskCreate(print_string, "Print String", configMINIMAL_STACK_SIZE, NULL, 2*tskIDLE_PRIORITY , NULL);
	
}
