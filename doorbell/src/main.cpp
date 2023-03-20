#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Qth.h>

#define QTH_PREFIX "hall/doorbell"

const char *qth_client_id = "nodemcu_doorbell";
const char *qth_client_description = "Doorbell monitor";
#include "common.inc"

const int input_pin = A0;
const float voltage_divider = 38.0 / (38.0 + 64.0);

const int adc_max = 1023;
const int adc_pressed_threshold = 50;

////////////////////////////////////////////////////////////////////////////////

Qth::Property *voltage_property;
Qth::Event *doorbell_event;

void setup() {
	setup_common();
	
	voltage_property = new Qth::Property(
		QTH_PREFIX "/battery_voltage",
		"Voltage of doorbell battery pack whilst shorted across solenoid.",
		true // true == 1:N
	);
	qth.registerProperty(voltage_property);
	qth.setProperty(voltage_property, "null");
	
	doorbell_event = new Qth::Event(
		QTH_PREFIX,
		"Fired when doorbell pressed (True) or released (False)",
		true // true == 1:N
	);
	qth.registerEvent(doorbell_event);
}

void slow_loop() {  // analogRead calls must be rate limited!
	static int pressed_adc_histogram[adc_max + 1];
	static int pressed_adc_histogram_count;
	
	static int last_adc = 0;
	int adc = analogRead(input_pin);
	
	bool last_pressed = last_adc >= adc_pressed_threshold;
	bool pressed = adc >= adc_pressed_threshold;
	bool newly_pressed = pressed && !last_pressed;
	bool newly_released = !pressed && last_pressed;
	
	last_adc = adc;
	
	// Update ADC value histogram while pressed
	if (newly_pressed) {
		for (int i = 0; i < adc_max + 1; i++) {
			pressed_adc_histogram[i] = 0;
		}
		pressed_adc_histogram_count = 0;
	}
	if (pressed) {
		pressed_adc_histogram[adc <= adc_max ? adc : adc_max] += 1;
		pressed_adc_histogram_count ++;
	}
	
	// Send events/set properties
	if (newly_pressed) {
		Serial.println("Doorbell pressed...");
		qth.sendEvent(doorbell_event, "true");
	}
	if (newly_released) {
		Serial.println("Doorbell released...");
		qth.sendEvent(doorbell_event, "false");
		
		// Find median ADC value from histogram
		int adc_median = 0;
		int cum_sum = 0;
		while (cum_sum < pressed_adc_histogram_count / 2) {
			cum_sum += pressed_adc_histogram[adc_median++];
		}
		Serial.print("adc_median = "); Serial.println(adc_median);
		
		float voltage = (adc_median / (float)adc_max) * 3.3 / voltage_divider;
		Serial.print("voltage = "); Serial.println(voltage);
		
		String voltage_str = String(voltage);
		qth.setProperty(voltage_property, voltage_str.c_str());
	}
}

void loop() {
	loop_common();
	
	static unsigned long last_slow_loop = 0;
	unsigned long now = millis();
	if (now - last_slow_loop > 10) {
		slow_loop();
		last_slow_loop = now;
	}
}
