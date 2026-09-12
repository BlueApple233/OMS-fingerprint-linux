#!/usr/bin/env python3
#
# omsmoc (OMS Match-on-Chip) umockdev test
# Copyright (C) 2026 The OMS Linux driver contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This test is run against the real hardware while recording (see
# tests/create-driver-test.py) and against the recorded USB traffic
# afterwards.  Everything here is read-only with respect to the chip
# storage, so recording the test never modifies an enrolled template.

import sys
import traceback

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, including those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

assert len(devices) == 1, 'Expected exactly one supported device'
d = devices[0]
del devices

assert d.get_driver() == 'omsmoc'
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)
assert d.get_scan_type() == FPrint.ScanType.PRESS
assert d.get_nr_enroll_stages() == 6

d.open_sync()

# The chip storage is only read here: listing the templates reads the
# index table (0x1f) and reports one print per occupied slot.
print('listing templates')
stored = d.list_prints_sync()
print('device reports %d template(s)' % len(stored))
assert len(stored) > 0, 'No template enrolled on the chip'

def verify_with_retries(fp_print, attempts=3):
    for attempt in range(attempts):
        print('verifying, press the finger for %s (attempt %d/%d)'
              % (fp_print.get_description(), attempt + 1, attempts))
        res, _print = d.verify_sync(fp_print)
        if res:
            return True
        print('the chip did not match the finger, try again')

    return False


# Verification of the first stored print.  The chip reports the slot it
# matched, and the driver only reports a match if that slot is the one of
# the requested print.
assert verify_with_retries(stored[0]), 'The enrolled finger was not recognised'
print('verify matched')

# Identification reports the print whose slot the chip matched.
identified = {'done': False, 'match': None}


def identify_done(dev, res):
    try:
        match, _print = dev.identify_finish(res)
        identified['match'] = match
    except GLib.Error as err:
        identified['error'] = err.message
    finally:
        identified['done'] = True


for attempt in range(3):
    identified.update({'done': False, 'match': None})
    print('identifying, press an enrolled finger again (attempt %d/3)' % (attempt + 1))
    d.identify(stored, callback=identify_done)
    while not identified['done']:
        ctx.iteration(True)

    assert 'error' not in identified, identified.get('error')
    if identified['match'] is not None:
        break

assert identified['match'] is not None, 'No match reported'
print('identify matched')

d.close_sync()

del d
del c
del stored
