# KVM Switch firmware

Bare-metal firmware for the STM32C031G6 on the KVM switch board. It uses only the
CMSIS register headers (vendored in `Drivers/`), with no HAL or LL. It builds to
about 7.4 KB of the 32 KB flash and has been tested on the rev A board.

## What it does

- **Button**: a short press switches HDMI and USB to the other host. A long press
  (0.8 s) steps the LED brightness through 4 levels, and the bar flashes at the
  new level.
- **Host detection**: watches HDMI +5V (PA5, PA11) and USB-C VBUS (PA4, PA12) from
  both hosts, debounced over 50 ms. Connect and disconnect events are printed on
  the UART.
- **Auto switch** (can be turned off): when the selected host disappears and the
  other host is present, the firmware switches to the other host.
- **Clean USB handover**: the firmware pulls the hub's `VBUS_DET` (PA8) low,
  moves the USB mux, then releases `VBUS_DET`. The old host sees an unplug and
  the new host sees a fresh plug. The hub only connects upstream while the
  selected host supplies VBUS.
- **LEDs** (D2 at the A end through D6 at the B end; host A orange, host B
  purple): the bar is dark at rest and only lights for events. Every effect fades
  out on its own. See the table below.
- **Settings in flash**: the settings are kept in the last 2 KB page, written as an
  append-only log of 16-byte records. The page is erased once per 128 saves.
  Saves happen 3 s after the last change. The last selected channel is restored
  at power-up.
- **Watchdog**: an IWDG with a ~1 s timeout. It is frozen while the core is halted
  in the debugger.

## LED behaviour

| Event | What the LEDs do |
|---|---|
| Power-up | A wave runs into the restored host's end in that host's color, then fades out (~1.5 s). |
| Switch (button, auto-switch, or UART) | A bright head starts at the old host's end in the old color. It travels to the new host's end, turning into the new color and filling the bar behind it. The whole bar then fades out (~1.5 s). |
| Switch with `anim off` | The bar lights in the new host's color and fades out (~0.9 s), with no travel. |
| Host connects (HDMI 5V or USB-C VBUS appears) | A soft pulse in that host's color at its end of the bar, spilling into the next two LEDs, fades out (~1.4 s). Each cable (HDMI, USB) restarts the pulse. |
| Host disconnects | Nothing. |
| Long button press (brightness step) | The whole bar flashes in the active host's color at the new brightness, then fades (~0.8 s). |
| `bright` over UART | Same flash. |
| `color a\|b` over UART | Pulse at that host's end in the new color. |
| Idle | All LEDs off. |

If the active host disconnects while the other is connected, auto-switch shows
the switch wave. If the other host's arrival triggers the switch, you also see
its connect pulse.

## UART (TX/RX test points, 115200 8N1)

```
status              show selection, detected hosts, settings
a | b | toggle      switch HDMI and USB together
hdmi a|b, usb a|b   switch one path only
bright 0-255        LED brightness
color a|b RRGGBB    channel color
auto on|off         follow the other host when the active one leaves
anim on|off         wave animation on switch
animms 100-5000     wave travel time
defaults            restore default settings
reboot
```

## Build

Build with `make` using any `arm-none-eabi-gcc`. The toolchain and `make`
bundled with STM32CubeIDE work fine. Add them to PATH in PowerShell:

```powershell
$p = "C:\ST\STM32CubeIDE_1.19.0\STM32CubeIDE\plugins"
$env:Path += ";$p\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344\tools\bin;$p\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845\tools\bin"
```

Then, from this folder:

```
make            # build/kvm_switch.elf / .hex / .bin
make flash      # program over SWD with an ST-Link on the CLK/DIO/GND/3V3 test points
make clean
```

`make flash` calls STM32CubeProgrammer from its default install path
(`C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe`).
If yours is somewhere else, pass `PROGRAMMER=<path>`.

To debug in CubeIDE, choose *File > New > Makefile Project with Existing Code*,
point it at this folder, and debug `build/kvm_switch.elf` with an ST-Link
configuration.

## Things to check during bring-up

- **HDMI select polarity**: `HDMI_SEL_B_LEVEL` in `src/board.h` assumes that
  `Dx_SEL` = high selects port B on the HD3SS215. Confirm this against the
  datasheet or on the bench. USB polarity comes from the netlist (S = high routes
  2D, which is J6 / host B).
- **LED data line**: the SK6812s only restart a frame after the data line has
  been low for more than 80 us. The SPI does not reliably idle low, so between
  frames PA2 is switched to a GPIO driven low (see `show()` in `src/leds.c`).
  Without this, D2 latches the first frame and passes every later frame on to D3,
  so D2 stays dark and the pattern is shifted by one LED. The chain also runs from
  5 V with 3.3 V data (spec V<sub>IH</sub> = 0.7 x VDD = 3.5 V). That works on the
  rev A board but has little margin.
- **Detect inputs**: these see 5 V through 10 kOhm. The firmware enables the
  internal pull-downs because the board has no external ones.

## Layout

```
src/board.h      pin map, polarity, GPIO helpers
src/main.c       clock, inputs/debounce, switching + USB handover, button, main loop
src/leds.c       SK6812 driver on SPI1 MOSI (3 MHz, 4 SPI bits per LED bit), event animations
src/settings.c   flash settings log
src/uart.c       USART1 driver
src/cli.c        command line
Drivers/CMSIS    ARM core + ST STM32C0 device headers, startup, system file
```
