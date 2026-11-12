# 烧录安安固件（StackChan）

CI（GitHub Actions 的 `Anan Stack-chan` 工作流）构建完成后，会产出名为 `anan-stackchan-firmware` 的 artifact。
下载后解压，里面有 `build/` 和 `releases/` 两个文件夹，文件如下（下表只写文件名，前三个在 `build/` 里）：

| 文件 | 用途 |
|---|---|
| `merged-binary.bin` | 完整镜像（bootloader、分区表、应用、资源），整包烧到 `0x0` |
| `xiaozhi.bin` | 只有应用固件，烧到 `ota_0` 分区（`0x20000`） |
| `configured_assets.bin` | 只有资源分区（表情动画、模型），烧到 `assets` 分区（`0x800000`） |
| `releases/*.zip` | `merged-binary.bin` 的压缩包 |

分区偏移来自 `partitions/v2/16m.csv`：`ota_0` 在 `0x20000`，`assets` 在 `0x800000`（8 MB）。

> [!WARNING]
> 不要用网页烧录工具。2026-11-12 实测它会把 8 MB 之后的数据写坏（资源分区损坏，表情不显示）。
> 一律用下面的 `esptool` 命令行。

## 准备

1. 安装 esptool：`python3 -m pip install esptool`。
2. 进入下载模式：长按复位按键约 3 秒，直到内部指示灯亮绿色，松开。
3. 找串口名。Mac 上终端运行：

   ```sh
   ls /dev/cu.usbmodem*
   ```

   输出类似 `/dev/cu.usbmodem1101`，下面用 `<串口>` 代替。设备没进下载模式或线没插好时，这条命令找不到东西。

## 选对文件

“下载”文件夹里往往留着以前的旧文件（`merged-binary.bin`、`merged-binary (1).bin` 之类）。烧之前确认用的是这次构建的：

- 用最新解压出来的那个文件夹，不要混用不同次下载的文件；
- 对一下文件大小：到 GitHub Actions 这次运行的日志里，打开 `Print firmware file sizes and checksums` 这一步，
  里面有三个文件的 `ls -l` 字节数和 `sha256sum`。Mac 上用 `ls -l <文件>` 看字节数，和日志对一下；
  想更保险就用 `shasum -a 256 <文件>` 对哈希。
  `configured_assets.bin` 必须不超过 `8388608` 字节（8 MB）。

## 方法一：首次烧录，或分区表 / bootloader / ESP-IDF 版本变了（整包）

```sh
python3 -m esptool --chip esp32s3 -p <串口> -b 460800 write_flash 0x0 merged-binary.bin
```

> [!NOTE]
> 整包镜像覆盖了 NVS 分区，**会清掉 Wi-Fi 配置**（以及设备绑定）。烧完要重新配网、重新绑定。

## 方法二：日常更新（保留 Wi-Fi 和绑定）

只在 bootloader、分区表和 ESP-IDF 版本都和上次整包烧录时一样、并且设备从 `ota_0` 启动时用；否则用方法一。

```sh
python3 -m esptool --chip esp32s3 -p <串口> -b 460800 write_flash 0x20000 xiaozhi.bin 0x800000 configured_assets.bin
```

只改了应用或表情时用这个。它只写 `ota_0` 和 `assets`，**不写** bootloader（`0x0`）、分区表（`0x8000`）和 otadata（`0xd000`），也不碰 NVS，所以 Wi-Fi 配置和绑定都在。

> [!CAUTION]
> 这条命令把应用写到 `ota_0`，只有在设备**从 `ota_0` 启动**时才对：整包烧录之后是这样，
> 只要之后没有发生过空中升级（OTA）把启动分区切到 `ota_1`，就一直是这样。
> 如果日常更新之后设备没有运行新固件（表情还是旧的，或行为没变），改用方法一整包烧录。

如果日常更新后设备仍启动旧固件（因为一次 OTA 把启动分区切到了 `ota_1`），可以先试这条，清掉 otadata，让设备重新从 `ota_0` 启动：

```sh
python3 -m esptool --chip esp32s3 -p <串口> erase_region 0xd000 0x2000
```

拿不准的时候，方法一永远可用，代价只是重新输入 Wi-Fi。

## 烧完之后

烧完 esptool 会自动复位设备。没有自动重启就按一下复位键。
