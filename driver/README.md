# omsmoc — OMS Match-on-Chip libfprint 驱动

> 目标设备：`33a7:2388`（江西欧迈斯 / 欧菲光指纹模块，Match-on-Chip）
> 状态：**已在真实硬件上跑通 `fprintd-enroll` / `fprintd-verify`**；无硬件 umockdev 回放测试通过
> 协议依据：驱动实现细节与实测结论都写在本文件与 `omsmoc.c` 头部注释里；
> 命令/响应语义见 §5，设备注意事项见 §7。

本目录是驱动源码与配套工具的**唯一事实来源**：源码按上游 libfprint 的目录布局组织，
通过 [`setup.sh`](./setup.sh) 挂进一份干净的 libfprint 源码树里构建，
因此这些文件将来可以直接提交上游（见 [`libfprint-oms.patch`](./libfprint-oms.patch)）。

---

## 1. 目录结构

| 文件 | 说明 |
|---|---|
| `omsmoc.c` / `omsmoc.h` | 驱动本体（放进 libfprint 的 `libfprint/drivers/`） |
| `tools/oms-smoke.c` | 硬件调试工具（只依赖公开 API；放进 `examples/`） |
| `tests/omsmoc/custom.py` | umockdev 回归测试脚本（放进 `tests/omsmoc/`） |
| `tests/omsmoc/custom.pcapng` | 回放数据（实测录制，芯片序列号等设备特征已脱敏） |
| `tests/omsmoc/device` | umockdev 设备描述（`umockdev-record` 生成，同样已脱敏） |
| `libfprint-oms.patch` | 对 libfprint 的改动（meson 注册、hwdb 支持列表），用于本地开发树 |
| `upstream/` | **上游 MR 交付物**：`git format-patch` 补丁 + 提交步骤 + 可直接粘贴的 MR 描述 |
| `setup.sh` | 拉取 libfprint、打补丁、软链源码、构建、可选跑测试 |

## 2. 构建

依赖（Arch）：`base-devel meson ninja glib2 glib2-devel gusb libusb cairo pixman nss gobject-introspection umockdev`。

```bash
driver/setup.sh              # 拉取 libfprint 1.94.100 + 打补丁 + 构建
driver/setup.sh --test       # 构建后跑 umockdev 回放测试（无需硬件）
```

产物在 `build/`（已 gitignore）：`build/libfprint-build/libfprint/libfprint-2.so.2.0.0`
与 `build/libfprint-build/examples/oms-smoke`。

## 3. 硬件调试工具

```bash
B=build/libfprint-build
$B/examples/oms-smoke info                # 打开/初始化并打印设备信息
$B/examples/oms-smoke verify 30 --slot 0  # 识别：按指定槽位对应的手指（省略 --slot 用第一个槽位）
$B/examples/oms-smoke identify 30         # 同上，走 identify 接口
$B/examples/oms-smoke enroll 120          # 注册：按下→抬起 共 6 次（每级一次）
$B/examples/oms-smoke list                # 列出芯片内模板（槽位）
$B/examples/oms-smoke delete <slot>       # 删除指定槽位模板
$B/examples/oms-smoke clear               # 清空整个模板区（危险：所有模板都会丢）
```

> `verify` 使用的是**芯片上真实存在的 print**（先 `list` 再按描述匹配槽位），
> 因为驱动会校验「命中槽位 == 请求 print 的槽位」。

## 4. fprintd

系统 fprintd 需指向本构建的 libfprint：

```bash
sudo systemctl stop fprintd                     # 若已由 D-Bus 激活
sudo env LD_LIBRARY_PATH=$PWD/build/libfprint-build/libfprint /usr/lib/fprintd --no-timeout &
fprintd-enroll      # 按 6 次手指
fprintd-verify      # 按 1 次手指
```

实测结果（2026-09-12，OMS 33a7:2388）：`fprintd-enroll` 六阶段通过、`enroll-completed`；
`fprintd-verify` 返回 `verify-match (done)`；未注册手指返回 `verify-no-match (done)`。

### 4.1 PAM 集成验证（不动系统配置）

用**独立的 PAM 服务**验证，避免影响 login/sudo：

```bash
# 1) 建一个只用于测试的服务（用后即删）
printf 'auth       sufficient pam_fprintd.so\nauth       required   pam_deny.so\n' |
    sudo tee /etc/pam.d/oms-fprint-test

# 2) 起我们的 fprintd（见上）
# 3) 触发认证：任意使用 PAM 的小程序，或 pamtester（AUR）
#    实测（自写的 40 行 pam_authenticate 调用）：
#      已注册手指 → pam_authenticate: Success
#      未注册手指 → 3 次 “Failed to match fingerprint” → Authentication failure

sudo rm -f /etc/pam.d/oms-fprint-test        # 清理
```

> 配置系统级 `login`/`sudo`（`system-auth`）前请先保留一个 root shell 或 TTY 兜底，
> 避免指纹不可用时把自己锁在外面。

## 5. 已实现的功能与对应命令

| 功能 | 命令 | 说明 |
|---|---|---|
| 初始化 | `0x34` / `0x1d` / `0x1f` | 芯片序列号、模板数量、索引位图（16 槽） |
| verify / identify | `0x32` + 轮询 + `0x30` | 参数 `05 FF FF 00 00`；**结果帧见下表** |
| enroll | `0x32`（采集+查重）→ `0x1f` → `0x31` → 轮询 → `0x1d`/`0x1f` 确认 | 参数 `[00][slot][06][00][00]` |
| list | `0x1f` | 每个占用槽位生成一个 print（`fpi-data = (q) slot`） |
| delete | `0x0c` `[00][slot][00][01]` | 删除单个槽位（实测：其余槽位不受影响） |
| clear_storage | `0x0c` `[00][00][00][1e]` | 清空模板区（实测：全部模板消失） |

**`0x32` 响应语义（2026-09-12 实机确认，纠正了 Phase 2 的误读）**

| 响应 payload | 含义 |
|---|---|
| `00 00 XX XX 00 00` | 未就绪（XX XX 为请求参数回显） |
| `00 01 XX XX 00 00` | 指纹已采集（**未注册的手指同样会触发**，不能当作命中） |
| `00 05 00 SS 03 e8` | **匹配成功，SS = 命中槽位（0 基）** |
| `09 05 ff ff 00 00` | 搜索完成、未匹配 |

驱动据此实现**精确** verify/identify：只有命中槽位与请求 print 的槽位一致才算匹配；
enroll 前的同一条 `0x32` 兼作查重（命中即报 `FP_DEVICE_ERROR_DATA_DUPLICATE`）。
`0x32` 参数中的两个字节被芯片原样回显但**不限制搜索范围**（已实测：掩码指向空槽位时仍会命中）。

传输层（USBC 请求头 / USBS 应答 / EF01 帧 / 校验和 / 60 ms 轮询节奏）严格按抓包实现，
细节见 `omsmoc.c` 顶部注释。

## 6. 已知限制与未确认项

1. `0x32` 参数中的两个字节（默认 `FF FF`）语义未明：芯片会回显但**不用于过滤**（已实测）。
   多模板下的命中归属改由结果帧的槽位字节解决，因此不影响正确性。
2. `0x34` 返回的 32 字节序列号语义未知（前 4 字节为可见 ASCII），仅用于日志。
3. 目前只在一种固件（`bcdDevice 0x0200`）上验证过；换固件版本需重跑 `driver/setup.sh --test`
   与实机冒烟测试。
4. 注册需要「按下→抬起」共 6 级；按压质量差时芯片可能采集到图像但不匹配（`09 05`），
   此时驱动如实报告未匹配，客户端可提示重试。
5. 每次 `0x32` 会话会留下结束帧，驱动在会话前后都会 drain，避免污染下一次命令。

## 7. 设备注意事项（与 Phase 2 结论一致，务必遵守）

- **绝不要发不完整的 USBC 序列**（只发请求头、或裸发 EF01 帧都会卡死固件，已踩过两次）。
- 该设备**扛不住内核 autosuspend**：必须常供电（`power/control=on`），udev 规则可用：

  ```
  ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="33a7", ATTR{idProduct}=="2388", \
      TAG+="uaccess", ATTR{power/control}="on"
  ```

- 卡死恢复：USB 端口复位
  `python3 -c "import usb.core; usb.core.find(idVendor=0x33a7, idProduct=0x2388).reset()"`
  （普通用户权限即可，需 pyusb）；若枚举成 `1234:abce`（引导模式），需要用厂商烧录工具做固件恢复
  （会 ChipErase，芯片内模板全丢）。
- 模板是唯一不可再生状态：删除前确认目标槽位；`clear` 会清空全部模板。

## 8. 回归测试（无需硬件）

`tests/omsmoc/custom.py` 会打开设备、列模板、verify、identify；录制/回放由 libfprint 的
`tests/create-driver-test.py` + `tests/umockdev-test.py` 完成：

```bash
driver/setup.sh --test                                  # 直接跑回放
meson test -C build/libfprint-build omsmoc              # 同上（等价）

# 重新录制（需要真实硬件 + 手指按压，root 权限）
sudo build/libfprint-build/tests/create-driver-test.py --test custom omsmoc
# 脱敏（把芯片序列号与 USB 序列号替换为占位值，并重算帧校验和）
python3 driver/tests/sanitize-capture.py \
        driver/tests/omsmoc/custom.pcapng driver/tests/omsmoc/device
```

`custom.py` 只做只读操作（列模板 / verify / identify），因此**录制过程不会改动芯片模板**；
按压不稳定时脚本会自动重试（芯片可能采集到图像但判定不匹配）。

## 9. 合规

只提交自写代码、文档与自制测试数据（回放数据已脱敏）；
厂商二进制（DLL / 固件 / 驱动包）不入库，使用者自备。
