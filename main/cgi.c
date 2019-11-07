/*
Some random cgi routines. Used in the LED example and the page that returns the entire
flash as a binary. Also handles the hit counter on the main page.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */


#include <libesphttpd/esp.h>
#include "cJSON.h"
#include "cgi.h"
#include "io.h"


//cause I can't be bothered to write an ioGetLed()
static char currLedState=0;

//Cgi that turns the LED on or off according to the 'led' param in the POST data
CgiStatus ICACHE_FLASH_ATTR cgiLed(HttpdConnData *connData) {
	int len;
	char buff[1024];

	if (connData->isConnectionClosed) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	len=httpdFindArg(connData->post.buff, "led", buff, sizeof(buff));
	if (len!=0) {
		currLedState=atoi(buff);
		ioLed(LED_CGI, currLedState);
	}

	httpdRedirect(connData, "led.tpl");
	return HTTPD_CGI_DONE;
}



//Template code for the led page.
CgiStatus ICACHE_FLASH_ATTR tplLed(HttpdConnData *connData, char *token, void **arg) {
	char buff[128];
	if (token==NULL) return HTTPD_CGI_DONE;

	strcpy(buff, "Unknown");
	if (strcmp(token, "ledstate")==0) {
		if (currLedState) {
			strcpy(buff, "ON");
		} else {
			strcpy(buff, "OFF");
		}
	}
	if (strcmp(token, "ledgpio")==0) {
		sprintf(buff, "%u", LED_CGI);
	}
	httpdSend(connData, buff, -1);
	return HTTPD_CGI_DONE;
}

static int hitCounter=0;

//Template code for the counter on the index page.
CgiStatus ICACHE_FLASH_ATTR tplCounter(HttpdConnData *connData, char *token, void **arg) {
	char buff[128];
	if (token==NULL) return HTTPD_CGI_DONE;

	if (strcmp(token, "counter")==0) {
		hitCounter++;
		sprintf(buff, "%d", hitCounter);
	}
	httpdSend(connData, buff, -1);
	return HTTPD_CGI_DONE;
}

static void cgiJsonResponseCommon(HttpdConnData *connData, cJSON *jsroot){
	char *json_string = NULL;

	//// Generate the header
	//We want the header to start with HTTP code 200, which means the document is found.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Cache-Control", "no-store, must-revalidate, no-cache, max-age=0");
	httpdHeader(connData, "Expires", "Mon, 01 Jan 1990 00:00:00 GMT");  //  This one might be redundant, since modern browsers look for "Cache-Control".
	httpdHeader(connData, "Content-Type", "application/json; charset=utf-8"); //We are going to send some JSON.
	httpdEndHeaders(connData);
	json_string = cJSON_Print(jsroot);
    if (json_string)
    {
    	httpdSend(connData, json_string, -1);
        cJSON_free(json_string);
    }
    cJSON_Delete(jsroot);
}
//Graph
CgiStatus ICACHE_FLASH_ATTR GraphInfo(HttpdConnData *connData)
{
	char buff[128]= "Hello world!";
	if (connData->isConnectionClosed)
	{ 
		return HTTPD_CGI_DONE;
	}

	cJSON *jsroot = cJSON_CreateObject();

	cJSON_AddStringToObject(jsroot, "temp", "18.23");
	cgiJsonResponseCommon(connData, jsroot);

	return HTTPD_CGI_DONE;
}