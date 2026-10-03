# ESP32R4 Sprinkler Controller

A four-zone sprinkler controller for the RobotDyn ESP32R4 4-relay board.  I wanted a simple irrigation controller that I had control over and a web interface.  Anything commercial out there is extremely overkill or requires cloud garbage.   the Only cloud I need is the ones with rain.

The project is designed around a single watering start time. On selected week days, enabled zones run one at a time in order, each for its configured number of minutes. A 10 second pause is inserted between zones to allow water pressure to stabilize before the next valve opens. This is hardcoded, if you need more time or a pump relay etc... well the source code is right here.    Another aspect of this project I really hate the stupid trend of trying to make arduino projects overly complicated and forcing people into the hell that is platformIO.   You dont need to objectify code to hell and back. it can be easy to follow and maintain.   This can easily be compiled on standard arduino IDE.  Also I dont go finding random wierd libraries like a lot of people do.  The only thing you need installed is ESP32 board package by Espressif Systems.    No I dont use JigglyPuff.lib version 47.6 Pre alpha test dev that you have to go hunting for.   Bad developers do that, dont be a bad developer.   If your code is not easy to maintain by even a newbie then your code sucks.  

IT does not have a rain sensor,  it does not have all kinds of fancy features.  You are more than welcome to add those, and if you want to be nice submit a patch, after review I'll add it.   

![main](\png\main.png)

## Hardware

The current pin mapping targets the RobotDyn ESP32R4 4-relay board and is compiled as an ESP32 Dev Module in Arduino IDE.

| Function | GPIO |
| --- | ---: |
| Zone 1 relay | 25 |
| Zone 2 relay | 26 |
| Zone 3 relay | 33 |
| Zone 4 relay | 32 |
| Zone 1 button | 34 |
| Zone 2 button | 35 |
| Zone 3 button | 36 |
| Zone 4 button | 39 |
| Status LED | 2 |

If another ESP32 relay board is used, update the relay and button pin arrays near the top of the sketch before compiling.    Some things to note.   I am using the base controller.  so there is NO RTC meaning it needs internet to be accurate in any way. it also will lose the time and date completely on power loss.  if you want more accuracy you may want to add an RTC board to the spi interface inside.   I was contemplating adding a temperature sensor to do a freezing inhibit, but I rarely get that here so I'm skippping it for now.

Something to think of,  Sprinkler irrigation valves spike hard  during field collapse, not a bad idea to put an external RC snubber to protect the relays.  Also remember your sprinkler wires are long antennas  they will gleefully suck up lighting strike energy.  a snubber can help with this a little bit, an MOV will help more.   or do what I did,  buy 3 of these boards and replace the board when lightning takes it out.![robotdyn_ESP32R4](\png\robotdyn_ESP32R4.webp)

## Main Features

The controller stores its schedule, zone settings, timezone, inhibit state, WiFi configuration, and access point password in ESP32 NVS flash, so normal settings survive a power loss and firmware update.

There is one watering start time and one set of watering days for the whole controller. Each of the four zones has its own enabled state and run time. During a scheduled cycle the zones run sequentially with a 10 second pressure stabilization delay between enabled zones.

The browser interface shows live controller time, the next scheduled watering date, current zone status, active run countdowns in minutes and seconds, the pressure stabilization countdown, and the 24 hour watering inhibit status. Status updates happen automatically without refreshing the page.

Each zone can also be started manually from the web page or its physical button. Only one manual zone can run at a time. A running manual zone can be toggled off again. Manual controls are blocked while a scheduled watering cycle is running. The red Stop Sprinkler button immediately stops active watering and cancels the current scheduled cycle.

The controller uses SNTP for time synchronization and supports selectable US timezones plus UTC. Daylight saving changes are handled automatically for the zones that observe it.

## First Boot and WiFi Setup

The public source does not contain a home WiFi SSID or password. On a fresh controller with no saved network configuration, the ESP32 starts its fallback access point.

Connect to the WiFi network named `SprinklerController` using the default password `sprinkler123`, then open `http://192.168.4.1` in a browser. Open the Network / Firmware page and enter the home WiFi SSID and password. The controller saves those values in NVS and restarts.  Where did it go on your network?  I have no idea. go ask your wifi router as to what IP Address it was given.     Yes this sucks and I suck as a programmer.   Feel free to go find someone elses project if it offends you.

On later boots it tries the saved home WiFi network for 20 seconds. If the network is unavailable it starts the fallback access point again. While in fallback mode it continues retrying the saved home network in the background. Once the normal network returns, the fallback access point shuts down automatically.

The fallback AP password can be changed from the Network / Firmware page. The AP password must be at least eight characters.

## Time and Offline Operation

Once the ESP32 has synchronized its clock, temporary WiFi loss does not stop the controller from running the watering schedule. The clock continues running locally. Du understand that the ESP32 software clock with no temp stabilization will drift horribly.  30 seconds per day is not out of the ordinary.

The board does not contain a battery-backed real-time clock. After a complete power loss, if the ESP32 boots with no network connection available, it cannot know the actual date and time. Manual control and the fallback access point still work, but scheduled watering waits until valid time is obtained from an NTP server.

A battery-backed RTC can be added later if completely network-independent scheduled operation after a power failure is required. a DS3231 board would be the easiest.  Oh yeah,  timezones in web interface are American only.  Sorry if this offends you, Oh look sourcecode, you could fix that!

## Arduino IDE Setup

Open `ESP32R4SprinklerController.ino` in Arduino IDE and select an ESP32 board configuration appropriate for the RobotDyn ESP32R4. The current project just uses `ESP32 Dev Module`.

The sketch uses libraries supplied by the ESP32 Arduino core: `WiFi`, `WebServer`, `Preferences`, `Update`, and the standard time support. No third-party library is required for the current firmware.

Choose an ESP32 partition scheme that supports OTA updates. OTA requires space for two application images. A single-application partition layout cannot perform web OTA updates. I am using Default 4MB with spiffs (1.2MB APP / 1.5MB SPIFFS)

Compile and upload normally over USB for the first installation.

## Web Firmware Updates

After the first USB installation, later firmware can be installed from the controller web interface without touching the board or pressing Reset.

In Arduino IDE 2.x, use `Sketch > Export Compiled Binary`. Arduino writes the compiled binary into the same folder as the `.ino` sketch. For the web updater, use the normal application firmware file, typically named something similar to:

```text
ESP32R4SprinklerController.ino.bin
```

Do not upload the bootloader or partition-table `.bin` files through the web updater.

Open the sprinkler controller in a browser, go to Network / Firmware, choose Update Firmware, select the application `.bin`, and start the upload. The controller immediately turns off all sprinkler relays before beginning the update. The new firmware is written to the inactive OTA application partition.

After a successful upload, the ESP32 marks the new image for boot and calls `ESP.restart()` automatically. There is no need to walk out to the controller and press its Reset button. Once it reboots, reconnect to the web page and verify the displayed firmware version.

Sprinkler settings and network configuration are stored in NVS and are not normally erased by an OTA application update.

## Watering Schedule

Select the days of the week and one start time. Each enabled zone then runs in numeric order for its configured number of minutes. A 10 second delay occurs between enabled zones.

For example, with a 6:00 AM start and run times of 12, 8, 15, and 5 minutes, the sequence is Zone 1 for 12 minutes, a 10 second pause, Zone 2 for 8 minutes, another 10 second pause, Zone 3 for 15 minutes, another pause, and finally Zone 4 for 5 minutes.

Disabled zones are skipped. The controller also records the last scheduled run date in NVS so a short power interruption during the scheduled start minute does not cause the entire cycle to start a second time that day.

## Manual Control

The four physical buttons and the four web buttons use the same basic manual watering rules. A manual zone runs for that zone's configured run time unless it is stopped sooner. Only one valve can be active manually at a time.

Manual starts are rejected during a scheduled watering cycle, during the 24 hour watering inhibit, or while a firmware update is in progress. The active manual zone can always be toggled off.

The red Stop Sprinkler control is an immediate stop. It turns off all zones and cancels the current scheduled sequence. It does not enable the 24 hour inhibit, so future scheduled watering remains enabled.

## 24 Hour Inhibit

The web interface can inhibit watering for 24 hours. The inhibit expiration time is stored in NVS so it survives a restart. The inhibit can also be released manually before the 24 hours has elapsed.  Why have this?   I see it's raining,  I push button and it doesnt sprinkle.  it's a poor persons rain sensor.  most sprinklers have this.

## License

This project is licensed under the GNU General Public License version 2 only, identified by the SPDX expression `GPL-2.0-only`. See the `LICENSE` file for the complete license text. 

Copyright (C) 2026 Tim Gray.

## Disclaimer

This software controls physical relays connected to irrigation equipment. Verify relay polarity, valve wiring, power requirements, enclosure protection, and local electrical requirements before leaving the controller unattended. Test each zone and the Stop Sprinkler function before relying on scheduled operation. Oh and did I use AI? you bet your ass I did.  seasoned developers use tools available to them.   I am not going to wrote all this text when I can have a machine do it.   and Yes I had it assist with some code instead of spending time figuring out how to enable and setup the OTA loader and Wifi Self AP fallback.  I know what every line of this code does, Unlike most vibe coders that dont even know what an int does or how big it is. (esp32 = 32bits)  

And yes if I seem spicy here  I am.  I am very much done with programmers that over complicate everything for the sake of complications or in attempts to try and impress someone.    Good Code is easy to read and maintain.  Good projects are easy to deploy.

