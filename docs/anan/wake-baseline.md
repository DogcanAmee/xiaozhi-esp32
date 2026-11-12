# Wake-word baseline

## sdkconfig wake-word lines (from CI log)

<!-- Paste verbatim from the "Print wake-word sdkconfig lines" step of the firmware job. -->

Stock build (CI, unmodified board):

```
CONFIG_USE_AFE_WAKE_WORD=y
# CONFIG_USE_CUSTOM_WAKE_WORD is not set
CONFIG_SEND_WAKE_WORD_DATA=y
# CONFIG_WAKE_WORD_DETECTION_IN_LISTENING is not set
CONFIG_SR_WN_WN9_HIESP=y
```

说明：原版固件的唤醒词是 "Hi ESP"（`CONFIG_SR_WN_WN9_HIESP`），说话时（listening 状态）唤醒检测是关闭的。
目标是改成“你好小智”（`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS`），并在 listening 状态保持唤醒检测。
- 唤醒检测：板级 `config.json` 加 `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y`（已做）。
- 唤醒词型号：`scripts/build.py` 按 `--language` / `--wake-word` 选择，板级 `sdkconfig_append` 不能写（主机测试禁止，且会被覆盖）。通过构建参数切换：CI 构建命令加 `--language zh-CN --wake-word nihaoxiaozhi`（已做）。

## Results (successes out of 10)

| Timing | Before | After |
|---|---|---|
| Idle | | |
| While she is speaking | | |
| Within 2 s after she finishes | | |
