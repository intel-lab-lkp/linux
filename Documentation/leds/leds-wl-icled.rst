=================================================
Kernel driver for WL-ICLED from Wurth Elektronik
=================================================

Description
-----------
The WL-ICLEDs are RGB LEDs with integrated controller that can be
daisy-chained to a arbitrary number of units. The MCU communicates
with the first LED in chain via SPI interface and can be single or
two wire connection, depending on  the model.

Single wire models like 1315050930002, 1313210530000, 1312020030000 and
1312121320437 are controlled with specific signal pattern on the
input line. The MCU is connected to input line only via SPI MOSI signal.
For example WE-1312121320437 uses following signal pattern per one LED:

|          RED            |          GREEN          |           BLUE          |
| GAIN:4bits | PWM:12bits | GAIN:4bits | PWM:12bits | GAIN:4bits | PWM:12bits |

 where logical 1 is represented as:
 (V)^
    |          T
    |<-------1.2us------->
    |
    +================+
    | <---0.9us----> |
    |                |
    +----------------+===|------> t

 and logical 0 is represented as:
 (V)^
    |          T
    |<-------1.2us------->
    |
    +=====+
    |0.3us|
    |     |
    +-----+==============|------> t

To generate the required pattern with exact timings SPI clock is selected
so that it devides T in 8 equal parts such that a logical true symbol can be
represented as 1111 1100 and a logical false can be represented as 1100 0000.
Single wire LEDs require the MOSI line to be set to low at idle and this should
be provided by the chip driver if possible or by external HW circuit.

Models 1313210530000, 1312020030000 and 1315050930002 require a slightly
different signaling scheme where each color of the LED is encoded in
8 bits.

Two wire LED types do not require specific encoding of the input line as
both clock and data are provided to each LED.

Additionally, models differ by available controls:
- WE 1312121320437 provide PWM and GAIN control per each RGB element.
  Both GAIN and PWM values are calculated by normalising particular
  multi_intensity value to 4 and 12 bits.

- WE 1315050930246 and 1311610030140 provide PWM control per each
  RGB element and one global GAIN control.
  Global GAIN value is calculated by normalising global led brightness
  value to 5 bits while PWM values are set by particular
  multi_intensity values.

- WE 1315050930002, 1313210530000 and 1312020030000 provide only PWM
  control per each RGB element.
  PWM values are set by particular multi_intensity value.

For more product information please see the link below:
https://www.we-online.com/en/components/products/WL-ICLED
