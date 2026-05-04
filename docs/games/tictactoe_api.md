# TicTacToe — AI API 接入文档

## 概览

- **game_id**：`tictactoe`
- **玩家数**：固定 2
- **动作空间**：9
- **隐藏信息**：无（完全可观察）
- **公开事件**：无（`pre_events` / `post_events` 总是 `[]`）

---

## 动作编码

每个动作 ID 对应棋盘上的一个格子。棋盘是 3×3，按**行优先**编号：

```
 0 | 1 | 2
---+---+---
 3 | 4 | 5
---+---+---
 6 | 7 | 8
```

从 `action_id` 反推行列：

```python
row = action_id // 3
col = action_id % 3
```

玩家 0 放 `X`，玩家 1 放 `O`。先连成一条线（横/竖/斜 3 个相同）的获胜。

---

## 多人变体

无。TicTacToe 只有 2 人版。

---

## state_dict（`GET /ai/sessions/{id}` 不直接返回 state，但 session 内部维护的 state 字段如下）

所有字段都是公开的：

| 字段 | 说明 |
|------|------|
| `board` | 长度 9 的 int 数组。元素值：`-1` 空、`0` X、`1` O |
| `current_player` | 0 或 1 |
| `is_terminal` | bool |
| `winner` | 0 / 1 = 玩家赢，-1 = 平局或未结束 |
| `scores` | `[scores_p0, scores_p1]`，胜者 +1 败者 -1 |
| `move_count` | 已下棋子数 |

---

## 完整示例（Python）

```python
import requests

BASE = "http://localhost:8000"
sess = requests.post(f"{BASE}/ai/sessions", json={
    "game_id": "tictactoe", "seed": 42, "my_seat": 0,
    "simulations": 200, "temperature": 0.0,
}).json()
sid = sess["session_id"]

def apply(ground_truth_board, action_id, acting_player):
    """第三方的 ground truth: 简单起见，这里直接模拟在 Python 里跑。"""
    r = action_id // 3
    c = action_id % 3
    ground_truth_board[r][c] = acting_player

board = [[-1]*3 for _ in range(3)]
current_player = 0

while True:
    # AI 回合（我们设 my_seat=0）
    if current_player == 0:
        r = requests.post(f"{BASE}/ai/sessions/{sid}/decide").json()
        action_id = r["action_id"]
        print(f"AI plays cell {action_id} ({r['action_info']})")
    else:
        # 真实对手逻辑（这里假设人工输入）
        action_id = int(input(f"Your move (0-8): "))

    apply(board, action_id, current_player)

    # 回喂 AI
    r = requests.post(f"{BASE}/ai/sessions/{sid}/observe", json={
        "action_id": action_id,
        "pre_events": [],
        "post_events": [],
    }).json()

    if r["is_terminal"]:
        status = requests.get(f"{BASE}/ai/sessions/{sid}").json()
        print(f"Game over. Winner: player {status.get('winner')}")
        break
    current_player = r["current_player"]

requests.delete(f"{BASE}/ai/sessions/{sid}")
```

---

## 常见踩坑

- **action_id 不在 legal set 里会被拒**：TicTacToe 的合法动作集 = 所有空格子。占用过的格子不能再下
- **`observe` 必须按实际发生的顺序喂**：包括 AI 自己的动作。跳过或重复喂会让 AI 的内部 state 和 ground truth 不同步
