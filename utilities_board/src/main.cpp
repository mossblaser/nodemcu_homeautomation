#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Qth.h>

// Prefix for all Qth paths
#define QTH_PATH_PREFIX "power/"

// Sample period (ms) for all sensors. By limiting the sampling rate we avoid
// having to deal with debouncing (in the case of the gas sensor) or large
// storage requirements (for the electricity sensor).
#define SENSOR_SAMPLE_PERIOD 50

// Pin number for gas sensor
//
//       +---------+
//       |Gas Meter|
//       |  o---o  |
//       +--|---|--+
//          |   |
//         Gnd Pin (with pull-up)
#define GAS_PIN D5

// Pin number of LDR attached to the electricity meter
//
//     +---+  ,------- +VCC
//     |LED) ||LDR    
//     |   |  '-+----- Analog Pin
//     +---+    |
//             _|_
//             |R|
//             '|'
//             Gnd
#define ELECTRICITY_PIN A0

// Window size (in samples) for electricity LDR values (see explanation below).
//
// When the LED on the electricity meter flashes, the LDR readings will look
// something like:
//
//     L       |
//     E       ||
//     V      ||||
//     E      ||||||
//     L ||||||||||||||||||||||||||||||||||||||||
//       TIME -->
//
// To detect the LED flashing we need to detect these pulses.
//
// The most naive approach is to look for sudden increases in the LDR reading.
// This approach has several shortcomings. Firstly it may double-trigger if the
// gradual leading rise happens to be just the wrong rate. In a similar way it
// may even miss the pulse if the leading rise is just a little too slow.
// Finally it may be falsely triggered by sudden ambient light changes, e.g.
// the room light being switched on.
//
// Instead, we capture a rolling window of the last ELECTRICITY_WINDOW
// readings. A pulse is detected when the difference between the peak value and
// the first and last values in the window exceed a suitable threshold
// (ELECTRICITY_PEAK_DELTA_THRESHOLD). This ensures only transient pulses are
// detected.
#define ELECTRICITY_WINDOW 10

// Minimum analogue reading delta between the start/end and peak reading during
// the window's interval to indicate a 'peak'
#define ELECTRICITY_PEAK_DELTA_THRESHOLD 100

const char *qth_client_id = "nodemcu_utilities_board";
const char *qth_client_description = "Utilities usage monitoring.";
#include "common.inc"

Qth::Event *electricity_pulse_evt;
Qth::Event *gas_pulse_evt;

void setup() {
	setup_common();
	
	pinMode(GAS_PIN, INPUT_PULLUP);
	
	electricity_pulse_evt = new Qth::Event(
		QTH_PATH_PREFIX"electricity/watt-hour-consumed",
		"Fires once per watt-hour consumed, with the number of milliseconds since the last pulse.");
	qth.registerEvent(electricity_pulse_evt);
	
	gas_pulse_evt = new Qth::Event(
		QTH_PATH_PREFIX"gas/cubic-foot-consumed",
		"Fires once per cubic-foot consumed, with the number of milliseconds since the last pulse.");
	qth.registerEvent(gas_pulse_evt);
}


/**
 * Call at SENSOR_SAMPLE_PERIOD to monitor the gas sensor.
 */
void loop_gas() {
	// NB: Inverted to get an active-high boolean
	bool this_state = !digitalRead(GAS_PIN);
	static bool last_state = false;
	
	static unsigned long last_pulse_ms = -1;
	
	// Positive-edge only
	if (this_state && !last_state) {
		unsigned long ms_since_last_pulse = 0;
		unsigned long now = millis();
		if (last_pulse_ms != -1) {
			ms_since_last_pulse = now - last_pulse_ms;
		}
		last_pulse_ms = now;
		
		// NB: Don't send first zero-containing reading since it will confuse
		// things taking a reciprocal.
		if (ms_since_last_pulse) {
			char buf[50];
			snprintf(buf, sizeof(buf), "%lu", ms_since_last_pulse);
			qth.sendEvent(gas_pulse_evt, buf);
		}
	}
	
	last_state = this_state;
}


/**
 * Call at SENSOR_SAMPLE_PERIOD to monitor the electricity sensor.
 *
 * See comment above 'ELECTRICITY_WINDOW' for explanation of the mechanism
 * used.
 */
void loop_electricity() {
	// A circular buffer containing the window. The intial value of '-1' is used
	// as a sentinel below to trigger initialisation on the first call to this
	// function.
	static int readings[ELECTRICITY_WINDOW] = {-1}; 
	static int next_reading_index = 0;
	
	int reading = analogRead(ELECTRICITY_PIN);
	
	// Initialise window on startup
	if (readings[0] == -1) {
		for (int i = 0; i < ELECTRICITY_WINDOW; i++) {
			readings[i] = reading;
		}
	}
	
	// Add new reading to window
	readings[next_reading_index] = reading;
	if (++next_reading_index >= ELECTRICITY_WINDOW) {
		next_reading_index = 0;
	}
	
	// Check for a pulse
	int window_oldest_reading = readings[next_reading_index];
	int window_newest_reading = reading;
	int window_max_reading = window_oldest_reading;
	for (int i = 0; i < ELECTRICITY_WINDOW; i++) {
		if (readings[i] > window_max_reading) {
			window_max_reading = readings[i];
		}
	}
	
	if (window_max_reading - window_oldest_reading > ELECTRICITY_PEAK_DELTA_THRESHOLD &&
	    window_max_reading - window_newest_reading > ELECTRICITY_PEAK_DELTA_THRESHOLD) {
		// Reset the window to prevent this pulse being reported several times
		for (int i = 0; i < ELECTRICITY_WINDOW; i++) {
			readings[i] = reading;
		}
		
		static unsigned long last_pulse_ms = -1;
		unsigned long now = millis();
		unsigned long ms_since_last_pulse = 0;
		if (last_pulse_ms != -1) {
			ms_since_last_pulse = now - last_pulse_ms;
		}
		last_pulse_ms = now;
		
		// NB: Don't send first zero-containing reading since it will confuse
		// things taking a reciprocal.
		if (ms_since_last_pulse) {
			char buf[50];
			snprintf(buf, sizeof(buf), "%lu", ms_since_last_pulse);
			qth.sendEvent(electricity_pulse_evt, buf);
		}
	}
}


void loop() {
	loop_common();
	
	static unsigned long last_sample = 0;
	unsigned long now = millis();
	if (now - last_sample > SENSOR_SAMPLE_PERIOD) {
		loop_gas();
		loop_electricity();
		last_sample = now;
	}
}
