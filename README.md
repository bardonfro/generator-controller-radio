# generator-controller-radio

## LoRa Radio-based controller for remote controlling a generator (or any other equipment)

# Overview

The system is comprised of two devices, both based on LilyGo LoRa32 boards, which are arduino-based LoRa radio boards. The server device is located within range of wifi and hosts a simple webserver with a single html page. That is the user interface for starting and stopping the generator and viewing some basic status and error information.

The second devices is inside the control panel of the generator. It communicates with the server over LoRa radio and activates the remote control circuit of the generator by connecting the remote signal to ground through a relay board. It also contains a high-voltage sensor to verify generator operation. It is powered by the generator 12v system via a buck converter. It also monitors generator battery voltage.

# Wiring

Wiring schematic for the controller device and its accessories is in the root file. The server device has no accessories except an antenna extender.

# Firmware

The firmware is flashed to the board using the Arduino IDE.ß

# Disclaimer
The majority of code generation was by Claude AI.