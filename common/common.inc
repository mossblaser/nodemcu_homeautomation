#include <ESP8266WiFi.h>
#include <Qth.h>
#include <EEPROM.h>

#ifndef WIFI_SSID
	#error "Macro WIFI_SSID must be defined (string)."
#endif

#ifndef WIFI_PASSWORD
	#error "Macro WIFI_PASSWORD must be defined (string)."
#endif

#ifndef QTH_SERVER
	#error "Macro QTH_SERVER must be defined (string)."
#endif

#ifndef SERIAL_BAUDRATE
	#define SERIAL_BAUDRATE 9600
#endif

WiFiClient wifiClient;
Qth::QthClient qth(
  QTH_SERVER,
  wifiClient,
  qth_client_id,
  qth_client_description);

void setup_serial() {
	Serial.begin(SERIAL_BAUDRATE);
}

void setup_eeprom() {
	EEPROM.begin(4096);
}

void setup_wifi() {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

	WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setup_qth() {
	Serial.print("Qth server: ");
	Serial.println(QTH_SERVER);
	Serial.print("Qth client ID: ");
	Serial.println(qth_client_id);
}

void setup_common() {
	setup_serial();
	setup_eeprom();
	setup_wifi();
	setup_qth();
}

void loop_common() {
	qth.loop();
	
	static bool last_connected = false;
	if (qth.connected() != last_connected) {
		Serial.print("Qth connection: ");
		last_connected = qth.connected();
		Serial.println(last_connected);
	}
}
