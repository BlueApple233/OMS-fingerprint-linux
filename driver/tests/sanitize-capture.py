#!/usr/bin/env python3
"""Scrub device identifying data from a recorded umockdev test case.

The libfprint test cases recorded by ``tests/create-driver-test.py`` contain
raw USB traffic of the developer's own device: the chip serial number (from
the 0x34 response) and the USB serial string descriptor.  Both identify the
device, so before committing a recording they are replaced with fixed
placeholders here.  Frame checksums are recalculated afterwards, otherwise
the driver would reject the modified response frames.

Usage:
    sanitize-capture.py <custom.pcapng> [<device>]

The device file (umockdev description) is optional; when given, the ASCII
occurrences of the serial are replaced there as well.
"""

import argparse
import re
import sys

# EF01 response frame with L = 35: the chip serial number response is
# "00" + 32 bytes of serial data (the index table response has the same
# length, so it is told apart by its printable ASCII prefix).
SERIAL_FRAME_MIN_PRINTABLE = 4

PLACEHOLDER = b'OMS-TEST' + b'\x00' * 8


def scrub_serial_frame(data, path):
    offset = 0
    scrubbed = 0

    while True:
        start = data.find(b'\xef\x01\xff\xff\xff\xff\x07\x00\x23\x00', offset)
        if start < 0:
            break

        # start + 9 is the status byte, start + 10 the first byte of the
        # 32 byte serial number; the first four bytes are printable ASCII.
        serial = data[start + 10:start + 14]
        if all(32 <= byte < 127 for byte in serial):
            data[start + 10:start + 10 + len(PLACEHOLDER)] = PLACEHOLDER
            scrubbed += 1

        offset = start + 1

    if scrubbed:
        print(f'{path}: {scrubbed} chip serial frame(s) replaced')

    return scrubbed


def fix_checksums(data, path):
    offset = 0
    fixed = 0

    while True:
        start = data.find(b'\xef\x01\xff\xff\xff\xff\x07\x00', offset)
        if start < 0:
            break

        length = data[start + 8]
        end = start + 9 + length - 2
        checksum = sum(data[start + 6:end]) & 0xffff
        if data[end] != (checksum >> 8) or data[end + 1] != (checksum & 0xff):
            data[end] = (checksum >> 8) & 0xff
            data[end + 1] = checksum & 0xff
            fixed += 1

        offset = start + 1

    if fixed:
        print(f'{path}: {fixed} frame checksum(s) recalculated')

    return fixed


def find_usb_serial(data):
    match = re.search(rb'^E: ID_SERIAL_SHORT=([\x20-\x7e]+)', data, re.MULTILINE)
    if not match:
        match = re.search(rb'^A: serial=([\x20-\x7e]+)', data, re.MULTILINE)

    return match.group(1).decode() if match else None


def scrub_ascii(data, value):
    replaced = data.count(value)

    return data.replace(value, b'oms-test-dev000'.ljust(len(value), b'0')), replaced


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcapng', help='recorded custom.pcapng')
    parser.add_argument('device', nargs='?', help='recorded umockdev device file')
    args = parser.parse_args()

    data = bytearray(open(args.pcapng, 'rb').read())
    device = open(args.device, 'rb').read() if args.device else None

    serial = find_usb_serial(device) if device is not None else None

    if not scrub_serial_frame(data, args.pcapng):
        print(f'{args.pcapng}: no chip serial frame found', file=sys.stderr)

    if serial:
        placeholder = 'oms-test-dev000'[:len(serial)].ljust(len(serial), '0')

        for encoding in ('ascii', 'utf-16-le', 'utf-16-be'):
            needle = serial.encode(encoding)
            replacement = placeholder.encode(encoding)

            for name, blob in (('pcapng', bytes(data)), ('device', device)):
                if blob is None:
                    continue
                count = blob.count(needle)
                if count:
                    print(f'{name}: {count} {encoding} serial occurrence(s) replaced')
                if name == 'pcapng':
                    data = bytearray(blob.replace(needle, replacement))
                else:
                    device = blob.replace(needle, replacement)

        upper = serial.upper()
        if upper != serial:
            for name, blob in (('pcapng', bytes(data)), ('device', device)):
                if blob is None:
                    continue
                count = blob.count(upper.encode('ascii'))
                if count:
                    print(f'{name}: {count} uppercase serial occurrence(s) replaced')
                if name == 'pcapng':
                    data = bytearray(blob.replace(upper.encode('ascii'),
                                                  placeholder.upper().encode('ascii')))
                else:
                    device = blob.replace(upper.encode('ascii'),
                                          placeholder.upper().encode('ascii'))
    elif args.device:
        print(f'{args.device}: no USB serial found to scrub', file=sys.stderr)

    fix_checksums(data, args.pcapng)

    open(args.pcapng, 'wb').write(bytes(data))
    if device is not None:
        open(args.device, 'wb').write(device)


if __name__ == '__main__':
    main()
