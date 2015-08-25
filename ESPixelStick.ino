/*
* ESPixelStick.ino
*
* Project: ESPixelStick - An ESP8266 and E1.31 based pixel driver
* Copyright (c) 2015 Shelby Merrick
* http://www.forkineye.com
*
* Library Requirements:
* - E1.31 for Arduino - https://github.com/forkineye/E131
* - Adafruit NeoPixel - https://github.com/adafruit/Adafruit_NeoPixel
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <E131.h>
//#include <Adafruit_NeoPixel.h>
#include "ESPixelDriver.h"
#include "ESPixelStick.h"
#include "helpers.h"

/* Web pages and handlers */
#include "page_microajax.js.h"
#include "page_style.css.h"
#include "page_root.h"
#include "page_config_net.h"
#include "page_config_pixel.h"
#include "page_status_net.h"
#include "page_status_e131.h"


/*************************************************/
/*      BEGIN - User Configuration Defaults      */
/*************************************************/

#define NUM_PIXELS      170     /* Number of pixels */
#define UNIVERSE        1       /* First Universe to listen for */
#define CHANNEL_START   1       /* Channel to start listening at */

const char ssid[] = "SSID_NOT_SET";             /* Replace with your SSID */
const char passphrase[] = "PASSWORD_NOT_SET";   /* Replace with your WPA2 passphrase */

/*************************************************/
/*       END - User Configuration Defaults       */
/*************************************************/

//Adafruit_NeoPixel   pixels;
ESPixelDriver	pixels;			/* Pixel object */
uint16_t		uniLast = 1;	/* Last Universe to listen for */
uint8_t         seqTracker[4];  /* Current sequence numbers for each Universe */
uint32_t        lastPacket;     /* Packet timeout tracker */

void setup() {
    Serial.begin(115200);
    delay(10);

    Serial.println("");
    for (uint8_t i = 0; i < strlen_P(VERSION); i++)
        Serial.print((char)(pgm_read_byte(VERSION + i)));
    Serial.println("");
    
    /* Load configuration from EEPROM */
    EEPROM.begin(sizeof(config));
    loadConfig();

    /* Fallback to default SSID and passphrase if we fail to connect */
    int status = initWifi();
    if (status != WL_CONNECTED) {
        Serial.println(F("*** Timeout - Reverting to default SSID ***"));
        strncpy(config.ssid, ssid, sizeof(config.ssid));
        strncpy(config.passphrase, passphrase, sizeof(config.passphrase));
        status = initWifi();
    }

    /* If we fail again, reboot */
    if (status != WL_CONNECTED) {
        Serial.println(F("**** FAILED TO ASSOCIATE WITH AP ****"));
        ESP.restart();
    }

    /* Configure and start the web server */
    initWeb();

    /* Setup DNS-SD */
/* -- not working
    if (MDNS.begin("esp8266")) {
        MDNS.addService("e131", "udp", E131_DEF_PORT);
        MDNS.addService("http", "tcp", HTTP_PORT);
    } else {
        Serial.println(F("** Error setting up MDNS responder **"));
    }
*/    

    /* Initialize globals */
    memset(seqTracker, 0x00, sizeof(seqTracker));
    memset(seqError, 0x00, sizeof(seqTracker));

    /* Configure UART1 for WS2811 output */
    pixels.setPin(DATA_PIN);    /* For protocols that require bit-banging */
    pixels.begin();
    updatePixelConfig();
    pixels.show();
}

int initWifi() {
    /* Begin listening for E1.31 data  - we aren't using DNS, so set it to the gateway if static */
    int status = WL_IDLE_STATUS;

    if (config.dhcp) {
        if (config.multicast)
            status = e131.beginMulticast(config.ssid, config.passphrase, config.universe);
        else
            status = e131.begin(config.ssid, config.passphrase);
    } else {
        if (config.multicast)
            status = e131.beginMulticast(config.ssid, config.passphrase, config.universe,
                    IPAddress(config.ip[0], config.ip[1], config.ip[2], config.ip[3]),
                    IPAddress(config.netmask[0], config.netmask[1], config.netmask[2], config.netmask[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3])
            );
        else
            status = e131.begin(config.ssid, config.passphrase,
                    IPAddress(config.ip[0], config.ip[1], config.ip[2], config.ip[3]),
                    IPAddress(config.netmask[0], config.netmask[1], config.netmask[2], config.netmask[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3])
            );
    }
    return status;
}

/* Configure and start the web server */
void initWeb() {
    /* JavaScript and Stylesheets */
    web.on ("/style.css", []() { web.send(200, "text/plain", PAGE_STYLE_CSS); });
    web.on ("/microajax.js", []() { web.send(200, "text/plain", PAGE_MICROAJAX_JS); });

    /* HTML Pages */
    web.on("/", []() { web.send(200, "text/html", PAGE_ROOT); });
    web.on("/config/net.html", send_config_net_html);
    web.on("/config/pixel.html", send_config_pixel_html);
    web.on("/status/net.html", []() { web.send(200, "text/html", PAGE_STATUS_NET); });
    web.on("/status/e131.html", []() { web.send(200, "text/html", PAGE_STATUS_E131); });

    /* AJAX Handlers */
    web.on("/rootvals", send_root_vals_html);
    web.on("/config/netvals", send_config_net_vals_html);
    web.on("/config/pixelvals", send_config_pixel_vals_html);
    web.on("/config/connectionstate", send_connection_state_vals_html);
    web.on("/status/netvals", send_status_net_vals_html);
    web.on("/status/e131vals", send_status_e131_vals_html);

    web.onNotFound([]() { web.send(404, "text/html", "Page not Found"); });
    web.begin();

    Serial.print(F("- Web Server started on port "));
    Serial.println(HTTP_PORT);
}

void updatePixelConfig() {
	uniLast = config.universe + ceil((config.pixel_count * 3) / 512);
    pixels.updateType(config.pixel_type, config.pixel_color);
    pixels.updateLength(config.pixel_count);    
    Serial.print(F("- listening for "));
    Serial.print(config.pixel_count * 3);
    Serial.print(F(" channels, from Universe "));
    Serial.print(config.universe);
    Serial.print(F(" to "));
    Serial.println(uniLast);
}

/* Attempt to load configuration from EEPROM.  Initialize or upgrade as required */
void loadConfig() {
    EEPROM.get(EEPROM_BASE, config);
    if (memcmp_P(config.id, CONFIG_ID, sizeof(config.id))) {
        Serial.println(F("- No configuration found."));

        /* Initialize configuration structure */
        memset(&config, 0, sizeof(config));
        memcpy_P(config.id, CONFIG_ID, sizeof(config.id));
        config.version = CONFIG_VERSION;
        strncpy(config.name, "ESPixelStick", sizeof(config.name));
        strncpy(config.ssid, ssid, sizeof(config.ssid));
        strncpy(config.passphrase, passphrase, sizeof(config.passphrase));
        config.ip[0] = 0; config.ip[1] = 0; config.ip[2] = 0; config.ip[3] = 0;
        config.netmask[0] = 0; config.netmask[1] = 0; config.netmask[2] = 0; config.netmask[3] = 0;
        config.gateway[0] = 0; config.gateway[1] = 0; config.gateway[2] = 0; config.gateway[3] = 0;
        config.dhcp = 1;
        config.multicast = 0;
        config.universe = UNIVERSE;
        config.channel_start = CHANNEL_START;
        config.pixel_count = NUM_PIXELS;
        config.pixel_type = PIXEL_WS2811;
        config.pixel_color = COLOR_RGB;
        config.gamma = 1.0;

        /* Write the configuration structre */
        EEPROM.put(EEPROM_BASE, config);
        EEPROM.commit();
        Serial.println(F("* Default configuration saved."));
    } else {
        Serial.println(F("- Configuration loaded."));
    }
}

void saveConfig() {
    /* Write the configuration structre */
    EEPROM.put(EEPROM_BASE, config);
    EEPROM.commit();
    Serial.println(F("* New configuration saved."));
}

/* Main Loop */
void loop() {
    /* Handle incoming web requests if needed */
    web.handleClient();

    /* Parse a packet and update pixels */
    if(e131.parsePacket()) {
        if ((e131.universe >= config.universe) && (e131.universe <= uniLast)) {
            lastPacket = millis();
            
            /* Universe offset and sequence tracking */
            uint8_t uniOffset = (e131.universe - config.universe);
            if (e131.packet->sequence_number != seqTracker[uniOffset]++) {
                seqError[uniOffset]++;
                seqTracker[uniOffset] = e131.packet->sequence_number + 1;
            }
            
            /* Find out starting pixel based off the Universe */
            uint16_t pixelStart = uniOffset * PIXELS_MAX;

            /* Calculate how many pixels we need from this buffer */
            uint16_t pixelStop = config.pixel_count;
            if ((pixelStart + PIXELS_MAX) < pixelStop)
                pixelStop = pixelStart + PIXELS_MAX;

            /* Offset the channel if required for the first universe */
            uint16_t offset = 0;
            if (e131.universe == config.universe)
                offset = config.channel_start - 1;

            /* Set the pixel data */
            for (int i = pixelStart; i < pixelStop; i++) {
                int j = i * 3 + offset;
                pixels.setPixelColor(i, e131.data[j], e131.data[j+1], e131.data[j+2]);
            }

            /* Refresh when last universe shows up */
            if (e131.universe == uniLast)
                pixels.show();
        }
    }
    
    /* Force refresh every second if there is no data received */
    if ((millis() - lastPacket) > E131_TIMEOUT) {
        lastPacket = millis();
        pixels.show();
    }
}
