# CoreS3 中文定制固件（cores3-zh）

这是 [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
的 M5Stack CoreS3 定制分支，在官方固件之上增加了中文界面、离线中文字体和
MiMo 语音合成。仓库：`schau0826-crypto/muse-gadget-sdk`，分支 `cores3-zh`。

## 功能

- **中文回复页**：CoreS3 的 320×240 横屏上使用 19 列 × 5 行的阅读页，
  支持 UTF-8 断行、中文标点避头尾、分页和头像缩小排版。
- **离线中文字体**：Noto Sans CJK SC Regular 16 px、2 bpp 位图，21,105 个
  字形（U+4E00–U+9FFF、中文标点、全角形式），ASCII 回退到现有 unscii-16。
  字体文件在 `esp32/components/muse/fonts/muse_font_cjk_16.c`，许可为 SIL OFL 1.1。
- **MiMo TTS**：回复通过小米 MiMo 的流式 TTS 朗读，配置项
  `CONFIG_MUSE_MIMO_TTS_API_KEY`、`MIMO_TTS_MODEL`、`MIMO_TTS_VOICE`、
  `MIMO_TTS_STYLE`（见 `esp32/components/muse/Kconfig`）。
- **MiMo ASR**：按住 PWR 录音，松开后把 16 kHz PCM 编码为 WAV 发给
  `mimo-v2.5-asr`（`CONFIG_MUSE_MIMO_ASR=y`，语言用
  `CONFIG_MUSE_MIMO_ASR_LANGUAGE`），识别出的文本作为文本回合发给 Muse。
  录音上限 10 秒，使用同一个 API key；识别失败时提示“没听清”。关闭该项时
  录音仍按上游方式作为 voice note 发给 Muse 自己的 ASR。
- **字幕流式**：回复文字随语音逐段显示。
- CoreS3 的 sdkconfig 打开 `CONFIG_MUSE_CJK_FONT=y` 和
  `CONFIG_LV_FONT_FMT_TXT_LARGE=y`，后者用于超过 1 MB 的字体位图。

## 与上游的关系

- 基线提交：`e4d5cdf`（官方加入 CoreS3 支持，2026-10-04）。
- 已合并上游 `f60347a`（2026-10-16，含 111 个提交）的通用固件修复和
  可选的 CJK caption 支持。
- 上游 remote 为 `upstream`，定期同步：

  ```sh
  git fetch upstream
  git merge upstream/main      # 或 git rebase upstream/main
  ```

- 本地定制集中在中文排版（`muse_ui.c`、`muse_chat_text.c`、`muse_text.*`）、
  字体（`components/muse/fonts/`）和 TTS（`muse_tts_mimo.*`、
  `muse_chat_session.cpp`、`muse_voice.c`）。

## 构建

GitHub Actions 在 push 到 `cores3-zh` 和打 `zh-v*` tag 时自动构建，产物为
`cores3-zh-firmware` artifact；tag 会创建 Release。工作流见
`.github/workflows/cores3-zh.yml`。

本机构建（需要 ESP-IDF v6.0.1）：

```sh
export IDF_TOOLS_PATH=/path/to/espressif
export IDF_COMPONENT_CACHE_PATH=/path/to/component-cache
. /path/to/esp-idf-v6.0.1/export.sh
cd esp32
idf.py -B build-muse-m5stack-cores3 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-m5stack-cores3/sdkconfig \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-cores3' \
  build
```

产品：`build-muse-m5stack-cores3/muse-gadget.bin`。

## 刷写

只更新应用分区（保留 NVS 里的 Wi-Fi 和 SDK token，推荐）：

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodem1101 write_flash 0x20000 muse-gadget.bin
```

完整刷写用 Release 里的 `flash_args`（包含 bootloader、分区表、otadata）：

```sh
cd release-files
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 write_flash @flash_args
```

CI 构建的固件**不含 SDK token**（仓库公开，token 不能写进固件）。已配对过的
设备只刷应用分区时 NVS 里的 token 会保留；全新设备需要用
[官方网页刷机页](https://facebookincubator.github.io/muse-gadget-sdk/)
或串口 `>sdk_token=mgst_…` 设置一次。

## 发版

```sh
git tag zh-v0.1.0
git push origin zh-v0.1.0
```

Release 附件：`muse-gadget.bin`、`bootloader.bin`、`partition-table.bin`、
`ota_data_initial.bin`、`flash_args`。

## 已知限制

- 字体覆盖简体/繁体常用汉字和全角标点，不含日文假名、emoji、扩展 A/B 汉字。
- MiMo TTS 需要有效的 API key；未配置时回复只显示字幕。
- 中文界面和字体只针对 CoreS3 屏幕排版；其他板型沿用上游行为。
