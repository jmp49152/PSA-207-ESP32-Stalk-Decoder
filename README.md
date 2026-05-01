# PSA207 2007 Stalk commands to standard out dumper — Written to target ESP-IDF 6 TWAI API

This is just a little tool born out of being frustated at failing in my quest to find a working canbus box for a 2007 Peugeot 207 once the RD4 had been replaced by a chinese android head unit that I have made as a stepping stone towards building my own working canbus adapter box.
I've uploaded it here for documentation and in case it has snippets that others find useful. And because information wants to be free.

## Hardware used
ESP32 DevKit V1 Elegoo ESP-WROOM-32
SN65HVD230 3.3 V CAN transceiver module
Buck boost module capable of converting 12v->5v for feeding the ESP32.
Many SN65HVD230 breakout boards include a 120 ohm smd resistor across the can lines. Remove it if it causes issues.
It uses the TWAI node API to log packets from  CAN ID 0x21F & requires > Espressive IDF 6.0.x of the ide to compile. 

POWER
-----
Loom 12V  -----------------> ESP32 power input via buck-boost
Loom GND  -----------------> ESP32 GND via buck-boost
ESP32 3.3V ----------------> SN65HVD230 VCC
ESP32 GND -----------------> SN65HVD230 GND


CAR CAN SIDE
------------
Quadlock CANH ------------------> SN65HVD230 CANH
Quadlock CANL ------------------> SN65HVD230 CANL
ESP32 GPIO21 -------------> SN65HVD230 TXD
ESP32 GPIO22 <------------- SN65HVD230 RXD

I took gnd, +12v, CANH & CANL from pins 16, 12, 10 & 13 of the Quadlock Part A.


Car CAN bus
   |
   v
SN65HVD230
   |
   v
ESP32 TWAI decoder
   |
   | decodes 0x21F stalk events:
   |   volume_up
   |   volume_down
   |   next_track
   |   prev_track
   |   source
   |   scroll_up / scroll_down


## Ascii art of the interface hardware because I'm a console monkey
     Peugeot 207 Comfort CAN bus
              (125 kbit/s)
             CANH        CANL
               |           |
               |           |
               |           |
        +-------------------------+
        |   SN65HVD230 CAN        |
        |   transceiver module    |
        |                         |
        |   CANH <-------------- CANH
        |   CANL <-------------- CANL
        |   TXD  <------------- ESP32 GPIO21
        |   RXD  -------------> ESP32 GPIO22
        |   VCC  <------------- 3.3V
        |   GND  <------------- GND
        +-------------------------+

                         +-----------------------------------+
                         |   ESP32 DevKit V1 Elegoo          |
                         |   ESP-WROOM-32                    |
                         |                                   |
5V from buck/boost------>| VIN / power input                |
GND from buck/boost----->| GND                              |
                         |                                   |
                         | CAN/TWAI interface:              |
                         |   GPIO21 -> CAN TXD              |
                         |   GPIO22 <- CAN RXD              |
                         |                                   |
                         | Head-unit UART interface:         |
                         |   GPIO17 -> HU serial RX         |
                         |   GPIO16 <- HU serial TX         |
                         |                                   |
                         | USB UART -> laptop/debug console  |
                         +-----------------------------------+

## What it does
- Initializes SN65HVD230 standby pin (default LOW = active)
- Starts a TWAI node in listen-only mode by default
- Logs received frames to CAN ID 0x21F to UART

## Build the firmware for the ESP32 using espressif IDE (linux version)
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py set-target esp32
idf.py fullclean
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

## Wiring

12v from quadlock -> Buck Boost converter set at 5v out -> Esp32 5vdc in.
ESP32 GPIO21 -> SN65HVD230 CTX/TXD
ESP32 GPIO22 <- SN65HVD230 CRX/RXD
GND common
CANH/CANL on SN65HVD230 -> PSA comfort CAN in the quadlock socket.

Listen-only by default for safety.

```c
#define ENABLE_CAN_TRANSMIT 0
```
## Using
Make a adaptor on perf/vero board, Flash the firmware, hook up the pins to the can socket, and monitor the esp32. You should see lines like these on the console monitor when you press the stalk buttons.


I (70020) PSA207_STALK: STALK baseline t=69771ms id=0x21F dlc=3 raw=00 00 00
I (90840) PSA207_STALK: STALK volume_up t=90592ms id=0x21F dlc=3 raw=08 00 00
I (100920) PSA207_STALK: STALK volume_down t=100673ms id=0x21F dlc=3 raw=04 00 00
I (411930) PSA207_STALK: STALK next_track t=411687ms id=0x21F dlc=3 raw=80 00 00
I (140040) PSA207_STALK: STALK prev_track t=139794ms id=0x21F dlc=3 raw=40 00 00
I (161050) PSA207_STALK: STALK source t=160809ms id=0x21F dlc=3 raw=02 00 00
I (186630) PSA207_STALK: STALK scroll_up t=186383ms delta=1 pos=1 raw=00 01 00
I (207540) PSA207_STALK: STALK scroll_down t=207298ms delta=-1 pos=4 raw=00 04 00

The decoder logs button events on rising edges only. This avoids repeated events while a button is held.
The scroll wheel is position-based. Direction is derived by comparing the current scroll byte with the previous one using signed 8-bit wraparound arithmetic.

## What use is it? 
Proving the stalk communications on a earlier generation 207 can be read with a esp32 & horrible hacky code.

## Whats next
Fix Readme.md so its not a formatting mess.
Stalk comms sent to chinese head unit via spi interface with the esp32 acting as a bridge to replace the lost RD4 stalk control functionality.


## Warning
This project connects to a vehicle CAN bus. Incorrect wiring or accidental transmission can affect vehicle behaviour.
If it causes issues for your car, or a flock of llama's with wings to suddenly attack you, I'm not responsible.
Use at your own risk. 
The following will probably help with at least the flying llama's. Its probably best to use this parked with the engine off.

- keep CAN transmit disabled during decoding work (its just a stub for some of my other tinkering)
- use a common ground
- avoid extra CAN termination
- fuse any vehicle power feed
- verify CANH/CANL before connecting
- test with ignition/accessory state controlled

