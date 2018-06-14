Utilities Monitoring Board
==========================

Monitors electricity and gas usage in my houes.

Electricity consumption is monitored using a light dependent resistor (LDR)
mounted in front of an LED on my electricity meter which blinks once per
watt-hour consumed. This is turned into an event
`power/electricity/watt-hour-consumed`.

Gas consumption is monitored using an RJ-11 socket on the bottom of my
(mechanical) gas meter which connects pins 3 and 4 every time a cubic foot of
gas is consumed. This is turned into an event `power/gas/cubic-foot-consumed`.
