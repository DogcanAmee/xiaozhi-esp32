# 安安表情素材脚本

把安安的表情图转成 StackChan 板子能播的 `.eaf` 动画（320×240、8 位调色板、RLE、5 fps），
同时生成 `emote.json` 和待机小动作列表。生成物提交进仓库，编译固件不需要 Pillow。

只依赖 Python 3 和 Pillow，版本固定为 12.3.0（`pip install Pillow==12.3.0`）。
不同 Pillow 版本量化出的调色板可能不同，CI 会重新生成 `.eaf` 并与提交的文件逐字节比对，所以本地和 CI 都要用这个版本。

## 文件

| 路径 | 说明 |
|---|---|
| `eaf.py` | `.eaf` 读写：`read`、`parse_frame`、`build_frame`、`write` |
| `build_faces.py` | `prepare`（原图 → 640×480 源图）和 `build`（源图 → 生成物） |
| `tests/` | 单元测试 |
| `main/boards/m5stack/stackchan/assets-src/anan/` | 21 张裁好边的 640×480 源图（提交） |
| `main/boards/m5stack/stackchan/emoji/anan_*.eaf` | 生成的动画（提交） |
| `main/boards/m5stack/stackchan/assets/320_240/emote.json` | 生成的表情表（提交） |
| `main/boards/m5stack/stackchan/anan_idle_actions.inc` | 生成的待机小动作列表（提交） |

21 个表情名：`neutral happy laughing funny sad angry crying loving embarrassed surprised shocked thinking winking cool relaxed delicious kissy confident sleepy silly confused`。

## 换图

1. 把新原图放进一个目录，文件名是表情名，例如 `happy.png`。原图要求：
   - 宽高比 4:3（误差 2% 以内，会居中裁成正好 4:3）；
   - 灰色圆角边框在外圈 8% 以内（脚本四边各裁 8%）。
   `prepare` 一次处理全部 21 张，缺一张就报错并说出名字。
2. 在仓库根目录运行：

   ```sh
   B=main/boards/m5stack/stackchan
   python3 scripts/anan_faces/build_faces.py prepare <原图目录> $B/assets-src/anan
   python3 scripts/anan_faces/build_faces.py build $B/assets-src/anan $B
   ```

   如果只换其中一张，也可以直接把裁好的 640×480 PNG 放进 `assets-src/anan/`，只跑 `build`。
3. `prepare` 会检查边框是否裁干净：取原图最外圈（短边 1% 宽）的颜色中位数作为边框色，
   裁好后四个角（边长为高度 6% 的方块）里与边框色每个通道相差都不超过 40 的像素占到 1% 就报错。
   报错说明边框比 8% 宽，需要先手动裁一下原图。
4. 跑测试，确认后提交 `assets-src/anan/` 和全部生成物。

## 加一个待机小动作

1. 待机小动作用的表情图必须是 21 张之一（例如 `sleepy`）。如果要新表情图，先按上面的方法加图，
   并把名字加进 `build_faces.py` 的 `FACE_NAMES`。
2. 在 `build_faces.py` 的 `IDLE_ACTIONS` 表里加一行：

   ```python
   ("idle_yawn", "sleepy", 10),   # 名字、用哪张图、这张图播几帧
   ```

   播放顺序固定为：neutral×2、该图×帧数、neutral×2；时长 = 总帧数 × 200 ms。
   文件名是 `anan_<名字>.eaf`，连扩展名不能超过 31 个字符。
3. 重跑 `build`（见上）。会多出 `emoji/anan_idle_yawn.eaf`，`emote.json` 和
   `anan_idle_actions.inc` 各多一行。删掉的动作对应的旧 `anan_*.eaf` 会被自动清掉。
4. 跑测试（`test_build_faces.py` 里写死了现有 3 个动作，要一起改），提交生成物。

## 测试

```sh
python3 -m unittest discover -s scripts/anan_faces/tests -v
```

可选测试（目录不存在就跳过）：

- 官方素材包 `/tmp/emote/esp_emote_assets/emoji_large`（或环境变量 `ANAN_STOCK_EAF_DIR`）：
  官方每一帧重编码后逐字节一致，尾帧结构一致。
- 环境变量 `ANAN_FACES_ORIGINALS=<原图目录>`：边框检查对所有原图通过，不裁边时全部报错。
