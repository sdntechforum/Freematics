This Arduino sketch is developed for [Freematics ONE+](https://freematics.com/products/freematics-one-plus/) to collect vehicle telemetry data from OBD, GPS, motion sensor, to log the data in local storage and to transmit the data to a remote server in real-time. It demonstrates most capabilities of Freematics ONE+ and works well with [Traccar](https://www.traccar.org) GPS tracking platform.

Data Collection
---------------

The sketch collects following data.

* Vehicle OBD data (from OBD port)
* Battery voltage (from OBD port)
* Geolocation data (from internal or external GNSS) 
* Accelerometer and gyroscope data (from internal MEMS motion sensor)
* Cellular or WiFi network signal level
* Device temperature

Collected data are stored in a circular buffer in ESP32's IRAM or PSRAM. When PSRAM is enabled, hours of data can be buffered in case of temporary network outage and transmitted when network connection resumes.
  
Data Transmission
-----------------

Data transmission over UDP and HTTP(s) protocols are implemented for the followings.

* WiFi (ESP32 built-in)
* 3G WCDMA (SIM5360)
* 4G LTE CAT-4 (SIM7600)
* 4G LTE CAT-M (SIM7070)

UDP mode implements a telemetry client for [Freematics Hub](https://hub.freematics.com) and [Traccar](https://www.traccar.org). HTTP(s) mode implements [OsmAnd](https://www.traccar.org/osmand/) protocol with additional data sent as POST payload.

Seamless WiFi and cellular network co-working is implemented. When defined WiFi hotspot is available, data is transmitted via WiFi and cellular module is switched off. When no WiFi hotspot can be reached, cellular module is switched on for data transmission until WiFi hotspot available again.

Data Storage
------------

Following types of data storage are supported.

* MicroSD card storage
* ESP32 built-in Flash memory storage (SPIFFS)

BLE & App
---------

A BLE SPP server is implemented in [FreematicsPlus](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus) library. To enable BLE support, change ENABLE_BLE to 1 [config.h](config.h). This will enable remote control and data monitoring via [Freematics Controller App](https://freematics.com/software/freematics-controller/).

Prerequisites
-------------

* Freematics ONE+ [Model A](https://freematics.com/products/freematics-one-plus/), [Model B](https://freematics.com/products/freematics-one-plus-model-b/), [Model H](https://freematics.com/products/freematics-one-plus-model-h/)
* A micro SIM card if cellular network connectivity required
* [PlatformIO](http://platformio.org/), [Arduino IDE](https://github.com/espressif/arduino-esp32#installation-instructions), [Freematics Builder](https://freematics.com/software/arduino-builder) or [ESP-IDF](https://github.com/espressif/esp-idf) for compiling and uploading code

## From ChatGPT decoding telelogger.ino program

This is the main code for a microcontroller-based project that uses various sensors and communication protocols to collect data and transmit it over the internet. The code is written in C++ and uses several libraries, including the Arduino library, to interact with the hardware.

Here's a high-level overview of the code structure and functionality:

* Initialization: The setup function initializes the hardware, including the CPU, memory, and I/O pins. It also initializes the serial communication interface and sets up the device ID.
* System Information: The showSysInfo function displays information about the system, such as the CPU speed, memory usage, and firmware version.
* Sensor Interface: The code includes interfaces for various sensors, such as temperature, humidity, pressure, and acceleration. These interfaces are implemented using different libraries, depending on the sensor type.
* Communication Protocols: The code supports multiple communication protocols, including HTTP, TCP, UDP, and BLE. These protocols are used to transmit data to a remote server or mobile device.
* Network Connection: The code establishes a network connection using the WiFi library and maintains it using a separate task.
* Data Collection and Logging: The process function collects data from the sensors and logs it to a file or sends it over the network using the chosen communication protocol.
* Error Handling: The code includes error handling mechanisms to handle unexpected events, such as a failed connection or sensor malfunction.
* Standby Mode: If an error occurs, the code enters a standby mode, where it disables all tasks and waits for a reset or manual restart.
* Main Loop: The main loop runs continuously and calls the process function repeatedly to collect and log data.
* Overall, this code appears to be designed for a IoT-based project that requires real-time data collection and transmission over a network.
