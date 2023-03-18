#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Servo.h>
#include <string.h>
#include <Qth.h>

// The control pin for the servo will be attached to this pin
#define SERVO_PIN D4
#define SERVO_MIN_PULSE_US 450
#define SERVO_MAX_PULSE_US 2450

// The pin for the LDR
#define LDR_PIN A0

// LDR low/high water mark (LDR reading)
#define LDR_LOW_WATER 500
#define LDR_HIGH_WATER 800

// Is the LDR inverted? (i.e. does a low LDR reading mean the boiler is on?)
#define LDR_INVERTED true

// Sample period of the LDR (ms)
#define LDR_SAMPLE_PERIOD 100

// Servo angles while the button is being pressed and released
#define SERVO_PRESSED_ANGLE 70
#define SERVO_RELEASED_ANGLE 0
#define SERVO_PRESS_DURATION 500

// Rate limit for changes (ms)
#define RATE_LIMIT (30 * 1000)

// Allow n changes in this many ms
#define N_CHANGES 10
#define N_CHANGE_RATE_LIMIT (10 * 60 * 60 * 1000)

// Allow re-trying pressing the button this many times
#define N_RETRIES 5

#define QTH_PREFIX "heating/hot_water"

const char *qth_client_id = "nodemcu_bathroom_board";
const char *qth_client_description = "Heating and bathroom stuff";
#include "common.inc"

////////////////////////////////////////////////////////////////////////////////


/**
 * Simple timeout timer.
 */
class Timeout {
	public:
		Timeout()
			: has_expired(true)
		{
			// Empty
		}
		
		/**
		 * Start (or restart) the timer counting down new_duration ms.
		 *
		 * Call expired() regularly until the timer expires (otherwise timer wrap
		 * around may occur).
		 */
		void reset(long new_duration) {
			end = millis() + new_duration;
			has_expired = false;
		}
		
		/**
		 * Call frequently after 'reset' is called. Returns 'true' once the timer
		 * has expired.
		 *
		 * NB: Once this function has returned 'true', the timer will not wrap
		 * around again and will not return 'false' until reset() is next called.
		 */
		bool expired() {
			if (!has_expired) {
				long now = millis();
				has_expired = (now - end) >= 0l;
			}
			
			return has_expired;
		}
	
	private:
		bool has_expired;
		long end;
};

/** 
 * Monitor the state of the LDR
 */
class LDRMonitor {
	public:
		LDRMonitor(int pin, int low_threshold, int high_threshold, bool inverted, long sample_interval)
			: pin(pin)
			, low_threshold(low_threshold)
			, high_threshold(high_threshold)
			, inverted(inverted)
			, sample_interval(sample_interval)
			, is_initialised(false)
			, state(false)
		{
		}
		
		/**
		 * Scan the LDR for state changes. Run regularly, or before reading
		 * get_state.
		 */
		void loop() {
			if (sample_timer.expired()) {
				const int adc = analogRead(pin);
				
				if (!is_initialised) {
					// Not initialised, set up state based on reality
					state = adc >= high_threshold;
					is_initialised = true;
				} else {
					// Already initialised, change state only if past a high/low water mark
					if (adc >= high_threshold) {
						state = true;
					} else if (adc <= low_threshold) {
						state = false;
					}
				}
				
				sample_timer.reset(sample_interval);
			}
		}
		
		/**
		 * Get the current LDR state.
		 */
		bool get_state() {
			// NB: != is boolean XOR
			return state != inverted;
		}
	
	private:
		// The Analogue pin to read
		const int pin;
		
		// The low and high water thresholds for the LDR ADC value
		const int low_threshold;
		const int high_threshold;
		
		// Should the reported state be inverted?
		const bool inverted;
		
		// Limit how frequently should the timer be polled to being polled no more
		// often than sample_interval ms.
		const long sample_interval;
		Timeout sample_timer;
		
		// Has 'loop' been called before? If not, initialisation must take place
		bool is_initialised;
		
		// The current state
		bool state;
};


/**
 * Drive the servo.
 */
class ServoControl {
	public:
		ServoControl(int pin, int up_position, int down_position, long press_duration)
			: pin(pin)
			, up_position(up_position)
			, down_position(down_position)
			, press_duration(press_duration)
			, state(State::idle)
			, servo()
		{
		}
		
		/**
		 * Call regularly.
		 */
		void loop() {
			if (timeout.expired()) {
				switch (state) {
					case State::press:
						enter_release_state();
						break;
					
					case State::release:
						enter_idle_state();
						break;
					
					default:
						// Nothing to do!
						break;
				}
			}
		}
		
		/**
		 * Cause the servo to cycle. Does nothing if servo is already actuating.
		 */
		void actuate() {
			if (state == State::idle) {
				enter_press_state();
			}
		}
		
		/**
		 * Is the servo currently actuating?
		 */
		bool idle() {
			return state == State::idle;
		}
	
	private:
		// Servo pin number
		const int pin;
		
		// Servo position for up (button not pressed) and down (button pressed)
		// servo positions.
		const int up_position;
		const int down_position;
		
		// Time (msec) to hold down the button (or hold it up) to allow time for
		// the servo to move and the button press to register.
		const long press_duration;
		
		// Current state of the servo
		enum struct State {idle, press, release} state;
		
		// Timeout used by state machine
		Timeout timeout;
		
		// Servo control
		Servo servo;
		
		void enter_press_state() {
			servo.attach(pin, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
			servo.write(down_position);
			delay(SERVO_PRESS_DURATION); // XXX
			Serial.print("Moving servo to ");
			Serial.println(down_position);
			state = State::press;
			timeout.reset(press_duration);
		}
		
		void enter_release_state() {
			servo.write(up_position);
			delay(SERVO_PRESS_DURATION); // XXX
			Serial.print("Moving servo to ");
			Serial.println(up_position);
			state = State::release;
			timeout.reset(press_duration);
		}
		
		void enter_idle_state() {
			servo.detach();
			delay(100); // XXX
			//digitalWrite(pin, LOW);
			state = State::idle;
		}
};



class HotWaterController {
	public:
		typedef void (*state_change_callback_t)(bool new_state);
		typedef void (*fault_callback_t)(const char *message);
		
		HotWaterController(int servo_pin, int ldr_pin,
		                   int up_position, int down_position, long press_duration,
		                   int ldr_low_threshold, int ldr_high_threshold, int ldr_inverted, long ldr_sample_period,
		                   long rate_limit, int n_changes, long n_change_rate_limit,
		                   long n_retries,
		                   state_change_callback_t state_changed_callback,
		                   fault_callback_t fault_callback)
			: servo(servo_pin, up_position, down_position, press_duration)
			, ldr(ldr_pin, ldr_low_threshold, ldr_high_threshold, ldr_inverted, ldr_sample_period)
			, rate_limit(rate_limit)
			, n_changes(n_changes)
			, n_change_rate_limit(n_change_rate_limit)
			, n_retries(n_retries)
			, state_changed_callback(state_changed_callback)
			, fault_callback(fault_callback)
			, state(State::idle)
			, next_state(false)
			, set_state_called(false)
		{
			pinMode(servo_pin, OUTPUT);
			digitalWrite(servo_pin, LOW);
			
			// Force a state change report on startup
			last_ldr_state = !ldr.get_state();
		}
		
		/**
		 * Call regularly.
		 */
		void loop() {
			servo.loop();
			ldr.loop();
			
			// Reset n_change rate limit as required
			if (n_change_rate_limit_timeout.expired()) {
				n_changes_remaining = n_changes;
				n_change_rate_limit_timeout.reset(n_change_rate_limit);
			}
			
			// Report LDR changes
			bool new_ldr_state = ldr.get_state();
			digitalWrite(LED_BUILTIN, !new_ldr_state);
			if (new_ldr_state != last_ldr_state) {
				last_ldr_state = new_ldr_state;
				state_changed_callback(new_ldr_state);
			}
			
			// Advance the main state machine
			switch (state) {
				case State::idle:
					if (set_state_called) {
						if (next_state != ldr.get_state()) {
							if (!--n_changes_remaining) {
								// State change rate limit hit, stop!
								state = State::fault_rate_limit_reached;
								fault_callback("FATAL: Rate limit reached.");
							} else {
								// Press the button
								state = next_state ? State::pressing_on : State::pressing_off;
								servo.actuate();
								rate_limit_timeout.reset(rate_limit);
								n_retries_remaining = n_retries;
							}
						 }
						 set_state_called = false;
					}
					break;
				
				case State::pressing_on:
				case State::pressing_off:
					if (servo.idle()) {
						// Press complete!
						if ((state == State::pressing_on) != ldr.get_state()) {
							// LDR didn't change as expected, fault condition after
							// n_retries!
							if (n_retries_remaining) {
								n_retries_remaining--;
								servo.actuate();
							} else {
								state = State::fault_button_press_failed;
								fault_callback("FATAL: Button press failed to change boiler state.");
							}
						} else {
							state = State::waiting;
						}
					}
					break;
				
				case State::waiting:
					if (rate_limit_timeout.expired()) {
						state = State::idle;
					}
					break;
				
				default:
					break;
			}
		}
		
		/**
		 * Call to set the desired state of the hot water heater.
		 *
		 * If the hot water state is already in the desired state, nothing will be
		 * done. If the state is different, the servo will be actuated.
		 *
		 * A rate limiting and fault-detection system is in place to avoid damage
		 * to the boiler.
		 */
		void set_state(bool new_state) {
			set_state_called = true;
			next_state = new_state;
		}
	
	private:
		ServoControl servo;
		LDRMonitor ldr;
		
		// The number of ms which must pass after a state change before another
		// is allowed.
		const long rate_limit;
		
		// The number of changes allowed within n_change_rate_limit milliseconds
		const int n_changes;
		const int n_change_rate_limit;
		
		// Number of times to re-try pressing the button before giving up
		const int n_retries;
		int n_retries_remaining;
		
		state_change_callback_t state_changed_callback;
		fault_callback_t fault_callback;
		
		enum struct State {
			// Idle state: waiting for either the LDR to report a change or set_state
			// to be called.
			idle,
			
			// If a fault occurs, one of these states will be entered. Faults are considered
			// permanent.
			fault_rate_limit_reached,  // n_changes rate limit hit
			fault_button_press_failed,  // the button press had no effect
			
			// The button is currently being pressed.
			pressing_on,
			pressing_off,
			
			// Waiting for the rate limit to expire before returning to idle
			waiting,
		} state;
		
		// If set_state has been called, set_state_called will be true and the
		// following will contain the desired state.
		bool next_state;
		bool set_state_called;
		
		// Timer counting down the rate limit timeout
		Timeout rate_limit_timeout;
		
		// Timer and counter for the n_changes per n_change_rate_limit rate-limit.
		int n_changes_remaining;
		
		// The previously observed LDR state
		bool last_ldr_state;
		Timeout n_change_rate_limit_timeout;
};


////////////////////////////////////////////////////////////////////////////////

Qth::Property *hot_water_state;
Qth::Property *hot_water_actual_state;
Qth::Property *hot_water_fault;
Qth::Event *move_servo;

HotWaterController *controller;

void on_hot_water_state_set(const char *topic, const char *json) {
	// Skip whitespace
	while (*json == ' ') {
		json++;
	}
	
	bool new_state = true;
	switch (*json) {
		case 'n': // null
		case 'f': // false
			new_state = false;
			break;
		
		case '0': // Maybe 0 or 0.0
			if (atof(json) == 0.0) {
				new_state = false;
			}
			break;
		
		case '\0': // Deleted
			// Don't do anything, return early
			return;
	}
	
	Serial.print("State change requested: ");
	Serial.println(new_state);
	controller->set_state(new_state);
}

void on_hot_water_state_changed(bool new_state) {
	qth.setProperty(hot_water_state, new_state ? "true" : "false");
	qth.setProperty(hot_water_actual_state, new_state ? "true" : "false");
	Serial.print("LED state changed: ");
	Serial.println(new_state);
}

void on_hot_water_fault(const char *message) {
	char message_quoted[strlen(message) + 3];
	message_quoted[0] = '"';
	message_quoted[strlen(message)+1] = '"';
	message_quoted[strlen(message)+2] = '\0';
	memcpy(message_quoted + 1, message, strlen(message));
	
	qth.setProperty(hot_water_fault, message_quoted);
}

void on_move_servo_called(const char *topic, const char *json) {
	Servo servo;
	servo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
	servo.write(atoi(json));
	Serial.print("Moving servo to ");
	Serial.println(atoi(json));
	Serial.print("LDR = ");
	Serial.println(analogRead(LDR_PIN));
	delay(500);
	servo.detach();
	digitalWrite(SERVO_PIN, LOW);
}


void setup() {
	// Force servo to rest position
	pinMode(SERVO_PIN, OUTPUT);
	digitalWrite(SERVO_PIN, LOW);
	{
		Servo s;
		s.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
		s.write(SERVO_RELEASED_ANGLE);
		delay(SERVO_PRESS_DURATION);
		s.detach();
	}
	pinMode(LED_BUILTIN, OUTPUT); // LED on ESP-12 board
	
	setup_common();
	
	hot_water_state = new Qth::Property(
		QTH_PREFIX,
		on_hot_water_state_set,
		"Set boiler hot water production state.",
		false, // false == N:1
		NULL // Don't delete on unregister
	);
	
	hot_water_actual_state = new Qth::Property(
		QTH_PREFIX"/actual-state",
		NULL,
		"Actual hot water production state.",
		true, // true == 1:N
		NULL // Don't delete on unregister
	);
	
	hot_water_fault = new Qth::Property(
		QTH_PREFIX"/fault",
		NULL,
		"What fault has occurred?",
		true, // true == 1:N
		NULL // Don't delete on unregister
	);
	
	move_servo = new Qth::Event(
		QTH_PREFIX"/move-servo",
		on_move_servo_called,
		"For calibration purposes.",
		false // false == N:1
	);
	
	qth.registerProperty(hot_water_state);
	qth.registerProperty(hot_water_actual_state);
	qth.registerProperty(hot_water_fault);
	qth.registerEvent(move_servo);
	
	qth.watchProperty(hot_water_state);
	qth.watchEvent(move_servo);
	qth.setProperty(hot_water_fault, "null");
	
	controller = new HotWaterController(
		SERVO_PIN, LDR_PIN,
		SERVO_RELEASED_ANGLE, SERVO_PRESSED_ANGLE, SERVO_PRESS_DURATION,
		LDR_LOW_WATER, LDR_HIGH_WATER, LDR_INVERTED, LDR_SAMPLE_PERIOD,
		RATE_LIMIT, N_CHANGES, N_CHANGE_RATE_LIMIT,
		N_RETRIES,
		on_hot_water_state_changed,
		on_hot_water_fault);
}

void loop() {
	loop_common();
	controller->loop();
}
