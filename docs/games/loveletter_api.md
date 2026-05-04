# Love Letter — AI API 接入文档

## 概览

- **game_id**：`loveletter` / `loveletter_2p` / `loveletter_3p` / `loveletter_4p`
- **玩家数**：2–4
- **动作空间**：47
- **隐藏信息**：有（每人手牌 + 轮到的人抽牌 + 牌堆顺序）
- **公开事件**：
  - `hand_override`（**pre**）：动作读隐藏手牌前先揭示
  - `drawn_override`（post）：下家抽牌后揭示他抽到的牌（仅下家能看到，对其他玩家是隐藏）
  - `discard`（隐式，通过 action 本身）

---

## 动作编码

Love Letter 的动作覆盖所有"打出某张牌 + 目标（若该牌需要目标）+ 猜测（Guard 专用）"的组合：

| 范围 | 类型 | 说明 |
|------|------|------|
| 0–27 | Guard 猜 | `target × 7 + (guess - 2)`。target 0..3，guess 值 2..8（7 种猜测；不能猜 Guard=1 因为规则不允许） |
| 28–31 | Priest | target 0..3（偷看该玩家手牌） |
| 32–35 | Baron | target 0..3（和该玩家比牌） |
| 36 | Handmaid | 自己保护一轮 |
| 37–40 | Prince | target 0..3（让该玩家弃手牌并重抽，可选自己） |
| 41–44 | King | target 0..3（交换手牌） |
| 45 | Countess | 自己打出 Countess（无效果，但规则强制与 King/Prince 同手时必须打） |
| 46 | Princess | 自己打出 Princess（立刻自我淘汰） |

**target** 用的是**相对座位**：target=0 表示自己下一位玩家，target=1 是下下位……映射到真实玩家：`target_player = (current_player + 1 + target) % num_players`。2p 游戏里只有 target=0 合法；3p/4p 多选。

**Guard 猜牌的 `guess` 值**：
| 值 | 牌 |
|----|----|
| 2 | Priest |
| 3 | Baron |
| 4 | Handmaid |
| 5 | Prince |
| 6 | King |
| 7 | Countess |
| 8 | Princess |

---

## 公开事件

### `hand_override`（**pre**）

动作要读取**对手或自己**的某个手牌时，先把该牌的真实值 reveal 给 AI。这样 AI 侧可以**不再需要猜**这个值——直接用正确的牌计算结果。

```json
{
  "kind": "hand_override",
  "payload": {"player": 1, "card": 6}
}
```

**何时发**（常见场景）：
- Guard 动作：actor 猜某玩家的手牌。如果猜对，该玩家被淘汰、手牌公开；猜错什么也不发生。但无论对错，**动作执行时需要读被猜玩家的手牌**——所以 ground truth 必须发一个 `hand_override` 告诉 AI 真牌是什么。
- Priest：actor 偷看 target 的手牌（仅 actor 看到）。仅 `my_seat == actor` 的 session 需要这个事件。
- Baron：actor 和 target 的手牌被比较。比较双方都会公开，两个 `hand_override` 都要发（actor 侧和 target 侧）。
- King：actor 和 target 交换手牌。两张都暴露给双方，发 2 个 `hand_override`。
- Prince：actor 让 target 弃手牌——该手牌公开，发一个 `hand_override` 揭示 target 的原手牌。

### `drawn_override`（post）

下家轮到时会抽一张牌（必须发，否则 AI 不知道下家有什么可用选择）。

```json
{
  "kind": "drawn_override",
  "payload": {"player": 2, "card": 5}
}
```

**对 perspective 是自己**：是真实的抽牌值。
**对 perspective 不是自己**：这个事件**不发**给非本人（他们看不到对手抽到什么），AI 会在 `randomize_unseen` 里采样。

---

## state_dict 字段

公开：`alive`（每玩家 bool）、`protected_flags`（Handmaid 状态）、`discard_piles`（每玩家的弃牌堆）、`hand_exposed`（Countess 被宣告等公开信息）、`deck_size`、`face_up_removed`（2p 开局移走的 3 张）、`ply`、`current_player`、`winner`

玩家私有：`hand[my_seat]`、`drawn_card`（若 `current_player == my_seat` 且已抽牌）。AI 另外维护 `known_hand[p]`：通过 Priest / Baron / King / Prince 事件合法知道的对手手牌。

---

## 完整示例（Python）

```python
import requests
BASE = "http://localhost:8000"

# AI 扮演 3 人局的第 1 号玩家
sess = requests.post(f"{BASE}/ai/sessions", json={
    "game_id": "loveletter_3p", "seed": 99, "my_seat": 1,
    "simulations": 800, "temperature": 0.0,
}).json()
sid = sess["session_id"]

# Ground truth 端要实现：每个动作算出 pre/post events
def compute_events(state_before, action_id, state_after, perspective):
    """对照 games/loveletter/loveletter_register.cpp::extract_events 的语义，
    返回 {"pre_events": [...], "post_events": [...]}。"""
    ...

while True:
    status = requests.get(f"{BASE}/ai/sessions/{sid}").json()
    if status["is_terminal"]: break

    if status["current_player"] == 1:
        r = requests.post(f"{BASE}/ai/sessions/{sid}/decide").json()
        action_id = r["action_id"]
    else:
        action_id = opp_pick()

    pre, post = compute_events(state_before, action_id, state_after, perspective=1)
    requests.post(f"{BASE}/ai/sessions/{sid}/observe", json={
        "action_id": action_id, "pre_events": pre, "post_events": post,
    })

requests.delete(f"{BASE}/ai/sessions/{sid}")
```

---

## 常见踩坑

- **`hand_override` 必须是 pre_event**：Guard/Baron/King/Prince 的效果依赖对手手牌。如果放在 post_events，AI 已经用错的占位手牌算过结果
- **Priest 仅对 actor 发事件**：其他玩家的 session 不应收到这个 `hand_override`——那是信息泄漏
- **淘汰时手牌自然公开**：被 Guard 猜中、Baron 比输、Prince 弃的 Princess 等情况，被淘汰的玩家手牌进入 discard pile，AI 通过 discard_piles state 自然知道
- **多人座位轮转**：`target` 是**相对座位**。3/4 人局里同一 action_id 对不同的 actor 指的 target 玩家不同
