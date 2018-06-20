"""
Quick Qth client which produces more useful metrics from the measurements from
this NodeMCU device.
"""

from qth_yarp import watch_event, set_property, run_forever
import yarp

################################################################################
# Convert electricity events into more sensible units
################################################################################

watt_hour_consumed = watch_event("power/electricity/watt-hour-consumed")

ms_per_watt_hour = yarp.make_persistent(watt_hour_consumed, float("inf"))
s_per_watt_hour = ms_per_watt_hour / 1000.0
watts = round((60 * 60) / s_per_watt_hour)

set_property("power/electricity/power-consumption",
             watts / 1000.,
             register=True,
             description="Current rate of electricity consumption in kilowatts",
             delete_on_unregister=True)

electricity_watt_hours_in_last_24_hours = yarp.len(yarp.time_window(watt_hour_consumed, 60*60*24))

set_property("power/electricity/24-hour-usage",
             electricity_watt_hours_in_last_24_hours / 1000.0,
             register=True,
             description="Number of kilowatt hours consumed in the past 24 hours.",
             delete_on_unregister=True)


################################################################################
# Convert gas events into more sensible units
################################################################################

cubic_foot_consumed = watch_event("power/gas/cubic-foot-consumed")

cubic_meters_per_cubic_foot = 0.028316847

# Account for atmospheric preassure -- industry defined quantity
conversion_factor = 1.02264

# Megajoules per unit meter of gas -- defined by industry according to quantity
# of energy in the gas.
calorific_value = 39.3

megajoules_per_kwh = 3.6

kwh_per_cubic_meter = (calorific_value / megajoules_per_kwh) * conversion_factor
kwh_per_cubic_foot = kwh_per_cubic_meter * cubic_meters_per_cubic_foot

gas_kwh_in_last_24_hours = yarp.len(yarp.time_window(cubic_foot_consumed, 60*60*24)) * kwh_per_cubic_foot

set_property("power/gas/24-hour-usage",
             gas_kwh_in_last_24_hours,
             register=True,
             description="Number of kilowatt hours of gas consumed in the past 24 hours.",
             delete_on_unregister=True)

################################################################################

run_forever()
