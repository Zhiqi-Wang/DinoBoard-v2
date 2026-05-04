# Azul — AI API 接入文档

## 概览

- **game_id**：`azul` / `azul_2p` / `azul_3p` / `azul_4p`
- **玩家数**：2–4
- **动作空间**：`(factories + 1) × 5 × 6`，2p/3p/4p 下的工厂数不同
  - 2p: 5 factories → 180
  - 3p: 7 factories → 240
  - 4p: 9 factories → 300
- **隐藏信息**：袋中瓷砖顺序是**对称随机**——所有玩家都不知道下一批翻到工厂上是什么颜色，但知道袋里各色的总数
- **公开事件**：`factory_refill`（post）——round 边界翻新工厂时发

---

## 动作编码

每个动作是一个三元组 `(source, color, target)`：

```python
factories_count = 5 if num_players == 2 else 7 if num_players == 3 else 9
COLORS = 5           # 蓝/黄/红/黑/白（索引 0..4）
TARGETS = 6          # 5 条 pattern line + 1 floor (索引 5)

source = action_id // (COLORS * TARGETS)     # 0..factories_count-1 = 工厂号；factories_count = center
color = (action_id // TARGETS) % COLORS       # 0..4
target = action_id % TARGETS                  # 0..4 = pattern line 0..4；5 = floor
```

含义：「从 `source`（某个工厂或 center）拿所有 `color` 色的瓷砖，放到 `target`（自己的 pattern line 或 floor）」。

规则简介：
- 每回合玩家选一个工厂（或 center）取某色全部瓷砖。工厂剩余的瓷砖推到 center。
- 拿到的瓷砖放到自己对应长度的 pattern line 上（line 0 长度 1，line 4 长度 5）。
- Pattern line 满员后在 wall 上成墙得分。
- 瓷砖溢出（已满 line、或颜色已在 wall 上）或 floor 都扣分。
- Round 结束（所有工厂 + center 清空）后下家成为先手，袋中重洗补给工厂。

---

## 公开事件

### `factory_refill`（post）

当一个 round 结束，游戏从袋子和弃盘补给工厂时发一次。

```json
{
  "kind": "factory_refill",
  "payload": {
    "factories": [[...], [...], [...], [...], [...]],
    "bag_remaining_counts": [8, 5, 10, 2, 4]
  }
}
```

- `factories`：长度 = factories_count 的二维数组，`factories[i][c]` = 工厂 i 上色 c 的瓷砖数
- `bag_remaining_counts`：长度 5（每色），袋里剩余的对称总数（公开）

**何时发**：当 `round_index` 增加时，即新一 round 刚开始。普通动作（取瓷砖）之间**不发**。

---

## state_dict 字段（全部公开，Azul 无玩家私有字段）

| 字段 | 说明 |
|------|------|
| `factories` | `[num_factories][5]`，每色瓷砖数 |
| `center` | 长度 5，center 上每色瓷砖数 |
| `first_player_token_in_center` | bool。某玩家从 center 拿瓷砖时拿走此 token，下 round 先手 |
| `players[i].wall_mask` | 长度 5 的整数（bitmask），wall 上各行已放的色 |
| `players[i].line_len` / `line_color` | pattern line 当前长度和颜色 |
| `players[i].floor` / `floor_count` | floor 瓷砖数组和数量 |
| `players[i].score` | 当前分数 |
| `round_index` | 当前 round |
| `current_player` / `is_terminal` / `winner` |  |
| `bag_size` | 袋中总瓷砖数（公开） |

---

## 完整示例（Python）

```python
import requests
BASE = "http://localhost:8000"

sess = requests.post(f"{BASE}/ai/sessions", json={
    "game_id": "azul_3p", "seed": 42, "my_seat": 2,
    "simulations": 1200, "temperature": 0.0,
}).json()
sid = sess["session_id"]

while True:
    status = requests.get(f"{BASE}/ai/sessions/{sid}").json()
    if status["is_terminal"]: break

    if status["current_player"] == 2:
        r = requests.post(f"{BASE}/ai/sessions/{sid}/decide").json()
        action_id = r["action_id"]
        info = r["action_info"]
        print(f"AI: 从{'工厂'+str(info['source']) if info['source']<7 else 'center'}拿色{info['color']}到{'line'+str(info['target']) if info['target']<5 else 'floor'}")
    else:
        action_id = opp_pick()

    pre, post = ground_truth_apply(action_id)  # ground truth 计算 factory_refill 若有
    requests.post(f"{BASE}/ai/sessions/{sid}/observe", json={
        "action_id": action_id, "pre_events": pre, "post_events": post,
    })

requests.delete(f"{BASE}/ai/sessions/{sid}")
```

---

## 常见踩坑

- **忘发 `factory_refill`**：round 结束那一步的 `observe` 之后，下一 round 开始前必须发。如果漏发，AI 会用"上一 round 剩下什么 × 袋中采样"算，和 ground truth 不一致
- **不需要单独 `take_tiles` 事件**：动作本身（action_id）已经表达了"从哪个 source 拿什么色"，AI 内部的 rules 会自己维护 factories/center 的更新
- **AI 的袋子顺序可能和 ground truth 不同**：Azul 的"袋中各色总数"是公开的（写在 state_dict），但具体翻到工厂的是哪几张是随机的。每次 `factory_refill` 事件覆盖 AI 自己的采样结果就行
