#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <Wire.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <SoftwareSerial.h>
#include <Adafruit_HTU21DF.h>

#define RX2 13
#define TX2 15
#define LED 14
#define BUT 16
#define MOT 0

Adafruit_HTU21DF htu = Adafruit_HTU21DF(); // I2C

SoftwareSerial gasSerial(RX2, TX2); // RX, TX

// Define default values here, if there are different values in config.json, they are overwritten.

char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_user[20];
char mqtt_pw[20];
char temp_offset[6] = "0.0";
char looptime[6] = "10";
char cooldown[10] = "5000";


char buf[25];
int counter = 0;
float remTime;
float prevMot = millis();
bool motionReported = false;
bool readyToSense = true;

// Flag for saving data
bool shouldSaveConfig = false;

WiFiClient espClient;
PubSubClient client(espClient);

// Callback notifying us of the need to save config
void saveConfigCallback() {
	Serial.println(F("Should save config"));
	shouldSaveConfig = true;
}


void setup() {
	// put your setup code here, to run once:
	Serial.begin(115200);
	gasSerial.begin(9600);

	Serial.println(F("Starting multisensor"));

	pinMode(BUT,  INPUT);
	pinMode(LED, OUTPUT);
	pinMode(MOT,  INPUT);

	// Initialize to empty
	mqtt_server[0] = 0;
	mqtt_user[0]   = 0;
	mqtt_pw[0]	   = 0;

	// Connect to sensors
	/*while (!gasSerial.available()) {
		Serial.println(F("Could not find a valid Mh-Z19 sensor, check wiring!"));
		digitalWrite(LED, HIGH);
		delay(500);
		digitalWrite(LED, LOW);
		delay(500);
	}*/
	while (!htu.begin()) {
		Serial.println(F("Could not find a valid HTU21 sensor, check wiring!"));
		digitalWrite(LED, HIGH);
		delay(1000);
		digitalWrite(LED, LOW);
		delay(1000);
	}

	delay(1000);

	setupFS();
	setupWifiParams(!SPIFFS.exists(F("/config.json")));

	digitalWrite(LED, HIGH);
}

void setupFS() {
	// Clean FS, for testing
    //SPIFFS.format();

	// Read configuration from FS json
	Serial.println(F("mounting FS..."));

	if (SPIFFS.begin()) {
		Serial.println(F("mounted file system"));
		if (SPIFFS.exists(F("/config.json"))) {
			// File exists, reading and loading
			Serial.println("reading config file");
			File configFile = SPIFFS.open("/config.json", "r");
			if (configFile) {
				Serial.println(F("opened config file"));
				size_t size = configFile.size();
				// Allocate a buffer to store contents of the file.
				std::unique_ptr<char[]> buf(new char[size]);

				configFile.readBytes(buf.get(), size);
				DynamicJsonBuffer jsonBuffer;
				JsonObject& json = jsonBuffer.parseObject(buf.get());
				json.printTo(Serial);

				if (json.success()) {
					Serial.println(F("\nparsed json"));

					strcpy(mqtt_server, json[F("mqtt_server")]);
					strcpy(mqtt_port,   json[F("mqtt_port")]);
					strcpy(mqtt_user,   json[F("mqtt_server")]);
					strcpy(mqtt_pw,     json[F("mqtt_port")]);
					strcpy(temp_offset, json[F("temp_offset")]);
					strcpy(looptime,    json[F("looptime")]);
					strcpy(cooldown,    json[F("cooldown")]);

					Serial.println(F("1"));

				}
				else {
					Serial.println(F("failed to load json config"));
				}
			}
		}
	}
	else {
		Serial.println(F("failed to mount FS"));
	}
	// End read

}

void setupWifiParams(boolean reset) {
	boolean ownAP = true;

	// The extra parameters to be configured (can be either global or just in the setup)
	// After connecting, parameter.getValue() will get you the configured value
	// id/name placeholder/prompt default length

	WiFiManagerParameter custom_mqtt_server("server",      "mqtt server",    mqtt_server, 40);
	WiFiManagerParameter custom_mqtt_port  ("port",        "mqtt port",      mqtt_port,    6);
	WiFiManagerParameter custom_mqtt_user  ("username",    "mqtt user",      mqtt_user,   20);
	WiFiManagerParameter custom_mqtt_pw    ("password",    "mqtt password",  mqtt_pw,     20);
	WiFiManagerParameter custom_temp_offset("calibration", "temp offset",    temp_offset,  6);
	WiFiManagerParameter custom_looptime   ("looptime",    "looptime",       looptime,     3);
	WiFiManagerParameter custom_cooldown ("cooldown",      "Cooldown",       cooldown,    10);
	//WiFiManager
	//Local intialization. Once its business is done, there is no need to keep it around
	WiFiManager wifiManager;

	//set config save notify callback
	wifiManager.setSaveConfigCallback(saveConfigCallback);

	//add all parameters here
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_user);
	wifiManager.addParameter(&custom_mqtt_pw);
	wifiManager.addParameter(&custom_temp_offset);
	wifiManager.addParameter(&custom_looptime);
	wifiManager.addParameter(&custom_cooldown);

	//Checks if supposed to reset connection, if so starts portal. Otherwise autoconnect
	if (reset) {
		ownAP = wifiManager.startConfigPortal("LaeretSensor", "administrator");
	}
	else {
		ownAP = wifiManager.autoConnect("LaeretSensor", "administrator");
	}

	if (!ownAP) {
		Serial.println(F("failed to connect and hit timeout"));
		delay(3000);
		// Reset and try again, or maybe put it to deep sleep
		ESP.reset();
		delay(5000);
	}

	// If you get here you have connected to the WiFi
	Serial.println(F("connected..."));

	// Read updated parameters
	strcpy(mqtt_server, custom_mqtt_server.getValue());
	strcpy(mqtt_port, custom_mqtt_port.getValue());
	strcpy(mqtt_user, custom_mqtt_user.getValue());
	strcpy(mqtt_pw, custom_mqtt_pw.getValue());
	strcpy(temp_offset, custom_temp_offset.getValue());
	strcpy(looptime, custom_looptime.getValue());
	strcpy(cooldown, custom_cooldown.getValue());

	//save the custom parameters to FS
	if (shouldSaveConfig) {

		Serial.println(F("saving config"));
		DynamicJsonBuffer jsonBuffer;
		JsonObject& json = jsonBuffer.createObject();

		json[F("mqtt_server")] = mqtt_server;
		json[F("mqtt_port")]   = mqtt_port;
		json[F("mqtt_user")]   = mqtt_user;
		json[F("mqtt_pw")]	   = mqtt_pw;
		json[F("temp_offset")] = temp_offset;
		json[F("looptime")]    = looptime;
		json[F("cooldown")]    = cooldown;

		File configFile = SPIFFS.open("/config.json", "w");
		if (!configFile) {
			Serial.println(F("failed to open config file for writing"));
		}

		json.printTo(Serial);
		json.printTo(configFile);
		configFile.close();
		// End save
	}

	Serial.println(F("local ip"));
	Serial.println(WiFi.localIP());


	client.setServer(mqtt_server, atoi(mqtt_port));
	client.setCallback(callback);
}

void callback(char* topic, byte* payload, unsigned int length) {
	Serial.print(F("Message arrived ["));
	Serial.print(topic);
	Serial.print(F("] "));
	for (int i = 0; i < length; i++) {
		Serial.print((char)payload[i]);
	}
	Serial.println();
}


void reconnect() {
	boolean conn_b = false;

	// Loop until we're reconnected
	while (!client.connected()) {
		Serial.print(F("Connect..."));

		// Attempt to connect
		sprintf(buf, "MQTT%i", ESP.getChipId());
		Serial.println(buf);
		
		// Connect to MQTT broker
		if ((mqtt_user[0] == 0) || (mqtt_pw[0] = 0)) {
			conn_b = client.connect(buf);
		}
		else {
			conn_b = client.connect(buf, mqtt_user, mqtt_pw);
		}

		if (conn_b == true) {
			Serial.println(F("connected"));

			// Once connected, publish an announcement...
			memset(buf, 0, sizeof(buf));
			sprintf(buf, "Temp%i", ESP.getChipId());
			client.subscribe(buf);

			memset(buf, 0, sizeof(buf));
			sprintf(buf, "Humid%i", ESP.getChipId());
			client.subscribe(buf);

			memset(buf, 0, sizeof(buf));
			sprintf(buf, "CO2%i", ESP.getChipId());
			client.subscribe(buf);

			memset(buf, 0, sizeof(buf));
			sprintf(buf, "Mot%i", ESP.getChipId());
			client.subscribe(buf);
		}
		else {
			buttonListener();
			Serial.print(F("Failed, rc="));
			Serial.print(client.state());
			Serial.println(F(" trying again in 1 seconds"));
		}
	}
}

int readCO2()
{

	byte cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
	// command to ask for data
	char response[9]; // for answer

	gasSerial.write(cmd, 9); //request PPM CO2
	gasSerial.readBytes(response, 9);
	if (response[0] != 0xFF)
	{
		Serial.println("Wrong starting byte from co2 sensor!");
		return -1;
	}

	if (response[1] != 0x86)
	{
		Serial.println("Wrong command from co2 sensor!");
		return -1;
	}

	int responseHigh = (int)response[2];
	int responseLow = (int)response[3];
	int ppm = (256 * responseHigh) + responseLow;
	return ppm;
}

void buttonListener() {
	int val = digitalRead(BUT);
	//Serial.println(val);
	if (val) {
		counter++;
		if (counter == 2) {
			counter = 0;
			Serial.println(F("Resetting"));
			digitalWrite(LED, LOW);
			delay(100);
			digitalWrite(LED, HIGH);
			delay(100);
			digitalWrite(LED, LOW);
			setupFS();
			setupWifiParams(true);
			delay(100);
			digitalWrite(LED, HIGH);
		}
	}
	else {
		counter = 0;
	}
}

void loop() {
	float motTimeOut = atof(cooldown);
	float startTime = millis();

	// Detect disconnected unit
	if (!client.connected()) {
		reconnect();
	}

	buttonListener();

	// Detect motion event
	int motRead = digitalRead(MOT);

	if (motionReported) {
		if (millis() > (prevMot + motTimeOut)) {
			client.loop();
			Serial.println(F("Motion cooled off"));
			memset(buf, 0, sizeof(buf));
			sprintf(buf, "Mot%i", ESP.getChipId());
			Serial.println(buf);
			if (client.publish(buf, "OFF", true)) {
				readyToSense = true;
				motionReported = false;
			}
		}
	}

	if (motRead && readyToSense) {
		if (millis() > (prevMot + motTimeOut)) {
			client.loop();
			Serial.println(F("Detected motion"));
			memset(buf, 0, sizeof(buf));
			sprintf(buf, "Mot%i", ESP.getChipId());
			Serial.println(buf);
			if (client.publish(buf, "ON", true)) {
				prevMot = millis();
				motionReported = true;
				readyToSense = false;
			}
		}
	}

	if (remTime < millis()) {
		remTime = atof(looptime)*1000.0 + startTime;
		client.loop();
		
		memset(buf, 0, sizeof(buf));
		sprintf(buf, "Temp%i", ESP.getChipId());
		client.publish(buf, String((htu.readTemperature() + atof(temp_offset))).c_str());

		memset(buf, 0, sizeof(buf));
		sprintf(buf, "Humid%i", ESP.getChipId());
		client.publish(buf, String(htu.readHumidity()).c_str());

		memset(buf, 0, sizeof(buf));
		sprintf(buf, "CO2%i", ESP.getChipId());
		client.publish(buf, String(readCO2()).c_str());
	}
	
	
}
