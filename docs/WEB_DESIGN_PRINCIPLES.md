# Web 前端设计原则

给游戏前端开发者的视觉与交互指引。技术实现细节（createApp API、ctx 对象、common.js 工具）见 [GAME_DEVELOPMENT_GUIDE.md §13](GAME_DEVELOPMENT_GUIDE.md#13-web-前端开发)。

---

## 1. 自然交互：不要一个动作一个按钮

动作空间可能有几十到上百个（Splendor 70 个、Quoridor 130+ 个），逐一列出按钮既丑陋又无法操作。**按物理游戏的交互方式设计 UI**：

| 物理操作 | 对应 UI 交互 |
|---------|-------------|
| 拿起棋子→放到目标格 | 点击棋子 → 高亮可达位置 → 点击目标 |
| 从公共区拿一个东西 | 点击公共区元素 |
| 抽一张暗牌 | 点击牌堆 |
| 组合操作（拿多颗宝石） | 逐步点击选择，最后确认 |

游戏 JS 内部将手势翻译为 `actionId`，通过 `ctx.submitAction(actionId)` 提交。**玩家永远不应看到或思考 action 编码数字**。

---

## 2. 空间锚定：固定区域不动，只有内容变化

游戏中固定存在的容器（棋盘格、工厂盘、中心区、玩家面板、牌库位置、银行区）必须有**固定的屏幕位置和尺寸**，不随内容数量变化而移动、缩放或重排。只有容器内部的元素（棋子、牌、token）可以出现、消失、移动。

**为什么**：玩家靠空间记忆快速定位信息——"左下角是我的手牌，右上角是公共牌库"。如果容器位置随内容增减而漂移（比如 Azul 中心区 token 被拿走后区域收缩导致旁边的工厂盘位移），玩家每步都要重新扫描整个画面，严重影响可玩性。物理桌游天然满足这个约束（棋盘不会自己挪位置）。

**实践要点**：

- 用固定尺寸容器（`width`/`height` 写死或 `min-width`/`min-height`），不用 `fit-content`
- 元素减少时容器留白，不收缩；元素增加时内部滚动或缩放，容器不撑大
- 避免对容器级元素使用 `flexbox` 的 `gap` + 自动换行——内容变化会改变行数，推动后续容器位移
- **可变数量的同类元素用固定插槽 + 计数徽章**，而不是动态增减 DOM 元素。例：Azul 中心区固定 6 个插槽（先手标记 + 5 种颜色），每个插槽右上角显示计数；砖被拿走后插槽变为半透明占位，容器尺寸不变。这比逐个渲染砖块要稳定得多。
- **固定布局要考虑屏幕空间和视觉平衡**。"固定"不是"一字排开"——要根据元素数量和容器可用空间选择合适的网格列数。例：6 个插槽用 2 列×3 行比 6 列×1 行紧凑；5-9 个工厂用 3 列自动换行比单列竖排更符合视觉习惯。响应式断点下适当减少列数（如小屏 2 列）。

---

## 3. 动作动画：状态切换要有过渡

每个动作（人类或 AI）都应有动画过渡，让玩家看清发生了什么。**不允许瞬间跳变到新局面**。

### 框架支持

在 `createApp(config)` 中提供可选的 `describeTransition` 函数：

```js
describeTransition(prevState, newState, actionInfo, actionId) → [AnimStep] | null
```

- 返回动画步骤数组，框架按顺序播放后再刷新到新状态
- 返回 `null` 或空数组 → 直接切换（向后兼容）
- 函数抛异常 → 自动降级为直接切换，不影响游戏功能

### 动画步骤类型

| type | 参数 | 说明 |
|------|------|------|
| `fly` | `from`, `to`, `createElement`, `width`/`height`, `onStart`, `hideFrom`, `onComplete`, `duration` | 创建飞行元素从 A 飞到 B |
| `flyGroup` | `flights[]` | 一组 fly 并行播放（Promise.all） |
| `group` | `children[]` | 一组任意 step 并行播放 |
| `popup` | `target`, `content`, `className`, `duration` | 数字/文字气泡浮现再淡出（得分、扣分等） |
| `run` | `fn` | 运行 DOM mutation 回调（阶段间改 DOM） |
| `fadeOut` | `target`, `duration` | 淡出一个 DOM 元素 |
| `highlight` | `target`, `className`, `duration` | 短暂高亮一个元素 |
| `pause` | `duration` | 等待一段时间 |
| `reveal` | `title`, `body`, `buttonText`, `className` | 阻塞式揭示弹窗，必须由玩家点击确认才继续（见 §3.7） |

### 中间状态维护

一个动作可能产生多步动画（如购买卡牌 = 宝石飞回 + 卡牌飞走）。每步动画结束后，上一步的 DOM 变化需要保持可见，否则视觉上会乱套。框架提供三个机制：

- **`onStart(srcEl)` 回调**：fly 开始时把源 DOM 变成"取走后"的样子（参考 §3.5 的坑点），后续看到的是新状态
- **`hideFrom: true`**：fly 结束后源元素设为 `visibility: hidden`（保持布局占位），后续步骤看到"东西已经不在了"
- **`onComplete` 回调**：fly 结束后更新 DOM（比如修改计数器文字），后续步骤看到的数字是对的

所有动画播完后框架自动恢复隐藏元素，然后重渲染到新状态。

### 3.5 重要陷阱：不要用 hideFrom 隐藏"带空态背景"的容器

**踩过的坑**：Azul 的中心池格子有两个逻辑状态——有砖时显示彩色砖块、没砖时显示半透明的"空位幽灵砖"表示"这里本来可以放这种颜色的砖"。两者渲染到同一个 `.center-slot` 容器里。

如果飞砖动画用 `hideFrom: true` 隐藏 `.center-slot`，`visibility: hidden` 会把整个容器(包括空态背景)一起藏起来,画面上用户看到的是"格子消失了一个洞",而不是"砖被取走了,空位还在"。

**正确做法**：

- **容器有明确空态背景时,用 `onStart` 回调而不是 `hideFrom`**。`onStart(srcEl)` 会在 fly 开始时把源 DOM 改成"取走后"的样子(比如清空 innerHTML 再加一个幽灵 tile),sprite 再从源位置飞走。视觉上容器位置不动、背景还在、只有实体砖飞走了。
- **容器没有空态背景(例如工厂 tile 格子堆)用 `hideFrom` 没问题**。因为取走后容器确实应该整体消失/变空,`visibility: hidden` 的效果和最终 re-render 后的效果一致。

示例(Azul 中心槽):
```js
flights.push({
  from: '[data-center-slot="' + color + '"]',
  to: patternRowSelector,
  createElement: () => makeFlyingTile(color),
  onStart: (slotEl) => {
    // 变成空态:清内容,加 .empty class,加幽灵 tile
    slotEl.innerHTML = '';
    slotEl.classList.add('empty');
    const ghost = document.createElement('div');
    ghost.className = 'tile ' + TILE_CLASSES[color];
    slotEl.appendChild(ghost);
  },
  // 不用 hideFrom
});
```

**判断流程**：
1. 源 DOM 在"东西被取走"后应该还显示某种背景(空格、虚线轮廓、幽灵)吗？
   - 是 → `onStart` 改成背景态
   - 否 → `hideFrom: true` 够了
2. 源 DOM 被取走后应该完全消失(无布局占位)吗？那用 fadeOut 或让 re-render 收掉,不要用 hideFrom。

### 3.6 信息型动作：没有实体位移时用"头顶气泡"

不是所有动作都有可以 fly 的实体。Love Letter 的 Priest 是"偷看对手一张牌",没有物件移动;Coup 的"征税/质疑/反制"是声明/决策,也没有物理位移。这类动作如果不做任何动画,AI 回合会"啪"一下跳过,玩家完全看不清发生了什么——尤其 AI 连续行动时,几秒钟之内走完三四个动作,对局变成一串谜语。

**正确做法**:在**行动玩家的头顶**弹一个气泡,里面写出动作描述(直接用 `formatActionInfo` / `formatMove` 的文本),悬浮 1.2–1.8 秒再淡出。人类和 AI 动作一视同仁——人类自己的动作也需要,这样复盘时能看清楚自己刚才做了什么。

**实现**:用 framework 自带的 `popup` step type:

```js
steps.push({
  type: 'popup',
  target: `[data-player="${actor}"]`,  // 或 [data-opponent], 只要指向行动玩家区域
  content: formatMove(actionInfo, actionId),
  className: 'action-bubble',
  width: 200, height: 36,
  duration: 1500,
});
```

**最低要求**(所有游戏 Checklist):

- 每个 `describeTransition` 返回的动画链里,**只要没有对行动玩家区域的 fly/highlight**,就必须加一个 popup 交代动作。宁可 popup + fly 都有(信息冗余无害),也不要两者都没有。
- popup 宽度要能装下一行中文动作描述(推荐 180–220px),高度 32–40px。
- popup 持续 `1200–1800ms`,短于这个范围玩家来不及读。和 fly 并行发生时可以用 `group` step 让 popup 在 fly 过程中持续显示,flight 播完 popup 还在。
- popup 的目标选择器必须能解析到**当前行动玩家的容器**。游戏渲染函数里必须给每个玩家区域(包括人类自己的 player-area)加 `data-player="<idx>"` 或 `data-opponent="<idx>"`,否则 popup 会定位失败被跳过。

**为什么要人类自己的动作也弹气泡**:看起来"自己点的按钮自己知道做了什么"似乎是多余的,但实际上:① 多人游戏里人类也在某些回合充当被动方,自己点完"允许"按钮之后也需要视觉反馈;② 如果只给 AI 弹、给人类不弹,节奏就会割裂——有时有气泡有时没有,玩家感知上不连贯;③ 复盘录像时需要看清每一步是谁、做了什么。

### 3.7 私密信息揭示：阻塞式弹窗让玩家看清

部分动作会把**只对当前玩家可见的信息**亮出来——Love Letter 牧师偷看对手手牌、男爵比较手牌、Coup 质疑失败时亮出的角色牌等等。这种信息如果只用头顶气泡(§3.6)显示，一闪而过就没了；更糟的是 AI 连续行动时几张关键牌一起呼啸而过，玩家根本不知道刚刚发生了什么，局后也无从复盘。

**正确做法**：用 framework 自带的 `reveal` step type，弹出一个居中的阻塞式弹窗，里面展示揭示出来的牌面 / 结果，要求玩家点击 "知道了" 后动画链才继续。这样既强制玩家看清，又让后续 AI 行动排队到揭示被确认之后。

```js
steps.push({
  type: 'reveal',
  title: '牧师 · 偷看手牌',
  body: /* HTMLElement or HTML string */ revealBody([
    { label: '玩家1 的手牌', card: 5 },
  ]),
  // buttonText: '知道了',   // 可选，默认就是 "知道了"
  // timeoutMs: 8000,         // 可选。一般不要自动关——"强制玩家看清" 就是这个 step 存在的目的
});
```

实现要点与约束：

- **只在"玩家本人"真的学到了新东西时触发**。Priest 弹给 actor（偷看者）；Baron 弹给 actor 和 target（对手被亮牌的那方，或自己牌被亮的那方）；如果都是 AI 之间的交互，不弹窗——玩家不需要看 AI 互相偷看。判断公式是：`humanIsActor || humanIsTarget`（视卡牌而定）。
- **背景点击不关窗**，只有明确的"知道了"按钮能关。错按背景就把关键信息跳过了太亏。按钮默认 focus，支持键盘 Enter 关闭。
- **弹窗用 `z-index: 10003`**（在 `.anim-overlay` 的 10001 / popup 的 10002 之上），动画 sprite 飞到终点后仍会被弹窗覆盖——这是刻意的，读牌比看尾迹重要。
- **一次性呈现所有相关信息**：Baron 比较要把双方的牌并排放出来 + 一句"你赢了 / 你被淘汰 / 平局"结论。不要分两次弹——玩家会等不及。
- **不要滥用**。一个 step 弹窗 = 一次强制打断。只在"不弹就真的看不到"的情境用；常规动作继续用头顶气泡 + 高亮。
- **录像回放里也会触发**。`reveal` 和其他 step 一样走 `describeTransition`，复盘时玩家仍然需要点一次"知道了"才能继续下一步——对复盘来说这是特性不是 bug。

### 与 AI 思考的配合

动画和 AI 后台计算是**并行的**：

```
玩家落子
  ├→ 立刻播玩家动画（~400ms）
  ├→ 同时后台：分析 + AI 思考
  动画播完 → 渲染新局面
  AI 返回 → 播 AI 动画 → 渲染 AI 落子后局面
```

动画不会增加等待时间。如果 AI 比动画先回来，动画播完后立刻播 AI 动画。

### data 属性约定

`describeTransition` 用 CSS 选择器定位 DOM 元素。渲染函数需要在关键元素上加 `data-*` 属性。推荐命名：

```
data-bank-gem="0"          银行宝石（按颜色索引）
data-tableau="2-1"         牌面卡（tier-slot）
data-player="0"            玩家区域
data-player-gem="0-3"      玩家宝石（player-color）
data-reserved="0-1"        保留卡（player-slot）
data-noble="2"             贵族卡（slot）
data-deck="1"              牌堆（tier）
```

命名没有强制要求，只要 `describeTransition` 和渲染函数使用一致的选择器即可。

### 速度建议

| 动画类型 | 推荐时长 |
|---------|---------|
| 宝石/token 移动 | 350-450ms |
| 卡牌移动 | 450-600ms |
| 步骤间间隔 | 60ms |
| 整体超时上限 | 5000ms（超时自动跳过剩余） |

**动画不要太快**（踩过的坑）。默认 `DEFAULT_DURATION = 350ms` 对 token 勉强够,对卡牌就偏快——牌一晃而过玩家来不及看清从哪里飞到哪里,尤其第一次玩时会完全错过"哦这张牌是从牌堆来的"这类因果信息。**实践规律**:

- token 类小元素在短距离移动(银行↔玩家区)`350-400ms` 合适;跨大距离(对角线)提到 `450ms`
- 卡牌是视觉焦点,应该比 token 慢 **50-150ms** 才能让玩家跟上:`CARD_FLY_MS = 550` 是 Splendor 实测舒适的数
- 出现"玩家反映看不清动画"的反馈,第一反应是把 duration 加 100-150ms,不要倾向于"节奏紧凑所以快"
- `flyGroup` 里的多个 flight 应该用相同或相近的 duration,否则一部分已经到位、另一部分还在飞,视觉上很割裂

---

## 4. 视觉可辨性

### 颜色对比度

- 不要用白色或浅灰色作为元素的唯一标识色——在浅色背景上几乎看不见
- 白色元素需要边框或阴影来标示边界
- 确保文字颜色和背景有足够对比度（深色背景用浅文字，反之亦然）

### 元素大小

- 可点击元素最小 36×36px（移动端 touch target）
- 关键信息（分数、手牌数、当前玩家指示）字号不小于 14px
- 宝石 / token 数字要清晰可读，不要被装饰遮挡

### 状态区分

- **当前玩家**必须有明显的视觉标记（边框加粗、背景高亮、标签文字）
- **可操作 vs 不可操作**的元素必须视觉可区分（可操作的有 hover 效果和 cursor:pointer）
- **选中状态**需要明确反馈（边框变色或背景变化）
- **禁用的按钮**要明显灰显，不要只是去掉 hover 效果

### 信息层次

- 最重要的信息（当前轮到谁、分数、局面关键数字）要第一眼能看到
- 次要信息（详细卡牌费用、历史记录）可以缩小或折叠
- 避免信息过载——不是所有状态都需要永久显示在屏幕上

---

## 5. 布局框架

通用层 (`general/`) 提供固定的页面框架，游戏前端只填充内容：

```
┌─────────────────────────────────────────────┐
│ 上方：公共游戏区（renderBoard）+ 信息栏     │
├─────────────────────────────────────────────┤
│ 下方：玩家区域（renderPlayerArea）           │
│   2 人：左右分列                             │
│   3-4 人：网格排列                           │
└─────────────────────────────────────────────┘
```

所有辅助功能（开局、悔棋、提示、录像、难度选择）由通用层处理，游戏代码不需要实现。

### 5.1 信息栏放侧栏，不放游戏区

游戏状态描述（当前阶段名、牌堆剩余、回合数等）**不要**单独放在游戏区上方做成一行信息栏。这种信息栏宽度会随文本长度变化（"声明行动" vs "失去影响力(反制)"），导致整个游戏区布局轻微跳动，看起来不稳。

**正确做法**：用通用层的 `extensions` 机制把这类信息做成侧栏 info-panel 的第 5 行（自定义 pill）。侧栏本身是固定宽度容器，pill 的宽度变化不会撬动任何主要游戏元素。

```js
const stageExtension = {
  render(el, gameState) {
    const st = gameState && gameState.state;
    if (!st) { el.textContent = '阶段：--'; return; }
    el.innerHTML = '<span class="coup-stage">阶段：' + STAGE_NAMES[st.stage] + '</span>'
      + '<span class="coup-deck">牌堆：' + st.deck_size + '</span>';
  },
};

createApp({
  // ...
  extensions: [stageExtension],
});
```

`extensions` 数组里每一项都会在侧栏 4 条固定信息 pill 之后按顺序追加一条自定义 pill。`render(el, gameState)` 在每次 re-render 被调用，直接往 `el` 写 innerHTML/textContent 即可。

**什么时候才在游戏区内放信息**：只有当信息跟某个空间位置绑定（例如某张牌旁边的"+5"分数气泡、棋盘角落的回合计数），才留在游戏区；即便如此也必须是**固定位置、固定尺寸**，不能横跨整行。一条跨整行的文字状态条几乎肯定应该搬到侧栏。

---

## Checklist

新游戏前端上线前自查：

- [ ] 没有裸露 action ID 按钮，交互方式自然
- [ ] 所有固定区域有固定位置和尺寸，内容变化不导致布局跳动
- [ ] 动作按钮栏尺寸和槽位**固定**——不可用动作应灰显禁用,不应消失/重排/变形导致容器跳动
- [ ] 实现了 `describeTransition`，核心动作有动画过渡
- [ ] **所有动作(包括信息型动作、人类动作)都有视觉反馈**——没有物件移动的动作必须在行动玩家头顶弹 popup(§3.6)
- [ ] **对玩家本人揭示私密信息的动作用阻塞式 `reveal` step**（§3.7）——Priest/Baron 型的"偷看 / 亮牌"不能只靠气泡飘过
- [ ] 每个玩家容器(含人类自己的 player-area)都有 `data-player` 或 `data-opponent` 属性,供 popup 定位
- [ ] 关键元素有 `data-*` 属性（动画选择器需要）
- [ ] 白色/浅色元素有边框或阴影
- [ ] 当前玩家有明显标记
- [ ] 可操作元素有 hover 和 cursor:pointer
- [ ] 关键数字字号 ≥ 14px
- [ ] 可点击元素 ≥ 36×36px
