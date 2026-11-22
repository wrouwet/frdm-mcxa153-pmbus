# Supported PMBus commands

Single page (`PAGE` accepts 0 and 0xFF). `VOUT_MODE` is 0x17: linear, exponent −9, so
VOUT values are ULINEAR16 in 1/512 V steps. Every other value is LINEAR11. Any command not
listed reads as all ones (with no valid PEC) and sets `STATUS_CML` bit 7, invalid command.

| Code | Command | Type | Access | Default / source |
|---|---|---|---|---|
| 0x00 | PAGE | byte | R/W | 0 |
| 0x01 | OPERATION | byte | R/W | 0x80 (on). Bits 5:4 select margin low/high |
| 0x02 | ON_OFF_CONFIG | byte | R/W | 0x18: obey OPERATION, ignore CONTROL |
| 0x03 | CLEAR_FAULTS | send byte | W | |
| 0x10 | WRITE_PROTECT | byte | R/W | 0x00. Accepts 0x00, 0x20, 0x40, 0x80 |
| 0x11 | STORE_DEFAULT_ALL | send byte | W | Writes flash; `STATUS_BYTE` BUSY while it runs |
| 0x12 | RESTORE_DEFAULT_ALL | send byte | W | Stored config, or the built-in one |
| 0x19 | CAPABILITY | byte | R | 0xB0: PEC, 400 kHz, SMBALERT# |
| 0x1A | QUERY | block process call | R | |
| 0x20 | VOUT_MODE | byte | R | 0x17 |
| 0x21 | VOUT_COMMAND | word | R/W | 12.0 V |
| 0x25 | VOUT_MARGIN_HIGH | word | R/W | 12.6 V |
| 0x26 | VOUT_MARGIN_LOW | word | R/W | 11.4 V |
| 0x40 | VOUT_OV_FAULT_LIMIT | word | R/W | 13.8 V |
| 0x42 | VOUT_OV_WARN_LIMIT | word | R/W | 13.2 V |
| 0x43 | VOUT_UV_WARN_LIMIT | word | R/W | 10.8 V |
| 0x44 | VOUT_UV_FAULT_LIMIT | word | R/W | 10.2 V |
| 0x46 | IOUT_OC_FAULT_LIMIT | word | R/W | 10 A |
| 0x4A | IOUT_OC_WARN_LIMIT | word | R/W | 8 A |
| 0x4F | OT_FAULT_LIMIT | word | R/W | 100 °C |
| 0x51 | OT_WARN_LIMIT | word | R/W | 85 °C |
| 0x52 | UT_WARN_LIMIT | word | R/W | −20 °C |
| 0x53 | UT_FAULT_LIMIT | word | R/W | −40 °C |
| 0x55 | VIN_OV_FAULT_LIMIT | word | R/W | 60 V |
| 0x57 | VIN_OV_WARN_LIMIT | word | R/W | 58 V |
| 0x58 | VIN_UV_WARN_LIMIT | word | R/W | 44 V |
| 0x59 | VIN_UV_FAULT_LIMIT | word | R/W | 40 V |
| 0x78 | STATUS_BYTE | byte | R | |
| 0x79 | STATUS_WORD | word | R | |
| 0x7A–0x7E | STATUS_VOUT, _IOUT, _INPUT, _TEMPERATURE, _CML | byte | R/W | Write 1 to clear a bit |
| 0x80 | STATUS_MFR_SPECIFIC | byte | R/W | Always 0 for now |
| 0x88 | READ_VIN | word | R | Simulated, 54 V |
| 0x89 | READ_IIN | word | R | PIN / VIN |
| 0x8B | READ_VOUT | word | R | The selected set point while on, else 0 |
| 0x8C | READ_IOUT | word | R | The simulated load while on, else 0 |
| 0x8D | READ_TEMPERATURE_1 | word | R | P3T1755 on the board |
| 0x8E | READ_TEMPERATURE_2 | word | R | MCU die |
| 0x96 | READ_POUT | word | R | VOUT × IOUT |
| 0x97 | READ_PIN | word | R | POUT / 95 % + 0.5 W standby |
| 0x98 | PMBUS_REVISION | byte | R | 0x33 (1.3) |
| 0x99 | MFR_ID | block | R/W | "EXAMPLE" |
| 0x9A | MFR_MODEL | block | R/W | "MCXA153-PMB-1" |
| 0x9B | MFR_REVISION | block | R/W | "A0" |
| 0x9C | MFR_LOCATION | block | R/W | "LAB" |
| 0x9D | MFR_DATE | block | R/W | "261122" |
| 0x9E | MFR_SERIAL | block | R/W | The first 8 bytes of the chip UID, in hex |
| 0xD0 | MFR_SPECIFIC_00 | word | R/W | Simulated load current, 5 A |
| 0xD1 | MFR_SPECIFIC_01 | word | R/W | Simulated input voltage, 54 V |

## Behaviour

- **Faults** (VOUT OV/UV, IOUT OC, OT, VIN OV) latch the output off. `CLEAR_FAULTS` clears
  the status bits but does not restart; turning `OPERATION` off and on again does.
- **VIN under-voltage** is a lockout, not a latch: the output returns when VIN does.
- **Warnings** only set status bits. Temperature limits apply to the hotter (and colder) of
  the two sensors.
- **Status bits** latch until `CLEAR_FAULTS` or a write-1-to-clear. If the condition
  persists they latch again at the next evaluation, every 200 ms.
- **SMBALERT#** goes low when a status bit newly sets. Reading the Alert Response Address
  releases it (and reports 0x58); so does clearing every status bit.
- **WRITE_PROTECT** 0x80/0x40/0x20 follow PMBus 1.3, except that `CLEAR_FAULTS` and `PAGE`
  stay writable at every level, so a host that clears faults after probing (Linux does)
  doesn't trip over a write-protected device.
- Writes take effect at STOP, which gives Group Command semantics.
- Writes with the wrong number of data bytes set `STATUS_CML` bit 6; a wrong PEC sets bit 5.
  Either way the write is ignored.
