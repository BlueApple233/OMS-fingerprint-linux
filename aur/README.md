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
makepkg -fC            # 构建（再次构建必须加 -C，否则 prepare() 因补丁已应用而失败）
# 安装（pacman 会提示删除发行版 libfprint，需确认 y；--noconfirm 会默认答 N 导致失败）
yes | sudo pacman -U libfprint-omsmoc-*.pkg.tar.zst
```

> makepkg 会在本目录生成 `src/`、`pkg/`、源码 tarball 与 `*.pkg.tar.zst`。
> 这些构建产物**不入库**（`.gitignore` 已覆盖 `/aur/**/src/`、`/aur/**/pkg/`、
> `*.pkg.tar.zst`、`*.tar.gz`）；提交前用 `git status --short` 确认一下即可。

### 包装细节（实测踩过的坑）

- `provides` 必须包含 soname：`provides=('libfprint' 'libfprint-2.so=2-64')`，
  否则 `fprintd` 的 `libfprint-2.so=2-64` 依赖无法满足，安装会失败。
- `conflicts` 必须限定版本：`conflicts=('libfprint>=1.90')`。系统里可能同时装有遗留的
  `libfprint-1`（`provides=('libfprint=1.0')`，`fingerprint-gui` 依赖它）；若写成
  `conflicts=('libfprint')`，pacman 会连 v1 库一起移除，进而因 `fingerprint-gui` 依赖破损
  而中止安装。

安装后**重启 fprintd**（`systemctl restart fprintd`，或让它按 D-Bus 激活），
然后 `fprintd-enroll` / `fprintd-verify` 即可使用；`fprintd-list` 应能看到设备。

卸载还原：`sudo pacman -R libfprint-omsmoc && sudo pacman -S libfprint`。

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

## 已验证（2026-09-12，本机 Arch）

- `makepkg -fC` 构建通过（1.94.100-1）；包内 `libfprint-2.so.2.0.0` 含 `FpiDeviceOmsMoc`，
  hwdb 中 `usb:v33A7p2388*` 位于**受支持**区块（并已从 known-unsupported 移除）
- 安装后系统 fprintd（**未用 LD_LIBRARY_PATH**，进程 maps 显示 `/usr/lib/libfprint-2.so.2.0.0`）实测：
  - `fprintd-list` → 识别设备与已注册指纹
  - `fprintd-enroll` → 六阶段 `enroll-stage-passed` → `enroll-completed`
  - `fprintd-verify`（已注册手指）→ `verify-match (done)`
  - `fprintd-verify`（未注册手指，健康手指）→ `verify-no-match (done)`
- `systemd-hwdb query 'usb:v33A7p2388*'` → `ID_AUTOSUSPEND=1` / `ID_PERSIST=0`
