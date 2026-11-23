# FRDM-MCXA153 PMBus target

Turns an NXP [FRDM-MCXA153](https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-MCXA153)
into a **PMBus 1.3 device** (an SMBus target) that a host such as a SONiC switch's
control-plane CPU can monitor and control over two wires. The device presents itself as a
simple 54 V to 12 V converter: its electrical side is simulated, its two temperatures are
real (the board's P3T1755 sensor and the MCU die).

The goal is a realistic PMBus endpoint for bringing up the host side: the Linux `pmbus`
hwmon driver, SONiC's PSU and thermal plumbing, and alert handling, on the
[LX2160ARDB SONiC port](https://github.com/wrouwet/lx2160-sonic).

> **Status (2026-11-22):** firmware builds; the PMBus library passes its host unit tests
> and the device test suite on the simulator. **Nothing has run on the board yet.** See
> [Not yet verified on hardware](#not-yet-verified-on-hardware).

## What it implements

| Area | Details |
|---|---|
| SMBus protocols | Send Byte, Read/Write Byte, Read/Write Word, Block Read/Write (32 bytes), Block Write-Block Read Process Call (`QUERY`) |
| PEC | Checked on every write that carries it, appended to every read; a bad PEC rejects the write and sets `STATUS_CML` |
| Alerts | `SMBALERT#` (open drain) on new status bits; answers the Alert Response Address 0x0C while alerting |
| SMBus timeout | A transaction stalled for 30 ms resets the target interface (SMBus: 25–35 ms) |
| Group Command | Writes take effect at STOP, so group commands execute together |
| Status | `STATUS_BYTE/WORD/VOUT/IOUT/INPUT/TEMPERATURE/CML/MFR_SPECIFIC`, latched, write-1-to-clear, `CLEAR_FAULTS` |
| Control | `OPERATION` (on/off, margins), `ON_OFF_CONFIG` (incl. a CONTROL input), `VOUT_COMMAND`, margins, `WRITE_PROTECT` |
| Limits | VOUT OV/UV, IOUT OC, OT/UT, VIN OV/UV, each warning and fault; faults latch the output off |
| Telemetry | `READ_VIN/IIN/PIN/VOUT/IOUT/POUT` (simulated), `READ_TEMPERATURE_1` (P3T1755), `READ_TEMPERATURE_2` (die) |
| Identification | `PMBUS_REVISION` 1.3, `CAPABILITY`, `MFR_ID/MODEL/REVISION/LOCATION/DATE` (dummy, writable), `MFR_SERIAL` (from the chip UID) |
| Storage | `STORE_DEFAULT_ALL` / `RESTORE_DEFAULT_ALL` to the 16 KB flash storage partition |
| Simulation knobs | `MFR_SPECIFIC_00` load current, `MFR_SPECIFIC_01` input voltage (LINEAR11), plus shell commands |

The full command list is in [docs/commands.md](docs/commands.md).

## Hardware

- FRDM-MCXA153, flashed over its on-board MCU-Link.
- PMBus on the **mikroBUS header J5**: pin 5 = SCL (P3_27), pin 6 = SDA (P3_28),
  pin 8 = GND, plus SMBALERT# on the mikroBUS INT pin (P2_5). These are the same pins
  [frdm-mcxa153-usb-i2c-hub](https://github.com/wrouwet/frdm-mcxa153-usb-i2c-hub) uses, so
  two FRDM boards wire header to header. P3_27/P3_28 are the MCX A153's only 5 V tolerant
  pins.
- Address **0x58** (`CONFIG_PMBUS_ADDRESS`), 100 or 400 kHz.

Wiring to the hub and to the LX2160ARDB, and binding the Linux driver, are in
[docs/hookup.md](docs/hookup.md).

## Quick start

```sh
sudo scripts/install-host-deps.sh   # once: Ubuntu packages, udev rule for the probe
make setup                          # once: Python venv, west, Zephyr v4.5.0, toolchain
make test                           # host unit tests + the device suite on the simulator
make flash                          # build and flash (RUNNER=linkserver by default)
make monitor                        # the Zephyr shell on the MCU-Link serial port
```

The workspace is the parent directory (`~/mcxa153-workspace`): `zephyr/`, `modules/`,
`.venv/` and `.tools/` live next to this repo.

On the shell:

```
uart:~$ pmbus status
address 0x58, output on
VIN  54.000 V   IIN  1.178 A   PIN  63.688 W
VOUT 12.000 V   IOUT 5.000 A   POUT 60.000 W
...
uart:~$ pmbus load 9000        # 9 A: IOUT warning, SMBALERT# goes low
uart:~$ pmbus temp 1 90000     # pretend sensor 1 reads 90 °C
uart:~$ pmbus temp 1 off
```

## Talking to it from a host

`tools/pmbus_tool.py` and the test suite share a small PMBus client with three transports:

| Transport | Use |
|---|---|
| `sim` | The firmware's PMBus library compiled for the PC (`make sim-lib`), no hardware |
| `hub:/dev/ttyACMx` | A second FRDM-MCXA153 running frdm-mcxa153-usb-i2c-hub |
| `linux:<bus>` | `/dev/i2c-<bus>` on any Linux host, e.g. the LX2160ARDB |

```sh
python tools/pmbus_tool.py -t hub:/dev/ttyACM1 dump
python tools/pmbus_tool.py -t linux:12 read 0x8d     # READ_TEMPERATURE_1
python tools/pmbus_tool.py -t linux:12 alert         # who pulled SMBALERT#?
make pytest PMBUS_TRANSPORT=hub:/dev/ttyACM1         # the device suite, on real hardware
```

The tools need Python 3 only; the hub transport also needs `pyserial`.

## Layout

```
lib/pmbus/        Portable PMBus library, no OS dependencies
  src/target.c      SMBus transaction engine: framing, PEC, ARA, execute-at-STOP
  src/device.c      The device model: command table, limits, status, simulation
  src/format.c      LINEAR11, ULINEAR16, CRC-8
app/              Zephyr application for the FRDM-MCXA153
  src/pmbus_i2c.c   LPI2C target glue, SMBus timeout, Alert Response Address
  src/storage.c     STORE/RESTORE_DEFAULT_ALL on the flash storage partition
  src/main.c        Sensors, LEDs, the update loop
  src/shell_cmds.c  `pmbus` shell commands
tests/host/       C unit tests of the library (ASan/UBSan), incl. the Linux probe sequence
tests/sim/        The library as a shared object for the Python simulator transport
tests/hw/         pytest device suite: runs on sim, hub or linux transports
tools/            pmbus_tool.py and the Python PMBus client
docs/             Hookup and command reference
```

The library is kept apart from Zephyr so it can be tested on the PC and, later, reused on
other MCUs or proposed upstream as a Zephyr PMBus target sample.

## Not yet verified on hardware

Everything below is written and builds, but needs the board:

- LPI2C0 target on P3_27/P3_28, and the J5 pin numbers for SCL/SDA (taken from the hub
  project, where they were tested) and for INT/SMBALERT# (P2_5, not yet checked against the
  board schematic).
- Zephyr's LPI2C target driver with PMBus traffic: repeated starts, the extra byte it
  prefetches at the end of a read, clock stretching per byte.
- Answering the Alert Response Address by toggling `SCFGR1[SAEN]` while the bus is idle.
- The SMBus timeout recovery (unregister and re-register the target).
- The P3T1755 over I3C and the die temperature through the ADC.
- `STORE_DEFAULT_ALL`: the flash erase blocks interrupts, so the target stretches SCL while
  it runs; the length needs measuring against the host's timeout.

## License

Apache-2.0

## SFP module emulation (`sfp/`)

A second app turns the board into an SFP optical module (SFF-8472) for I2C host
testing, on the same pins: A0h (serial ID) at 0x50 and A2h (diagnostics) at 0x51.

- A0h: SFP, LC, 10GBASE-SR, 850 nm, vendor `EXAMPLE`, PN `MCXA153-SFP-SR`, valid
  CC_BASE/CC_EXT checksums.
- A2h: thresholds (0-39), live diagnostics (96-105: temperature from the on-board
  P3T1755, Vcc, TX bias, TX and RX power), status/control (110, host-writable soft
  TX disable), alarm and warning flags (112-117) computed from the values, user
  EEPROM (128-247, writable).
- Shell: `sfp status`, `sfp set <temp|vcc|bias|tx|rx> <value|auto>`,
  `sfp los on|off`, `sfp txfault on|off`, `sfp dump a0|a2`.

Zephyr's LPI2C driver registers one target address and reads SASR before the
callbacks run, which clears the matched address. The app adds 0x51 as LPI2C
address match 1 and wraps the LPI2C0 interrupt (`CONFIG_DYNAMIC_INTERRUPTS`) to
keep SSR's address match flags from the address interrupt.

Build and flash:

    west build -b frdm_mcxa153 -d build/sfp sfp
    west flash -d build/sfp -r linkserver

Reading the status block from a Linux host (bus N): write offset 96 to 0x51, then
read 64 bytes (e.g. `smbus2` `i2c_rdwr`).

### Transaction timing (`sfp timing`)

The interrupt wrapper also timestamps the LPI2C0 interrupts with the DWT cycle
counter (96 MHz): first address match, repeated start, every read-phase TX
interrupt, STOP, and how long the target holds SCL per byte. Measured from an
LX2160ARDB (Linux `i2c-imx`, IIC1 87.5 MHz / 896 = 97.66 kHz) for a 64-byte read of
A2h from offset 96 (offset write, repeated start, read):

| Measured at | Result |
|---|---|
| Host, application (`i2c_rdwr`, 500x) | min 7.1 ms, median 8.0 ms, max 9.9 ms |
| Host, kernel I2C trace (start to result) | 7.7-8.7 ms |
| Target, first address byte to STOP | min 6.9 ms, avg 7.7 ms, max 9.5 ms |
| Target, per read byte | avg 117.7 us (9 SCL = 92.2 us, so about 26 us per byte between bytes) |
| Target, SCL held per byte | at most 3.1 us |
| Pure bus time at 97.66 kHz | 6.2 ms |

The time between bytes is the host's: `i2c-imx` moves one byte per interrupt, and
the controller holds SCL until the driver has read it.
