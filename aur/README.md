# AUR 包（保底路线）

当「freedesktop GitLab 权限」还没下来、或上游还没合并时，用这个包就能让设备在本机可用。

```
aur/libfprint-omsmoc/
├── PKGBUILD            # libfprint 1.94.100 + omsmoc 驱动
├── omsmoc-driver.patch # 只含驱动/meson/hwdb 的补丁（不含测试夹具，patch(1) 友好）
└── .SRCINFO            # 由 makepkg --printsrcinfo 生成
```

## 本地构建 / 安装

```bash
cd aur/libfprint-omsmoc
makepkg -fi            # 构建并安装（会替换发行版的 libfprint）
# 或先只构建：
makepkg -f && sudo pacman -U libfprint-omsmoc-*.pkg.tar.zst
```

安装后**重启 fprintd**（`systemctl restart fprintd`，或让它按 D-Bus 激活），
然后 `fprintd-enroll` / `fprintd-verify` 即可使用；`fprintd-list` 应能看到设备。

> ⚠️ 该包 `provides/conflicts = libfprint`：它会**取代发行版 libfprint**。
> 发行版升级 libfprint 时 pacman 会提示冲突，需要重装本包或等上游合并。
> 卸载后 `pacman -S libfprint` 即可回到发行版版本。
> 本包关闭了 gtk-doc 文档（`-Ddoc=false`），其余文件与发行版包一致
> （lib/头文件/GIR/typelib/pkgconfig/udev 规则/hwdb/metainfo）。

## 上传到 AUR

AUR 账号注册是即时的（不需要像 freedesktop 那样等审批），只需加 SSH key：

```bash
# 1) 注册并添加 SSH key：https://aur.archlinux.org/ （Account → My Account → SSH Public Key）
# 2) 首次检出（包名需在 AUR 上尚未被占用；若被占用请改名，如 libfprint-omsmoc-git）
git clone ssh://aur@aur.archlinux.org/libfprint-omsmoc.git
cd libfprint-omsmoc
cp /path/to/OMS-fingerprint-linux/aur/libfprint-omsmoc/{PKGBUILD,.SRCINFO,omsmoc-driver.patch} .
git add PKGBUILD .SRCINFO omsmoc-driver.patch
git commit -m "Initial import: libfprint with the OMS match-on-chip driver"
git push
```

上传后可在 <https://aur.archlinux.org/packages?K=libfprint-omsmoc> 查看，
并在 libfprint 的 Driver Request issue 里附上该链接（对用户更友好的一条安装路径）。

## 已验证

- `makepkg -f` 构建通过（1.94.100-1）
- 包内 `libfprint-2.so.2.0.0` 含 `FpiDeviceOmsMoc`，hwdb 中 `usb:v33A7p2388*`
  位于**受支持**区块（并已从 known-unsupported 移除）
