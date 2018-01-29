#include <Arduino.h>
#include <EEPROM.h>
#include <jsmn.h>
#include <ESP8266WiFi.h>
#include <Qth.h>
#include <FourThreeThree.h>

// Prefix for all Qth paths
#define QTH_PATH_PREFIX "sys/433mhz/"

const char *qth_client_id = "nodemcu_radio_board";
const char *qth_client_description = "Qth proxy for 433 MHz radio devices.";

const int rx_pin = D1;
const int tx_pin = D2;

Qth::EEPROMProperty *rx_codes_prop;
Qth::Event rx_unknown_code_event(QTH_PATH_PREFIX"rx_unknown_code", NULL,
                                 "Got an unknown code: [code, length].");

Qth::EEPROMProperty *tx_codes_prop;

// Number of sequential receipts of the same unknown code to receive before
// reporting it via Qth
const int UNKNOWN_CODE_REPEAT_COUNT = 4;

// Minimum code length to bother reporting
const int UNKNOWN_CODE_MIN_LENGTH = 10;

// Minimum interval (ms) between events for the same code
const int MIN_INTER_EVENT_TIME = 3000;

#include "common.inc"

typedef struct {
	char *qth_path;
	unsigned long code;
	unsigned int code_length;
	Qth::Event *event;
	unsigned long last_event_time;
} rx_code_t;

// The set of codes currently registered in the Qth sys/433mhz/rx_codes
// property.
size_t num_rx_codes = 0;
rx_code_t *rx_codes = NULL;

void on_rx_codes_changed(const char *topic, const char *value) {
	// Remove all old registrations
	for (size_t i = 0; i < num_rx_codes; i++) {
		qth.unregisterEvent(rx_codes[i].event);
		delete rx_codes[i].event;
		delete [] rx_codes[i].qth_path;
	}
	delete [] rx_codes;
	num_rx_codes = 0;
	rx_codes = NULL;
	
	// Parse incoming JSON specification
	jsmn_parser parser;
	
	jsmn_init(&parser);
	int num_tokens = jsmn_parse(&parser, value, strlen(value), NULL, 0);
	if (num_tokens < 1) {
		// Bad JSON, stop now
		Serial.print("Bad JSON in rx_codes (");
		Serial.print(num_tokens);
		Serial.print("): ");
		Serial.println(value);
		return;
	}
	
	jsmntok_t *tokens = new jsmntok_t[num_tokens];
	jsmn_init(&parser);
	num_tokens = jsmn_parse(&parser, value, strlen(value), tokens, num_tokens);
	if (num_tokens < 1) {
		// Bad JSON, stop now
		Serial.print("Bad JSON in rx_codes (");
		Serial.print(num_tokens);
		Serial.print("): ");
		Serial.println(value);
		return;
	}
	
	if (tokens[0].type != JSMN_OBJECT) {
		// Expected an object, give up
		Serial.print("Expected object in rx_codes (got ");
		Serial.print(tokens[0].type);
		Serial.println(",");
		Serial.print(tokens[0].start);
		Serial.println(",");
		Serial.print(tokens[0].end);
		Serial.println(",");
		Serial.print(tokens[0].size);
		Serial.println(")");
		delete tokens;
		return;
	}
	
	
	// Count and verify the entries of the provided object
	size_t num_entries = tokens[0].size;
	for (size_t i = 1; i < (num_entries*4) + 1; i += 4) {
		if (tokens[i+0].type != JSMN_STRING ||     // Expect a name
		    tokens[i+1].type != JSMN_ARRAY ||      // Expect an array
		    tokens[i+1].size != 2 ||               // ...of length 2
		    tokens[i+2].type != JSMN_PRIMITIVE ||  // Both elements should be
		    value[tokens[i+2].start] == 't' ||     // numbers.
		    value[tokens[i+2].start] == 'f' ||
		    value[tokens[i+2].start] == 'n' ||
		    tokens[i+3].type != JSMN_PRIMITIVE ||
		    value[tokens[i+3].start] == 't' ||
		    value[tokens[i+3].start] == 'f' ||
		    value[tokens[i+3].start] == 'n') {
			// Expected a "name": [code, code_length] entry, give up!
			delete tokens;
			Serial.print("Expected rx_code entry at offset ");
			Serial.print(i);
			Serial.println(" to be two-arrays of integers.");
			return;
		}
	}
	
	num_rx_codes = num_entries;
	rx_codes = new rx_code_t[num_rx_codes];
	
	// Create and register all RX events
	rx_code_t *rx_code = rx_codes;
	for (size_t i = 1; i < (num_entries*4)+1; i += 4, rx_code++) {
		// Get event path name
		const char *name = value + tokens[i].start;
		size_t name_length = tokens[i].end - tokens[i].start;
		rx_code->qth_path = new char[name_length + 1];
		memcpy(rx_code->qth_path, name, name_length);
		rx_code->qth_path[name_length] = '\0';
		
		// Create and register the event
		rx_code->event = new Qth::Event(rx_code->qth_path, NULL, "433 MHz receiver");
		qth.registerEvent(rx_code->event);
		
		// Get code
		const char *code_str = value + tokens[i+2].start;
		rx_code->code = strtoul(code_str, NULL, 10);
		
		// Get code length
		const char *code_length_str = value + tokens[i+3].start;
		rx_code->code_length = (unsigned int)strtoul(code_length_str, NULL, 10);
		
		rx_code->last_event_time = 0ul;
	}
	
	delete tokens;
}


typedef struct {
	char *qth_path;
	unsigned long on_code;
	unsigned long off_code;
	unsigned int code_length;
	Qth::Property *property;
	// Is this command waiting to be sent?
	bool waiting;
	// Which command (on or off) should be sent when it comes to this code's
	// turn?
	bool state;
} tx_code_t;

// The set of codes currently registered in the Qth sys/433mhz/rx_codes
// property.
size_t num_tx_codes = 0;
tx_code_t *tx_codes = NULL;

void on_tx_code_set(const char *topic, const char *value) {
	// Determine the desired state
	// XXX: Ideally this would use a full JSON parsing pass to determine if the
	// value is 'truthy'...
	bool state;
	if (strcmp(value, "true") == 0) {
		state = true;
	} else if (strcmp(value, "false") == 0) {
		state = false;
	} else {
		// Invalid value, ignore!
		return;
	}
	
	// Send the code
	for (size_t i = 0; i < num_tx_codes; i++) {
		if (strcmp(tx_codes[i].qth_path, topic) == 0) {
			tx_codes[0].waiting = true;
			tx_codes[0].state = state;
		}
	}
}

void on_tx_codes_changed(const char *topic, const char *value) {
	// Remove all old registrations
	for (size_t i = 0; i < num_tx_codes; i++) {
		qth.unregisterProperty(tx_codes[i].property);
		qth.unwatchProperty(tx_codes[i].property);
		qth.setProperty(tx_codes[i].property, ""); // Delete the property
		delete tx_codes[i].property;
		delete [] tx_codes[i].qth_path;
	}
	delete [] tx_codes;
	num_tx_codes = 0;
	tx_codes = NULL;
	
	// Parse incoming JSON specification
	jsmn_parser parser;
	
	jsmn_init(&parser);
	int num_tokens = jsmn_parse(&parser, value, strlen(value), NULL, 0);
	if (num_tokens < 1) {
		// Bad JSON, stop now
		Serial.print("Bad JSON in tx_codes (");
		Serial.print(num_tokens);
		Serial.print("): ");
		Serial.println(value);
		return;
	}
	
	jsmntok_t *tokens = new jsmntok_t[num_tokens];
	jsmn_init(&parser);
	num_tokens = jsmn_parse(&parser, value, strlen(value), tokens, num_tokens);
	if (num_tokens < 1) {
		// Bad JSON, stop now
		Serial.print("Bad JSON in tx_codes (");
		Serial.print(num_tokens);
		Serial.print("): ");
		Serial.println(value);
		return;
	}
	
	if (tokens[0].type != JSMN_OBJECT) {
		// Expected an object, give up
		Serial.print("Expected object in tx_codes (got ");
		Serial.print(tokens[0].type);
		Serial.println(",");
		Serial.print(tokens[0].start);
		Serial.println(",");
		Serial.print(tokens[0].end);
		Serial.println(",");
		Serial.print(tokens[0].size);
		Serial.println(")");
		delete tokens;
		return;
	}
	
	
	// Count and verify the entries of the provided object
	size_t num_entries = tokens[0].size;
	for (size_t i = 1; i < (num_entries*5) + 1; i += 5) {
		if (tokens[i+0].type != JSMN_STRING ||     // Expect a name
		    tokens[i+1].type != JSMN_ARRAY ||      // Expect an array
		    tokens[i+1].size != 3 ||               // ...of length 3
		    tokens[i+2].type != JSMN_PRIMITIVE ||  // All elements should be
		    value[tokens[i+2].start] == 't' ||     // numbers.
		    value[tokens[i+2].start] == 'f' ||
		    value[tokens[i+2].start] == 'n' ||
		    tokens[i+3].type != JSMN_PRIMITIVE ||
		    value[tokens[i+3].start] == 't' ||
		    value[tokens[i+3].start] == 'f' ||
		    value[tokens[i+3].start] == 'n' ||
		    tokens[i+4].type != JSMN_PRIMITIVE ||
		    value[tokens[i+4].start] == 't' ||
		    value[tokens[i+4].start] == 'f' ||
		    value[tokens[i+4].start] == 'n') {
			// Expected a "name": [on_code, off_code, code_length] entry, give up!
			delete tokens;
			Serial.print("Expected tx_code entry at offset ");
			Serial.print(i);
			Serial.println(" to be two-arrays of integers.");
			return;
		}
	}
	
	num_tx_codes = num_entries;
	tx_codes = new tx_code_t[num_tx_codes];
	
	// Create and register all TX events
	tx_code_t *tx_code = tx_codes;
	for (size_t i = 1; i < (num_entries*5)+1; i += 5, tx_code++) {
		// Get event path name
		const char *name = value + tokens[i].start;
		size_t name_length = tokens[i].end - tokens[i].start;
		tx_code->qth_path = new char[name_length + 1];
		memcpy(tx_code->qth_path, name, name_length);
		tx_code->qth_path[name_length] = '\0';
		
		// Create and register the property
		tx_code->property = new Qth::Property(tx_code->qth_path,
		                                      on_tx_code_set,
		                                      "433 MHz code TX.",
		                                      false);
		qth.registerProperty(tx_code->property);
		qth.watchProperty(tx_code->property);
		qth.setProperty(tx_code->property, "null");
		
		// Get code
		const char *on_code_str = value + tokens[i+2].start;
		const char *off_code_str = value + tokens[i+3].start;
		tx_code->on_code = strtoul(on_code_str, NULL, 10);
		tx_code->off_code = strtoul(off_code_str, NULL, 10);
		
		// Get code length
		const char *code_length_str = value + tokens[i+4].start;
		tx_code->code_length = (unsigned int)strtoul(code_length_str, NULL, 10);
		
		// Initially not sending anything
		tx_code->waiting = false;
	}
	
	delete tokens;
}



//void test_tx(const char *topic, const char *json) {
//	jsmn_parser parser;
//	jsmn_init(&parser);
//	jsmntok_t tokens[3];  // Long enough for an array with two ints
//	int num_tokens = jsmn_parse(&parser, json, strlen(json), tokens, 3);
//	
//	if (num_tokens != 3 ||
//	    tokens[0].type != JSMN_ARRAY ||
//	    tokens[0].size != 2 ||
//	    tokens[1].type != JSMN_PRIMITIVE ||
//	    tokens[1].type == 't' ||     // numbers.
//	    tokens[1].type == 'f' ||
//	    tokens[1].type == 'n' ||
//	    tokens[2].type != JSMN_PRIMITIVE ||
//	    tokens[2].type == 't' ||     // numbers.
//	    tokens[2].type == 'f' ||
//	    tokens[2].type == 'n') {
//		Serial.print("Invalid JSON: ");
//		Serial.println(json);
//		return;
//	}
//	
//	unsigned long code = (unsigned long)strtoul(json + tokens[1].start, NULL, 10);
//	unsigned int code_length = (unsigned int)strtoul(json + tokens[2].start, NULL, 10);
//	if (FourThreeThree_tx(code, code_length)) {
//		Serial.print("Sending code ");
//		Serial.print(code);
//		Serial.print(" (length ");
//		Serial.print(code_length);
//		Serial.println(")");
//	} else {
//		Serial.println("Couldn't transmit code: already busy transmitting!");
//	}
//}


void setup() {
	setup_common();
	
	FourThreeThree_rx_begin(rx_pin);
	FourThreeThree_tx_begin(tx_pin);
	
	rx_codes_prop = new Qth::EEPROMProperty(
		QTH_PATH_PREFIX"rx_codes",
		MQTT_MAX_PACKET_SIZE,  // EPROM Block Length
		0,  // EPROM Start addr
		"Codes to listen for. {\\\"qth_path\\\": [code, length], ...}.",
		false,
		"",
		on_rx_codes_changed);
	
	tx_codes_prop = new Qth::EEPROMProperty(
		QTH_PATH_PREFIX"tx_codes",
		MQTT_MAX_PACKET_SIZE,  // EPROM Block Length
		MQTT_MAX_PACKET_SIZE,  // EPROM Start addr
		"On/off codes to make properties for. {\\\"qth_path\\\": [on_code, off_code, length], ...}.",
		false,
		"",
		on_tx_codes_changed);
	
	qth.registerProperty(rx_codes_prop);
	qth.registerProperty(tx_codes_prop);
	qth.watchProperty(rx_codes_prop);
	qth.watchProperty(tx_codes_prop);
	
	qth.registerEvent(&rx_unknown_code_event);
}


rx_code_t *find_rx_code(unsigned long code, unsigned int code_length) {
	for (size_t i = 0; i < num_rx_codes; i++) {
		if (rx_codes[i].code == code && rx_codes[i].code_length == code_length) {
			return rx_codes + i;
		}
	}
	
	return NULL;
}

void loop() {
	loop_common();
	FourThreeThree_tx_loop();
	
	static unsigned long last_code = 0;
	static unsigned int last_code_length = 0;
	static unsigned int last_code_repeats = 0;
	
	// Try and receive
	unsigned long code;
	unsigned int code_length;
	if (FourThreeThree_rx(&code, &code_length)) {
		rx_code_t *rx_code = find_rx_code(code, code_length);
		if (last_code == code && last_code_length == code_length) {
			last_code_repeats++;
		} else {
			last_code = code;
			last_code_length = code_length;
			last_code_repeats = 1;
		}
		
		if (rx_code) {
			// Known code, only send out events for the first occurrence
			unsigned long now = millis();
			unsigned long ellapsed = now - rx_code->last_event_time;
			if (ellapsed >= MIN_INTER_EVENT_TIME) {
				qth.sendEvent(rx_code->event, "null");
				rx_code->last_event_time = now;
			}
		} else if (last_code_repeats == UNKNOWN_CODE_REPEAT_COUNT &&
		           last_code_length >= UNKNOWN_CODE_MIN_LENGTH) {
			// Unknown code (only issue after it has been seen several times in
			// succession to reduce chances of it being noise)
			char *buf = new char[16];
			snprintf(buf, 16, "[%ld,%d]", code, code_length);
			qth.sendEvent(&rx_unknown_code_event, buf);
			delete buf;
		}
	}
	
	// Try and transmit
	for (size_t i = 0; i < num_tx_codes; i++) {
		if (tx_codes[i].waiting &&
		    FourThreeThree_tx(tx_codes[i].state ? tx_codes[i].on_code : tx_codes[i].off_code,
		                      tx_codes[i].code_length)) {
			tx_codes[i].waiting = false;
		}
	}
}
