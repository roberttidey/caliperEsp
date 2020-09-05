# Wifi Calipers
Wifi / Web server add on to digital calipers

Construction details to be posted at https://www.instructables.com/id/Wifi-Calipers
It makes use of the BaseSupport library at

https://github.com/roberttidey/BaseSupport

Edit the WifiManager and update passwords in BaseConfig.h

## Features
- Add on to back of digital calipers to make series of measurements available over wifi
- Self contained, no extra wires
- Battery powered (rechargeable LIPO) with inbuilt charger; also powers calipers
- Very low quiescent current (< 30uA) for long battery life
- Single button control to power on, take measurements, power off
- Auto turns off if quiescent for a period
- Measurements can be saved and loaded to files containing up to 16 measurements
- Individual measurements can be named
- Status and configuration data also available from web interface​
- Software can be updated via web interface
- Initial AP to set wifi access details when first configured or network changes.

## Usage
- Press button to turn on, caliper display comes on
- Status screen shows current measurement and other details
- Short press of button to take a measurement, displayed on measures screen and move onto next measurement item
- Medium length press of button to step back
- Long press of button to turn web server and calipers off
- Configuration screen displays current configuration and allows values to be changed
	- noChangeTimeout to set duration before auto switch off
	- button times to control duration of button presses for different actions
	- measureFilePrefix to control start of naming for all measure files
	
## Web interface
The firmware supports the following http calls
- /edit - access filing system of device; may be used to download measures Files
- /status - return a string containing status details
- /loadconfig -return a string containing config details
- /saveconfig - send and save a string to update config
- /loadmeasures - return a string containing measures from a files
- /savemeasures - send and save a string containing current measure details
- /setmeasureindex - change the index to be used for next measure
- /getmeasurefiles - get a string with list of available measure files




