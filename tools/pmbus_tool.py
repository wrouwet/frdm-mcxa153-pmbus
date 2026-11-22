#!/usr/bin/env python3
"""Talk to a PMBus device from a host.

    pmbus_tool.py -t sim dump
    pmbus_tool.py -t hub:/dev/ttyACM1 dump
    pmbus_tool.py -t linux:3 -a 0x58 read 0x8b
    pmbus_tool.py -t linux:3 write-word 0x51 0xeb20
    pmbus_tool.py -t linux:3 alert

Transports: sim (needs `make sim-lib`), hub:<serial port> for the FRDM-MCXA153
USB-to-I2C hub, linux:<bus number> for /dev/i2c-N.
"""

import argparse
import sys

from pmbus import PMBusClient, PMBusError, open_transport
from pmbus import client as c

TELEMETRY = [
    ("READ_VIN", c.READ_VIN, "V", "lin"),
    ("READ_IIN", c.READ_IIN, "A", "lin"),
    ("READ_PIN", c.READ_PIN, "W", "lin"),
    ("READ_VOUT", c.READ_VOUT, "V", "vout"),
    ("READ_IOUT", c.READ_IOUT, "A", "lin"),
    ("READ_POUT", c.READ_POUT, "W", "lin"),
    ("READ_TEMPERATURE_1", c.READ_TEMPERATURE_1, "°C", "lin"),
    ("READ_TEMPERATURE_2", c.READ_TEMPERATURE_2, "°C", "lin"),
]

LIMITS = [
    ("VOUT_COMMAND", c.VOUT_COMMAND, "V", "vout"),
    ("VOUT_OV_FAULT_LIMIT", c.VOUT_OV_FAULT_LIMIT, "V", "vout"),
    ("VOUT_OV_WARN_LIMIT", c.VOUT_OV_WARN_LIMIT, "V", "vout"),
    ("VOUT_UV_WARN_LIMIT", c.VOUT_UV_WARN_LIMIT, "V", "vout"),
    ("VOUT_UV_FAULT_LIMIT", c.VOUT_UV_FAULT_LIMIT, "V", "vout"),
    ("IOUT_OC_FAULT_LIMIT", c.IOUT_OC_FAULT_LIMIT, "A", "lin"),
    ("IOUT_OC_WARN_LIMIT", c.IOUT_OC_WARN_LIMIT, "A", "lin"),
    ("OT_FAULT_LIMIT", c.OT_FAULT_LIMIT, "°C", "lin"),
    ("OT_WARN_LIMIT", c.OT_WARN_LIMIT, "°C", "lin"),
    ("VIN_OV_FAULT_LIMIT", c.VIN_OV_FAULT_LIMIT, "V", "lin"),
    ("VIN_UV_FAULT_LIMIT", c.VIN_UV_FAULT_LIMIT, "V", "lin"),
]

STRINGS = [
    ("MFR_ID", c.MFR_ID), ("MFR_MODEL", c.MFR_MODEL), ("MFR_REVISION", c.MFR_REVISION),
    ("MFR_LOCATION", c.MFR_LOCATION), ("MFR_DATE", c.MFR_DATE), ("MFR_SERIAL", c.MFR_SERIAL),
]

STATUS = [
    ("STATUS_WORD", c.STATUS_WORD, 2), ("STATUS_VOUT", c.STATUS_VOUT, 1),
    ("STATUS_IOUT", c.STATUS_IOUT, 1), ("STATUS_INPUT", c.STATUS_INPUT, 1),
    ("STATUS_TEMPERATURE", c.STATUS_TEMPERATURE, 1), ("STATUS_CML", c.STATUS_CML, 1),
]


def value(dev, cmd, kind):
    return dev.read_vout(cmd) if kind == "vout" else dev.read_linear11(cmd)


def cmd_dump(dev, args):
    print(f"PMBUS_REVISION 0x{dev.read_byte(c.PMBUS_REVISION):02x}  "
          f"CAPABILITY 0x{dev.read_byte(c.CAPABILITY):02x}  "
          f"VOUT_MODE 0x{dev.vout_mode():02x}  "
          f"OPERATION 0x{dev.read_byte(c.OPERATION):02x}  "
          f"WRITE_PROTECT 0x{dev.read_byte(c.WRITE_PROTECT):02x}")
    for name, cmd in STRINGS:
        print(f"{name:20} {dev.read_string(cmd)!r}")
    for group in (TELEMETRY, LIMITS):
        for name, cmd, unit, kind in group:
            try:
                print(f"{name:20} {value(dev, cmd, kind):10.3f} {unit}")
            except PMBusError as e:
                print(f"{name:20} ({e})")
    for name, cmd, size in STATUS:
        v = dev.read_word(cmd) if size == 2 else dev.read_byte(cmd)
        print(f"{name:20} 0x{v:0{2 * size}x}")


def cmd_read(dev, args):
    code = int(args.code, 0)
    if args.block:
        print(dev.block_read(code).hex(" "))
    elif args.byte:
        print(f"0x{dev.read_byte(code):02x}")
    else:
        w = dev.read_word(code)
        print(f"0x{w:04x}  (LINEAR11 {c.linear11_decode(w):g})")


def cmd_write_byte(dev, args):
    dev.write_byte(int(args.code, 0), int(args.value, 0))


def cmd_write_word(dev, args):
    dev.write_word(int(args.code, 0), int(args.value, 0))


def cmd_send(dev, args):
    dev.send_byte(int(args.code, 0))


def cmd_alert(dev, args):
    a = dev.alert_response()
    print("no device alerting" if a is None else f"alert from 0x{a:02x}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("-t", "--transport", default="sim", help="sim | hub:<port> | linux:<bus>")
    p.add_argument("-a", "--addr", default="0x58", help="7-bit device address")
    p.add_argument("--no-pec", action="store_true", help="don't use packet error checking")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("dump", help="identification, telemetry, limits and status").set_defaults(fn=cmd_dump)
    r = sub.add_parser("read", help="read a command (word by default)")
    r.add_argument("code")
    r.add_argument("--byte", action="store_true")
    r.add_argument("--block", action="store_true")
    r.set_defaults(fn=cmd_read)
    for name, fn in (("write-byte", cmd_write_byte), ("write-word", cmd_write_word)):
        w = sub.add_parser(name)
        w.add_argument("code")
        w.add_argument("value")
        w.set_defaults(fn=fn)
    s = sub.add_parser("send", help="Send Byte, e.g. 0x03 for CLEAR_FAULTS")
    s.add_argument("code")
    s.set_defaults(fn=cmd_send)
    sub.add_parser("alert", help="read the SMBus Alert Response Address").set_defaults(fn=cmd_alert)
    args = p.parse_args()

    addr = int(args.addr, 0)
    dev = PMBusClient(open_transport(args.transport, addr), addr, pec=not args.no_pec)
    try:
        args.fn(dev, args)
    except (PMBusError, OSError) as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
