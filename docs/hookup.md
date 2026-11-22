# Hooking the PMBus target up

## Electrical notes

- The MCX A153 I/O runs from one supply, 1.71–3.6 V; the FRDM board runs it at 3.3 V.
  Inputs switch at 0.3/0.7 × VDD, which meets SMBus levels on a 3.3 V bus.
- P3_27/P3_28 (SCL/SDA) are 5 V tolerant, the only such pins on the chip.
- The firmware enables the internal pull-ups (33–75 kΩ). They are far too weak for a real
  bus: use external 2.2–4.7 kΩ pull-ups to 3.3 V unless the host side has them (the
  LX2160ARDB does).
- The datasheet guarantees VOL ≤ 0.5 V at 4 mA, not the SMBus 0.4 V; the overlay sets high
  drive strength, and with 4.7 kΩ pull-ups the sink current is under 1 mA anyway.
- Always connect GND between the boards.
- An unpowered FRDM board clamps the bus through its I/O protection diodes. Power it
  before the host starts using the bus, or add a buffer if it may be off while the bus is
  live.

## FRDM-MCXA153 pins

mikroBUS header **J5**:

| J5 pin | Signal | MCU pin |
|---|---|---|
| 5 | SCL | P3_27 (LPI2C0_SCL) |
| 6 | SDA | P3_28 (LPI2C0_SDA) |
| 8 | GND | |
| INT | SMBALERT# (open drain, active low) | P2_5 |

The pin positions of SCL, SDA and GND come from frdm-mcxa153-usb-i2c-hub, where they were
checked against NXP's quick start guide. **Check the INT position against the board
schematic before relying on it.**

## Bench: a second FRDM-MCXA153 as the host

Flash the second board with
[frdm-mcxa153-usb-i2c-hub](https://github.com/wrouwet/frdm-mcxa153-usb-i2c-hub). It uses the
same J5 pins, so wire J5 to J5: pin 5 to 5, pin 6 to 6, pin 8 to 8. With only the two boards'
internal pull-ups the bus is slow to rise; add 4.7 kΩ from SCL and SDA to 3.3 V.

```sh
python tools/pmbus_tool.py -t hub:/dev/ttyACM1 dump
make pytest PMBUS_TRANSPORT=hub:/dev/ttyACM1
```

Find the hub's port by its product string, as its README explains: it enumerates as
`MCU VIRTUAL COM DEMO`. The target board's own MCU-Link serial port carries the Zephyr
shell.

## LX2160ARDB

### Where the bus is

The LX2160ARDB reference manual (rev. 4, section 2.10) has no general-purpose I2C header.
I2C1 fans out through a PCA9547 at 0x77, and its channel 7 through a second PCA9547 at
0x75:

| Path | Goes to |
|---|---|
| 0x77 ch2 | INA220 (0x40) and the LTC3882 core regulator (0x63): a real PMBus device |
| 0x77 ch3 | SA56004 thermal monitors (0x4C, 0x4D) |
| 0x77 ch4, ch5 | Unused |
| 0x77 ch7 → 0x75 ch4, ch5 | SFP+ cages (MAC5, MAC6) |
| 0x77 ch7 → 0x75 ch6 | **PCIe slot 1 SMBus** |
| 0x77 ch7 → 0x75 ch7 | **PCIe slot 2 SMBus** |

The PCIe slots are the practical way in: every PCIe connector carries SMBus on side B,
pin B5 = SMCLK, B6 = SMDAT, B4 = GND (B7 is GND too). A PCIe x1 riser or breakout board with
exposed pads gives a clean, unmodified connection, and it is how a real plug-in card would
be managed. 0x58 is free on that segment: besides the target, only the CPLD (0x66) and the
two muxes (0x77, 0x75) are visible there.

**Care:** side B pins 1–3 carry 12 V. Work with the board powered off, and check the
breakout's pin labels against the PCIe pinout before connecting.

Unused 0x77 channels 4 and 5 would avoid the second mux, but have no connector; using them
means soldering to the board.

### Linux

The upstream device tree (`fsl-lx2160a-rdb.dts`) declares the 0x77 mux but not the 0x75 one
behind channel 7. Instantiate it by hand, then bind the generic PMBus driver:

```sh
i2cdetect -l                                    # find the adapter for 0x77 channel 7
echo pca9547 0x75 > /sys/bus/i2c/devices/i2c-<ch7>/new_device
i2cdetect -l                                    # 8 more adapters appear; slot 1 is channel 6
i2cdetect -y <slot1>                            # 0x58 should answer
echo pmbus 0x58 > /sys/bus/i2c/devices/i2c-<slot1>/new_device
sensors                                         # pmbus-i2c-<slot1>-58
```

The `pmbus` driver reads `CAPABILITY`, sees PEC support and turns PEC on (the i.MX I2C
driver's SMBus emulation supports it). It probes each `READ_*` command and keeps the ones
that don't set `STATUS_CML`, so it should create:

| hwmon | PMBus |
|---|---|
| `in1_input` (vin), `in2_input` (vout1) | `READ_VIN`, `READ_VOUT` |
| `curr1_input` (iin), `curr2_input` (iout1) | `READ_IIN`, `READ_IOUT` |
| `power1_input` (pin), `power2_input` (pout1) | `READ_PIN`, `READ_POUT` |
| `temp1_input`, `temp2_input` | `READ_TEMPERATURE_1/2` |
| `*_alarm`, `*_crit`, `*_max`, ... | the limits and `STATUS_*` bits |

The host unit tests replay this probe sequence (`test_linux_probe`).

For a permanent setup, describe the 0x75 mux and the device in a device-tree overlay
instead of `new_device`.

To use SMBALERT#, wire it to a host GPIO and bind `smbus_alert`; without it the hwmon alarm
files still work, since the driver polls the status registers.

### SONiC

SONiC's `psud` and `thermalctld` read the platform API (`sonic_platform/psu.py`,
`thermal.py`). On the LX2160ARDB platform these can read the hwmon files above: voltage,
current, power, temperature, presence (the device answering) and status (`POWER_GOOD#`
clear, no faults). That work belongs in
[lx2160-sonic](https://github.com/wrouwet/lx2160-sonic), not here.
