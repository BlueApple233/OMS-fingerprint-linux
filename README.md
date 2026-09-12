# OMS fingerprint module — Linux driver

Reverse engineering project for the **Jiangxi OMS Microelectronics fingerprint
module (`33a7:2388`)** that ships in several laptops and has no Linux driver.
The goal is a proper [libfprint](https://fprint.freedesktop.org/) driver so the
reader works with `fprintd`, PAM and the desktop environments.

**Status: the driver works on real hardware** — `fprintd-enroll` and
`fprintd-verify` pass, and a hardware-free umockdev regression test is included.
An upstream merge request (patch) is prepared in [`driver/upstream/`](./driver/upstream).

```
$ fprintd-enroll          → Enroll result: enroll-completed
$ fprintd-verify          → Verify result: verify-match (done)
$ meson test omsmoc       → 1/1 drivers+custom - libfprint:omsmoc OK
```

## The device

| | |
|---|---|
| USB ID | `33a7:2388` (Jiangxi OMS Microelectronics / OFilm) |
| Endpoints | EP1 OUT `0x01` / EP1 IN `0x81`, bulk, 64 bytes |
| Matching | **Match-on-Chip** — the chip stores up to 16 templates and does the matching, so no fingerprint algorithm is needed on the host |
| Firmware | vendor flash tooling only (not part of this repository) |

## Protocol (reverse engineered)

Two layered and completely plain text (the vendor Windows driver uses no
handshake, no key exchange and no encryption either):

```
① transport   8 byte "USBC" request header + 4 byte "USBS" acknowledgement
              (00 40 = write 64 / 80 40 = read 64 / 80 20 = poll 32)
② frames      64 byte "EF01" frames, last two data bytes = 16 bit checksum

commands      0x34 chip serial · 0x1d template count · 0x1f index table
              0x32 identify · 0x31 enroll · 0x0c delete/clear · 0x30 cancel
identify      00 01 … = finger captured (NOT a match)
              00 05 00 SS 03 e8 = matched template in slot SS
              09 05 … = no match
```

The transport, the command parameters and the response semantics (including the
timing rules and the frame checksum) are documented in
[`driver/omsmoc.c`](./driver/omsmoc.c) (file header), and in
[`driver/README.md`](./driver/README.md) together with the hardware pitfalls
(autosuspend, wedged firmware, the status of every implemented command).

## Repository layout

```
README.md                  this file
driver/                    the libfprint driver and its tooling
  omsmoc.c/.h              driver source (upstream style, fpi_device + fpi_ssm)
  tools/oms-smoke.c        bring-up tool (info/verify/identify/enroll/list/delete/clear)
  tests/omsmoc/            umockdev regression test + sanitized recorded traffic
  tests/sanitize-capture.py scrub device identifying data from a new recording
  upstream/                ready-to-submit upstream patch, steps and issue/request text
  libfprint-oms.patch      same changes as a plain patch for a local build tree
  setup.sh                 fetch libfprint, apply patch, build, run the test
  README.md                build, debug, test, PAM and device notes
aur/                       Arch package (libfprint + this driver) as a stop-gap
LICENSE                    LGPL-2.1-or-later
```

## Building and testing

```bash
driver/setup.sh --test        # fetch libfprint 1.94.100, build, run the umockdev test
```

Requirements: `meson ninja gcc glib2 glib2-devel gusb libusb cairo pixman nss
gobject-introspection umockdev` (Arch names).

Hardware bring-up (real device, uses the built libfprint):

```bash
B=build/libfprint-build
$B/examples/oms-smoke info                 # open and print device state
$B/examples/oms-smoke verify 30 --slot 0   # press an enrolled finger
$B/examples/oms-smoke enroll 120           # press/lift 6 times
$B/examples/oms-smoke list                 # templates on the chip
$B/examples/oms-smoke delete <slot>        # delete one template
```

## Upstream status

The driver, the umockdev test case and the `hwdb` entries are ready as a single
patch in [`driver/upstream/`](./driver/upstream). It applies to upstream master,
builds there without warnings and passes the replay test. Not submitted yet.

## ⚠️ Device notes

* Never send an incomplete `USBC` sequence (header without data, or a bare
  `EF01` frame): it wedges the firmware.
* This device does **not** survive kernel autosuspend: it must stay powered
  (`power/control=on`) while a driver is being developed or used, e.g.

  ```
  ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="33a7", ATTR{idProduct}=="2388", \
      TAG+="uaccess", ATTR{power/control}="on"
  ```

* A wedged device (bulk transfers time out, enumeration still fine) recovers
  with a USB port reset, which can be done without root:

  ```bash
  python -c "import usb.core; usb.core.find(idVendor=0x33a7, idProduct=0x2388).reset()"
  ```

* Templates live in the chip only — treat them as the one irreplaceable state.

## Compliance

This is interoperability reverse engineering of hardware owned by the author.
No vendor binaries, firmware or raw captures are included in this repository;
the test recording shipped in `driver/tests/` has device identifying data
(chip serial, USB serial) removed. If you want to reproduce the captures, use
your own device and the vendor driver obtained from your hardware vendor.

## License

GNU Lesser General Public License 2.1 or later — see [`LICENSE`](./LICENSE),
the same license as libfprint so the driver can be merged upstream.
