# xone-mt7612fix

基于 [dlundqvist/xone](https://github.com/dlundqvist/xone) 的修改版，修复 **Xbox 无线适配器 045e:02fe（Model 1790，MT7612UN）** 初始化时报 `init radio failed: -110` 的问题，并新增一个 XInput（Xbox 360 协议）传输驱动。

## 问题

045e:02fe 适配器固件写入成功，但 `xone_mt76_init_radio()` 超时：

```
xone_dongle_fw_load: init radio timed out, retrying (1/3)
xone_mt76_init_radio: id=0x7613
xone_dongle_fw_load: init radio failed: -110
```

原因是 MT7612 硬件需要的等待时间比上游给的长：USB bulk 超时只有 1 秒，固件 USB 重连后的等待只有 500 毫秒。

## 修复

```
transport/mt76.c:
  XONE_MT_USB_TIMEOUT  1000 -> 3000
  msleep(500) -> msleep(2000)   (reset_firmware / load_firmware)
```

改完后 `init_radio` 每一步都能通过，无线手柄可以正常通过 `xone_gip` 连接。

## 为什么这不是“MT7613 专用初始化”

xone 的 MT76 初始化本来就是 xow 的 MT7612（mt76x2）序列的内核移植：

- `xone_mt76_init_registers()` 写入的寄存器和值与 xow `Mt76::initRegisters()` 完全一致
- `load_cr()` 只是发送 `MT_CMD_LOAD_CR` 命令，CR 数据在固件里，驱动没有 MT7613 专用表
- `xone_dongle_02fe.bin` 与 `xow_dongle.bin` 字节完全相同（md5 `8cecc98764d7975d4b0babe7a3b19314`）
- 日志里的 `0x7613` 是 efuse/固件报告的值，不代表走了不同的初始化路径

真正的差异是时序：045e:02fe 上的 MT7612 需要超过 1 秒的 USB bulk 超时、超过 500 毫秒的固件重连等待。所以这个 fork 只是放宽了参数，没有把初始化序列换成“MT7612 专用”的。

## 新增：XInput（Xbox 360）传输驱动

`transport/xinput.c` 新增了一个驱动模块 `xone_xinput`，用于支持**模拟 Xbox 360 有线手柄（045e:028e）的设备**，例如 ShanWan Q34U2（2.4G 接收器会伪装成 045e:028e）。

- 协议：标准 XInput（Interrupt IN 0x81 / Interrupt OUT 0x03）
- 支持按键、摇杆、扳机、十字键（HAT）
- 力反馈（rumble）使用异步 URB（`usb_submit_urb` + `GFP_ATOMIC`），不会在原子上下文睡眠
- 重复帧会跳过（数据与上一帧相同不产生事件）
- 插上设备后由 udev 自动加载（`modules.alias`）

## 测试环境

- Xbox 无线适配器 `045e:02fe`（Model 1790，MT7612UN）
- Xbox Series 手柄（无线）
- ShanWan Q34U2（USB 有线 / 2.4G 伪装成 `045e:028e`）
- Arch Linux，内核 `7.3.6-x64v3-xanmod-custom`，DKMS 构建
- `xone_dongle` + `xone_gip` + `xone_xinput`

## xone-fix DKMS 包

`packaging/PKGBUILD` 用于构建 `xone-fix` DKMS 包（替代 `xone-dkms-git`），包含 MT7612 超时修复和 XInput 驱动。

先生成源码 tar：

```bash
pkgver=0.5.8.r2.g30317da
rsync -a --exclude=.git . "/tmp/xone-fix-$pkgver/"
tar -C /tmp -czf "xone-fix-$pkgver.tar.gz" "xone-fix-$pkgver"
```

把 tar 放到 `packaging/PKGBUILD` 同目录，然后：

```bash
makepkg -si
```

包信息：

- `provides=('xone-dkms')`
- `conflicts=('xone-dkms' 'xone-dkms-git')`
- 安装到 `/usr/src/xone-$pkgver`，由 DKMS hook 自动构建
- 内核升级（安装新 headers）后会自动重新编译

## 构建与测试

### 依赖

- Linux 6.5+
- Linux headers

### 自动

```bash
sudo make test
```

### 手动

```bash
make            # 构建
make debug      # 带调试日志构建
sudo make load  # 加载模块
sudo make unload
make clean
```

## 安装

### 依赖

- Linux（内核 5.13+ 与 headers）
- DKMS
- curl（下载固件）
- bsdtar（解压固件）
- SecureBoot 系统参见 [SecureBoot dkms guide](https://github.com/dell/dkms#secure-boot)

### 步骤

1. 拔掉 Xbox 设备
2. 克隆仓库
3. 安装：
   ```bash
   sudo make install
   ```
   （调试版用 `install-debug`）
4. 下载无线适配器固件（可选，makefile 会自动安装）：
   ```bash
   sudo install/firmware.sh
   ```
   `--skip-disclaimer` 可用于脚本化。
5. 插回 Xbox 设备

### 更新

```bash
git pull
sudo make install
```

建议重启。

## 兼容性

- [x] 有线设备（USB）
- [x] 无线设备（Xbox 无线适配器）
- [ ] 蓝牙设备（请用 [`xpadneo`](https://github.com/atar-axis/xpadneo)）

安装 `xone` 会禁用 `xpad`。如果还要用 Xbox 360 外设，需要装 [`xpad-noone`](https://github.com/forkymcforkface/xpad-noone) 替代 `xpad`。

## 功能

- [x] 输入与力反馈（震动）
- [x] 电量上报（`UPower`）
- [x] LED 控制（`/sys/class/leds`）
- [x] 音频采集/播放（`ALSA`）
- [x] 电源管理（挂起/恢复、无线唤醒）

## 支持的设备

- [x] 手柄
    - [x] Xbox One 手柄
    - [x] Xbox Series X|S 手柄
    - [x] Xbox Adaptive Controller
    - [x] 第三方手柄（PowerA、PDP 等）
    - [x] 模拟 Xbox 360 有线手柄的设备（`xone_xinput`，如 ShanWan Q34U2）
- [x] 耳机
    - [x] Xbox One Chat Headset
    - [x] Xbox One Stereo Headset
    - [x] Xbox Wireless Headset
    - [x] 第三方有线/无线耳机（SteelSeries、Razer 等）
- [x] 吉他/鼓
    - [x] Mad Catz Rock Band 4 Wireless Fender Stratocaster
    - [x] Mad Catz Rock Band 4 Wireless Drum Kit
    - [x] PDP Rock Band 4 Wireless Fender Jaguar
- [x] Xbox One Chatpad
- [ ] 第三方赛车方向盘（Thrustmaster、Logitech 等）

## 无线配对

Xbox 设备需要先与无线适配器配对。之前用 USB 线或蓝牙连接过的手柄不会自动连上适配器。

配对说明见 [微软官方文档](https://support.xbox.com/en-US/help/hardware-network/controller/connect-xbox-wireless-controller-to-pc)（*Xbox Wireless* 部分）。

## 内核接口

### LED 控制

```bash
echo 2 | sudo tee /sys/class/leds/gip*/mode
echo 5 | sudo tee /sys/class/leds/gip*/brightness
```

临时生效，设备断开即恢复。要自动应用，可创建 `/etc/udev/rules.d/50-xone.rules`：

```
ACTION=="add", SUBSYSTEM=="leds", KERNEL=="gip*", ATTR{mode}="2", ATTR{brightness}="5"
```

### 配对模式

```bash
cat /sys/bus/usb/drivers/xone-dongle/*/pairing
echo 1 | sudo tee /sys/bus/usb/drivers/xone-dongle/*/pairing   # 开启
echo 0 | sudo tee /sys/bus/usb/drivers/xone-dongle/*/pairing   # 关闭
```

### 连接数与强制关机

```bash
# 已连接手柄数
cat /sys/bus/usb/drivers/xone-dongle/*/active_clients

# 关闭指定客户端（0-15）
sudo tee /sys/bus/usb/drivers/xone-dongle/*/poweroff <<< 1

# 关闭所有
sudo tee /sys/bus/usb/drivers/xone-dongle/*/poweroff <<< -1
```

## 故障排查

- 先卸载 release 版，装 debug 版（见安装部分）
- `sudo dmesg` 查看 xone 相关错误
- 如果 xone 没有自动加载，可能需要重启

### 常见错误

- `Direct firmware load for xow_dongle.bin failed with error -2`
    - 下载无线适配器固件（见安装部分）

### 输入问题

可以用 `evtest` 和 `fftest` 检查输入与力反馈。

- `045e:028e` 设备（XInput）如果没被识别，确认 `xone_xinput` 已加载：
  ```bash
  lsmod | grep xone_xinput
  ```

## 许可证

`xone` 使用 [GNU General Public License, Version 2](LICENSE)。

```
Copyright (C) 2021 Severin von Wnuck-Lipinski

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
```
