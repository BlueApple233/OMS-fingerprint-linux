# libfprint「Driver Request」issue 文案（可直接粘贴）

- 提交位置：<https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/new>
- 标签：**Driver Request**（HACKING.md 指定的需求入口）
- 标题建议：`Driver Request: OMS match-on-chip sensor (33a7:2388) — driver available`

> 文案里的链接已填好，可整段直接复制粘贴。

---

```markdown
### Device

* USB ID: `33a7:2388` (Jiangxi OMS Microelectronics / OFilm fingerprint module)
* Found in several laptops (e.g. Mechrevo Wujie 14/15 Pro); no kernel driver,
  the device is accessed from userspace through a vendor specific interface
  with two bulk endpoints (EP1 OUT/IN, 64 byte packets).
* Match-on-chip: the chip stores up to 16 templates and performs the matching,
  so no host side fingerprint processing is required.

### Summary

I reverse engineered the vendor's Windows driver (static analysis of the DLLs,
USB captures of the driver with usbmon, an independent pyusb prototype on the
hardware) and wrote a libfprint driver for it. It is working on real hardware
here: `fprintd-enroll` completes its six stages and `fprintd-verify` returns
`verify-match`; an unenrolled finger correctly returns `verify-no-match`.

Repository (documentation, driver, bring-up tool, umockdev test):
https://github.com/BlueApple233/OMS-fingerprint-linux

Direct patch against master (single commit, driver + test case + hwdb):
https://github.com/BlueApple233/OMS-fingerprint-linux/blob/main/driver/upstream/0001-drivers-Add-support-for-OMS-match-on-chip-sensors.patch

Arch package as a stop-gap for users (built from the patch above):
https://github.com/BlueApple233/OMS-fingerprint-linux/tree/main/aur/libfprint-omsmoc

### Protocol

Plain text end to end: no handshake, no key exchange and no encryption (the
vendor driver behaves the same). Transport is an 8 byte `USBC` request header
with a 4 byte `USBS` acknowledgement around 64 byte `EF01` frames with a 16 bit
checksum. Commands used: `0x34` chip serial, `0x1d` template count, `0x1f`
index table, `0x32` identify, `0x31` enroll, `0x0c` delete/clear, `0x30`
cancel. The identify result reports the matched template slot, which allows
exact verification with several enrolled fingers and duplicate detection
during enrollment.

The frame format, the command parameters, the response semantics and the
hardware pitfalls are documented in the driver source (file header) and in
`driver/README.md`.

### Implemented

identify, verify, enroll, delete, clear-storage, listing; duplicate detection;
the driver follows the `fpi_device`/`fpi_ssm` framework like `goodixmoc` and
`elanmoc`, is LGPL-2.1-or-later and adds no public API.

### Testing

* Real hardware: `fprintd-enroll` (6 stages, `enroll-completed`),
  `fprintd-verify` (`verify-match`), negative cases (`verify-no-match` for an
  unenrolled finger and for another enrolled finger than the requested print),
  `enroll-duplicate` when the finger is already on the chip.
* PAM: `pam_fprintd` authentication succeeds for an enrolled finger and fails
  for an unenrolled one.
* No hardware needed: an umockdev test case recorded with
  `tests/create-driver-test.py` is included and passes
  (`meson test omsmoc`); device identifying data was removed from the recording.
* The patch applies to current master, builds without warnings and passes that
  test there.

### Merge request

I am happy to open a merge request with the patch once my freedesktop account
has passed the new-account permission check. If you prefer, the patch above can
also be applied directly, or I can attach it here.
```

---

## 备注

- 若已拿到权限：优先走 MR（见 [`README.md`](./README.md)），Driver Request issue 只作为
  「告知 + 留痕」。
- 若尚未拿到权限：先提本 issue，并在里面说明补丁可直接应用；等权限到位后再建 MR 并在这里回链。
- 上游若要求 3 台独立设备（HACKING.md 对「libfprint 开发者提交驱动」的要求），说明本驱动是
  外部贡献者提交、且已有实机与回放双重测试。
