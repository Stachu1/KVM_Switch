# KVM Switch firmware

Bare-metal firmware for the STM32C031G6 on the KVM switch board. It uses only the
CMSIS register headers (vendored in `Drivers/`), with no HAL or LL. It builds to
about 7 KB of the 32 KB flash.

## What it does

- **Button**: a short press switches HDMI and USB to the other host. A long press
  (0.8 s) steps the LED brightness through 4 levels.
- **Host detection**: watches HDMI +5V (PA5, PA11) and USB-C VBUS (PA4, PA12) from
  both hosts, debounced over 50 ms. Connect and disconnect events are printed on
  the UART.
- **Auto switch** (can be turned off): when the selected host disappears and the
  other host is present, the firmware switches to the other host.
- **Clean USB handover**: the firmware pulls the hub's `VBUS_DET` (PA8) low,
  moves the USB mux, then releases `VBUS_DET`. The old host sees an unplug and
  the new host sees a fresh plug. The hub only connects upstream while the
  selected host supplies VBUS.
- **LEDs** (D2 at the A end through D6 at the B end):
  - The end LED for the active host is at full brightness. The middle LEDs show
    the same channel color, dimmer.
  - The other host's end LED glows dimly in that host's color when the host is
    connected, and is off when it isn't.
  - If the active host has nothing connected, the LEDs breathe slowly.
  - If USB is routed separately from HDMI, the centre LED shows the USB channel.
  - On a switch, a bright head runs from the old host's end to the new host's end
    and the colors crossfade behind it.
- **Settings in flash**: the settings are kept in the last 2 KB page, written as an
  append-only log of 16-byte records. The page is erased once per 128 saves.
  Saves happen 3 s after the last change. The last selected channel is restored
  at power-up.
- **Watchdog**: an IWDG with a ~1 s timeout. It is frozen while the core is halted
  in the debugger.

## UART (TX/RX test points, 115200 8N1)

```
status              show selection, detected hosts, settings
a | b | toggle      switch HDMI and USB together
hdmi a|b, usb a|b   switch one path only
bright 0-255        LED brightness
color a|b RRGGBB    channel color
auto on|off         follow the other host when the active one leaves
anim on|off         wave animation on switch
animms 100-5000     wave duration
defaults            restore default settings
reboot
```

## Build

Build with `make` using any `arm-none-eabi-gcc`. The toolchain and `make`
bundled with STM32CubeIDE work fine:

```sh
P=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins
export PATH="$P/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin:$P/com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845/tools/bin:$PATH"
make            # build/kvm_switch.elf / .hex / .bin
make flash      # needs STM32_Programmer_CLI on PATH and an ST-Link on the SWD test points
```

To debug in CubeIDE, choose *File > New > Makefile Project with Existing Code*,
point it at this folder, and debug `build/kvm_switch.elf` with an ST-Link
configuration.

## Things to check during bring-up

- **HDMI select polarity**: `HDMI_SEL_B_LEVEL` in `src/board.h` assumes that
  `Dx_SEL` = high selects port B on the HD3SS215. Confirm this against the
  datasheet or on the bench. USB polarity comes from the netlist (S = high routes
  2D, which is J6 / host B).
- **LED data level**: the SK6812 chain runs from 5 V, but the data line is a
  3.3 V signal. Its spec is V<sub>IH</sub> = 0.7 x VDD = 3.5 V. It usually works
  anyway. If the first LED flickers, that is the cause.
- **Detect inputs**: these see 5 V through 10 kOhm. The firmware enables the
  internal pull-downs because the board has no external ones.

## Layout

```
src/board.h      pin map, polarity, GPIO helpers
src/main.c       clock, inputs/debounce, switching + USB handover, button, main loop
src/leds.c       SK6812 driver on SPI1 MOSI (3 MHz, 4 SPI bits per LED bit), status and wave rendering
src/settings.c   flash settings log
src/uart.c       USART1 driver
src/cli.c        command line
Drivers/CMSIS    ARM core + ST STM32C0 device headers, startup, system file
```
