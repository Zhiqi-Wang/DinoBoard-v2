# Coup — AI API 接入文档

## 概览

- **game_id**：`coup` / `coup_2p` / `coup_3p` / `coup_4p`
- **玩家数**：2–4
- **动作空间**：32
- **隐藏信息**：有（每人 2 张 influence 卡 + deck 剩余卡序列）
- **公开事件**：
  - `card_revealed`（post）：某玩家揭示一张牌（被挑战失败、lose influence 或成功 reveal 自证）
  - `exchange_complete`（post）：Ambassador 换牌流程结束
- **特点**：AI 用**启发式概率 sampler**（而非均匀采样）来采样对手暗牌——基于 claim/challenge 历史推断。接入方不需要操心这一层，只要正确发事件即可

---

## 动作编码

固定 32 个动作 ID：

| 范围 | 类型 | 说明 |
|------|------|------|
| 0 | Income | +1 币 |
| 1 | Foreign Aid | +2 币（可被 Duke 挡） |
| 2–5 | Coup target 0..3 | 花 7 币强制 target 丢一张 influence |
| 6 | Tax | +3 币（claim Duke，可被挑战） |
| 7–10 | Assassinate target 0..3 | 花 3 币让 target 丢 influence（claim Assassin，可被挑战或 Contessa 挡） |
| 11–14 | Steal target 0..3 | 从 target 偷 2 币（claim Captain，可被挑战或 Captain/Ambassador 挡） |
| 15 | Exchange | claim Ambassador，从 deck 抽 2 张换手牌 |
| 16 | Challenge | 质疑当前 claim |
| 17 | Allow | 不挑战（普通动作） |
| 18 | Block Duke | claim Duke 挡 Foreign Aid |
| 19 | Block Contessa | claim Contessa 挡 Assassinate |
| 20 | Block Ambassador | claim Ambassador 挡 Steal |
| 21 | Block Captain | claim Captain 挡 Steal |
| 22 | Allow No Block | （不挡 Foreign Aid，默认流程） |
| 23 | Reveal Slot 0 | 被挑战后揭示 slot 0 影响 |
| 24 | Reveal Slot 1 | 被挑战后揭示 slot 1 |
| 25 | Lose Slot 0 | 被 coup / 挑战失败时选择丢 slot 0 |
| 26 | Lose Slot 1 | 同上，slot 1 |
| 27–31 | Return 角色 | Ambassador exchange 最后把某角色牌还回 deck。27=Duke, 28=Assassin, 29=Captain, 30=Ambassador, 31=Contessa |

**多阶段回合**：Coup 的一个"回合"可能产生多个 action_id（出招 → 被挑战 → 揭示 → 丢牌 等）。AI 每收到一个 action_id 就更新一步内部 state，直到 `current_player` 走到下一真正的决策者。

---

## 公开事件

### `card_revealed`（post）

任何角色被**公开**揭示时发——包括：
- 挑战失败方揭示一张非 claim 的牌（lose influence）
- 挑战成功方揭示 claim 对应的角色（证实 claim，然后该牌洗回 deck + 新抽）
- Coup / Assassinate 导致 target lose influence

```json
{
  "kind": "card_revealed",
  "payload": {"player": 1, "role": 2}
}
```

- `role` 值：0=Duke, 1=Assassin, 2=Captain, 3=Ambassador, 4=Contessa

**何时发**：无论是 reveal 成功（自证）还是失败（丢牌），只要这张牌被当众揭示过，都要发。AI 侧需要这个信号来更新 tracker 的 signal count（"玩家 p 的某角色已经亮过了")。

### `exchange_complete`（post）

Ambassador 的 exchange 流程（claim → 若不被挑战 → 抽 2 张 → 从手里选保留 → 把其余的还回 deck）全部结束后发一次。AI 侧据此**清空该玩家的所有 claim 信号**（因为他的手牌已经被部分重洗了）。

```json
{
  "kind": "exchange_complete",
  "payload": {"player": 0}
}
```

---

## state_dict 字段

公开：`alive`（每玩家）、`coins`（每玩家）、`influences[i]`（长度 2 的数组，其中 `revealed=true` 的 `character` 可见）、`deck_size`、`stage`、`declared_action`（当前被挑战的 action）、`active_player`（出招方）、`action_target`、`blocker`、`challenger`、`ply`

私有：`influences[my_seat]` 里 `revealed=false` 的 `character`（自己的暗牌）。对手的暗牌不可见，AI 靠启发式 sampler 采样。

---

## 完整示例（Python）

```python
import requests
BASE = "http://localhost:8000"

sess = requests.post(f"{BASE}/ai/sessions", json={
    "game_id": "coup_4p", "seed": 555, "my_seat": 3,
    "simulations": 1000, "temperature": 0.0,
}).json()
sid = sess["session_id"]

# AI 需要所有玩家的 claim / challenge / reveal 事件才能正确建模
while True:
    status = requests.get(f"{BASE}/ai/sessions/{sid}").json()
    if status["is_terminal"]: break

    if status["current_player"] == 3:
        r = requests.post(f"{BASE}/ai/sessions/{sid}/decide").json()
        action_id = r["action_id"]
    else:
        action_id = opp_pick()

    # Ground truth 端必须：
    # 1. 应用 action 到自己的 state
    # 2. 检测是否有 card_revealed / exchange_complete 事件
    pre, post = compute_events(action_id)
    requests.post(f"{BASE}/ai/sessions/{sid}/observe", json={
        "action_id": action_id, "pre_events": pre, "post_events": post,
    })

requests.delete(f"{BASE}/ai/sessions/{sid}")
```

---

## Coup 特有：启发式 belief

Coup 的 tracker 不是 uniform 采样。它在内部维护每个对手对每个角色的**信号计数**：

- 对手**claim 某角色且未被挑战** → +1 信号
- 对手**被挑战成功/失败** → 相关信号清零（牌已经暴露/洗回 deck，信号失效）
- 对手**挑战别人的某 claim** → 该对手对该角色 +1 信号（他敢挑战，说明他自己可能持有）
- 对手**揭示某角色** → 清空对那角色的信号
- 对手**完成 Ambassador exchange** → 清空所有信号

采样时权重 = `pool_remaining × (1 + 0.5 × signal_count)`，加硬约束保证全局守恒（每角色总共 ≤ 3 张）。

**接入方什么都不用做**，只要正确发 `card_revealed` / `exchange_complete` 事件，AI 的启发式自己就工作。

详见 `docs/GAME_DEVELOPMENT_GUIDE.md` §11.4 的 Coup 子节。

---

## 常见踩坑

- **多阶段回合要逐 action 喂**：一次出招 → 挑战 → reveal → 丢牌 是 4 个连续的 action。不能只喂第一个然后想靠事件补齐后面——每个 action 都必须 observe 一次
- **挑战成功时的 reveal 要发 `card_revealed`**：即使那张 card 没进 discard pile（被洗回 deck 了），也还是被公开过，信号需要更新
- **`exchange_complete` 要独立事件**：不能只靠 action ID 推断——因为 exchange 是几个 action 组成的复合流程
