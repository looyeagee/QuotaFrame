# 歌词滚动算法

本文说明 RLCD 4.2 面板如何在放不下时显示一整句歌词，以及每个参数为什么取这个值。实现见 `firmware/targets/waveshare_rlcd_42/main` 的 `screens.cpp`（`lyric_row`）、`panel_logic.cpp`（`lyric_scroll_offset`、`apply_now_playing`）和 `cjk_font.cpp`（`boundary_at_or_before`、`window_width`、`tail_start`）。

## 约束

- 歌词页一行 364 px、时钟页右栏一行 216 px，都是 12 px 字号（汉字 24 px 一列，ASCII 12 px 一列）。时钟页那一栏只有歌词页的 59 % 宽。
- 一句中文歌词常有 20–25 个字（60–75 字节），一行最多显示 15 个左右，其余必须滚动才能看到。
- 面板 4 fps 组帧，每帧整帧写入；帧内容不变时不发 SPI。
- 换句只在收到新发布时发生，最多比实际换句晚一个发布周期（1 s）。

## 数据来源

Bridge 每次发布 `now_playing` 都给出这一行及其后四行、当前行下标，以及可选的 `line_end`（这一行结束、下一行开始的位置，秒）。面板把 `line_end` 只当作**上限**用，见下。

## 算法

对正在唱的那一行（`row == 0`），已知整行宽度 `width` 与这一行可用宽度 `room`：

```
travel   = tail_start(line, room)          # 整条尾巴能放进这一行的最左字形边界
text_col = ceil(width / 12)                # 整行有多少列文字
content  = 900 ms + 100 ms × text_col      # 读这行需要多久

budget   = (line_end − position) × 1000    # 首次收到这行时算一次；缺失为 0
lead     = 900 ms                          # 行首静置
motion   = content
if budget > 0:
    if budget > lead + 600 ms:   motion = min(content, budget − lead)
    else:                        lead = 0; motion = min(content, max(budget, 600 ms))

step(t)  = min(ceil(travel/12), (t − lead) × ceil(travel/12) / motion)   # t < lead 时为 0
start    = boundary_at_or_before(line, min(step × 12, travel))
visible  = window_width(line, start, room)     # 从 start 起能完整放下的整字
绘制      = clip(x, top, visible, 行高) 内，把整行从 x − start 开始画
```

要点：

1. **节奏由文本决定，不由行程决定。** `content` 只看整行有多少列，与这一行要滚多远无关。于是同一批词在窄栏里滚得更快而不是更久——时钟页那条正是旧算法的失效点：按行程定时长时，窄栏把行程放大 1.87 倍，时长也放大 1.87 倍，一句 8 秒的歌词被排成 12.2 秒，第 8 秒只走了 61 %。
2. **`line_end` 只能压短，不能拉长。** 句子短时（`budget` 比 `content` 小）滚动被压进本句时间，末字在换句前到达；句子后面跟着长间奏时（`budget` 很大）用 `content`，滚完就停在终态，不会像旧方案那样把速度摊薄到整段间奏。
3. **整尾对齐。** `travel` 取 `tail_start`：从行尾往回累加整字宽度，找到「整条尾巴能放进一行」的最左字形边界。直接按 `width − room` 算终位时，量化可能比这个边界多走 12 px，最后一个字会被整字裁掉——中文行约有一半会命中，正好是"看不到最后 1–2 个字"。
4. **整字裁剪。** `boundary_at_or_before` / `window_width` 保证裁剪窗左右两端都落在字形边界上，滚动过程中不会出现半个字。纯像素裁剪在 1-bit 屏上会把半个字显示成像「口」的方框。
5. **每句只算一次 `budget`。** 锚点是"这一行的文字变了"的那一刻（`apply_now_playing` 比较当前行文本），句内重复发布不重锚、不重算上限，位置抖动不会改动速度。
6. **换句从头开始。** 新句从行首、静置 900 ms 起播；暂停时停在行首，不滚走。
7. **只滚正在唱的那行。** 其余行是上下文，用 `fit` 截断加省略号。

## 参数

| 参数 | 值 | 含义 |
| --- | --- | --- |
| `kLyricScrollColumn` | 12 px | 一列文字；偏移与裁剪都按它量化 |
| `kLyricLeadInMs` | 900 ms | 行首静置，先看清开头 |
| `kLyricBaseMs` | 900 ms | 每行的固定阅读开销 |
| `kLyricColumnMs` | 100 ms | 每列文字的阅读时间（汉字约 0.2 s） |
| `kLyricFloorMs` | 600 ms | 剩余时间极短时的最短运动时长 |
| 行缓冲 | 192 字节 | 协议上限；一句最多 64 个汉字 |

一句话 45 列时：内容时长 5.4 s，加前导共 6.3 s。时钟页 336 px 行程 → 62 px/s，歌词页 180 px → 33 px/s。

## 边界情况

| 情况 | 行为 |
| --- | --- |
| 整行放得下（`travel ≤ 0`） | 不滚，静态显示 |
| 没有 `line_end`（最后一句、旧版 Bridge） | 只用 `content`，滚完停在终态 |
| `line_end ≤ position` | 视为没有上限 |
| 剩余时间不足 1.5 s | 取消静置，用剩余时间（至少 600 ms）滚完 |
| 一句几十个字（192 字节上限） | `content` 变大但有 `budget` 压着，不会拖过本句 |
| 间奏 | `budget` 很大 → 取 `content`，滚完停住，间奏期间不动 |

## 验证

- 单元测试：`firmware/targets/waveshare_rlcd_42/test_apps/logic/main/test_panel_logic.cpp`（曲线数值、整列对齐、上限只压短不拉长、锚点只在换句时复位）与同目录 `test_screens.cpp`（整尾对齐、只有当前行滚、裁剪不越界）。
- 渲染验证：`tools/rlcd42/render_preview.cpp` 生成全部页面；另用临时驱动渲染同一句在 0 / 2 / 4 / 6.3 / 8 / 30 秒的帧，确认 6.3 秒即终态、30 秒与终态逐字节相同。
- 行为回归：34 页预览与改动前逐字节一致（短句行为未变）。
