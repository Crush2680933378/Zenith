# Zenith Crew 实时事件契约（ZenithDesk 前端对接文档）

> 面向 2.5D 办公室可视化界面（ZenithDesk）。数据源三条通道同构：
> REST 轮询、SSE 流、WebSocket 双向。所有事件带 `seq`（单调递增）与
> `ts_ms`（毫秒时间戳），可按时间轴快进回放。

## 1. 通道

| 通道 | 地址 | 用途 |
|---|---|---|
| SSE 事件流 | `GET /crew/tasks/:id/events/stream` | 只读实时流（text/event-stream，事件名 `crew`，id=seq） |
| 历史事件 | `GET /crew/tasks/:id/events?since=N` | 增量轮询/断线补齐（N=上次 last_seq） |
| WebSocket | `ws://…/crew/ws` | 双向：订阅 + 游戏内动作（审批/取消/投票） |
| 状态板 | `GET /crew/tasks/:id/board` | 1–2s 轮询的稳态快照（防事件丢失的兜底） |
| 聚合板 | `GET /crew/board` | 所有运行中/待审批任务的角色大图（多项目同屏） |
| 名册 | `GET /crew/roster[?task=ID]` | 角色↔AI 形象/模型映射（选立绘的依据） |
| 元信息 | `GET /crew/meta` | 特性开关（前端显隐交互元素） |
| 任务列表 | `GET /crew/tasks` | 大厅视图 |
| 增量迭代 | `POST /crew/tasks/:id/followup` `{"brief":...,"config"?}` | 基于父任务产物开新版本任务（M16） |

**写操作鉴权**：若 `/crew/meta` 的 `features.write_guard=true`，REST 需带
`X-Crew-Token` 头，WS 帧需带 `"token"` 字段。

## 2. WebSocket 协议（/crew/ws）

文本 JSON 帧：

```jsonc
// 客户端 → 服务端
{"type":"watch","task":"crew-1-93275"}          // 订阅（换台可重复调用）
{"type":"approve","task":"…","comment":"做得好"}  // 人工放行（comment 可选）
{"type":"cancel","task":"…"}                    // 取消
{"type":"resume","task":"…"}                    // 断点恢复
{"type":"vote","task":"…","choice":"sqlite"}    // 人工投票（human_vote_required 响应）
{"type":"ping"}                                  // 心跳

// 服务端 → 客户端
{"type":"hello","task":"…"}                      // 订阅确认，随后立刻补全量历史事件
{ …CrewEvent 原样… }                             // 与 SSE 同构的实时事件
{"type":"ack","action":"approve","ok":true}      // 动作回执（false 带 error）
{"type":"pong"}
```

## 3. roster / board / meta 模式

```jsonc
GET /crew/roster?task=ID →
{ "roles": [
    {"role":"architect","display":"架构师","vote_weight":2,
     "model":"deepseek-chat","persona":"deepseek"} ],
  "default_model":"", "task_id":"crew-1-…" }

GET /crew/tasks/ID/board →
{ "task_id":"…","phase":"implement","running":true,"resumable":false,
  "cost_usd":0.0042,"tokens_in":12000,"tokens_out":5000,
  "config":{"stories_mode":false,"require_approval":false,"human_voter":false,…},
  "agents":[
    {"role":"backend","state":"working","detail":"API contract:…",
     "updated_ms":1792854493276,
     "model":"kimi-k2","persona":"kimi","idle_ms":0} ],
  "artifact_count":6,"vote_count":2 }

GET /crew/board →  // 聚合：只含 running / awaiting_approval 的任务
{ "tasks":[ {"task_id":"…","phase":"…","cost_usd":…,"agents":[同上]}, … ] }

GET /crew/meta →
{ "api":"v2","max_concurrent_tasks":4,
  "features":{"persistence":true,"resume":true,"memory":true,"otel":false,
              "webhook":false,"human_voter":true,"exec_verify":true,
              "stories":true,"debate":true,"cancel":true,"export":true,
              "write_guard":false},
  "roles":["pm","architect",…] }
```

**persona 取值**：`deepseek | kimi | glm | mimo | minimax | llama | qwen |
gemini | anthropic | openmi…（openai）`；`""` = 未识别 → 前端用默认立绘。
`personasJson` 引擎配置可强制覆盖（如 `{"pm":"glm","backend":"kimi"}`）。

## 4. 事件目录（CrewEvent）

公共字段：`seq, ts_ms, task_id, phase, agent, type, detail, data?`（data 为
dataJson 解析后的对象；省略表示无）。

| type | 触发时机 | data 字段 |
|---|---|---|
| `phase` | 进入新阶段/轮次 | — |
| `agent_start` | 角色开始一次生成 | — |
| `agent_done` | 角色完成生成 | — |
| `token` | 生成中的增量（≥24 字符/帧+尾帧） | `{delta}` |
| `handoff` | 产物交接（谁→谁） | `{from,to,item}` item∈prd/contract/implementation/bugs/architecture/tests |
| `vote_cast` | **逐票**即时（会议动画核心） | `{question,choice,reason,weight,human}` |
| `vote` | 投票整包结果 | VoteRecord（votes/winner/reason） |
| `debate_round` | PRD 辩论修订轮 | `{round,budget}` |
| `artifact` | 产物入库 | `{artifact:"prd-v2",bytes}`（id 前缀即类型） |
| `test_result` | 每轮测试结果 | `{cycle,passed,failed,total,pass_ratio,failures:[{case,component,detail}]}` |
| `bug_assigned` | 缺陷工单派发 | `{assignee,cycle,budget,issues:[…]}` |
| `exec` | 真执行验证命令（opt-in） | —（detail 含命令与结果） |
| `memory` | 组织记忆命中注入 | `{hits}` |
| `gate` | 预算耗尽/降级放行等里程碑 | — |
| `approval` | 人工门禁（等待/放行；带评论时 agent=human） | `{comment}` |
| `human_vote_required` | 等待人工投票 | `{question,options}` |
| `retry` | 推理失败重试 / JSON 自修正重试 | — |
| `truncate` | 提示词超预算被截断（M16） | —（detail 含 原长→截后长） |
| `cancelled` | 用户取消 | — |
| `error` | 任务失败 | — |
| `webhook` | 回调送达/失败 | `{target,delivered}` |
| `story_started` / `story_done` | story 子环（并行工位岛） | `{story,status?}` |

**agent.state**（board）：`idle | working | voting | waiting | done | failed`。
摸鱼由 `idle_ms`（board 已算好）分档推导，见下表。

## 5. 信号 → 动画映射表（每信号多随机变体，直接抄）

> 随机策略建议：同信号连续触发时避免立即重复上一条（洗牌队列）；
> 优先级 `error/cancelled > bug_assigned > handoff > vote_cast > 其余 > token`；
> `token` 高频，只做粒子级轻动画并可合并。

### 5.1 phase（进入新阶段）
1. 广播喇叭响起，全员抬头看部门灯牌切换颜色
2. 摄像机摇镜到新区域，标题字从上滑入
3. 电梯门打开，显示屏滚出阶段名
4. 打卡钟"叮"，全体小人原地小跳一下
5. 背景天窗光线随阶段变色（需求=晨光/测试=冷色/发布=金光）

### 5.2 agent_start（开工）
1. 拉开椅子坐下，按开机键屏幕亮起
2. 深呼吸搓手活动手指
3. 端着马克杯入座，吹一口气
4. 猛地坐直，把手机扣在桌上
5. 整理领口对镜头点头

### 5.3 token（生成中·打字）
1. 键盘敲击粒子飞溅，屏幕文字滚动
2. 一手托腮思考，屏幕仍在出字
3. 高速敲击手部残影+机械键盘音效
4. 屏幕蓝光映在脸上，字符流瀑布
5. 停顿回读两秒再爆速敲击（节奏变化）

### 5.4 handoff（交接·走动）
1. 起身走到对方工位递文件袋，对方站起双手接
2. 隔着隔板抛纸飞机，纸飞机滑翔特写
3. 桌间传送带把文件夹运过去
4. 拍对方肩膀交接，两人碰拳
5. 视频呼叫图标弹出，对方接起点头
6. 小推车推着一摞文件哐当哐当运达
> 交接完成后：from 方进入摸鱼池（idle_ms 开始计时），to 方立即 agent_start。

### 5.5 vote_cast（逐票亮牌）
1. 举起投票牌（正面选项、背面自家 logo）
2. 按下桌上大按钮，头顶亮对应色灯
3. 举起手机，屏幕上是选项表情包
4. 写便利贴"啪"拍在桌上
5. 起立喊出选项，头顶对话气泡放大
> `weight=2`（架构师）：牌更大/按钮更大+金色描边；`human=true`：老板办公室动作。

### 5.6 vote（结果公布）
1. 计分板翻牌定格胜出项
2. 胜方阵营比心，败方阵营摊手耸肩
3. 天平动画向胜方倾斜
4. 头顶礼花小彩带（仅胜方角色）

### 5.7 debate_round（辩论修订）
1. 白板前两人对峙激辩，手势你来我往
2. 头顶情绪气泡升级 ?! → !!
3. 把一叠修改稿"啪"拍在会议桌中央
4. 唇枪舌剑字效飞舞
5. 有人翻白线（被说服），有人奋笔疾书

### 5.8 artifact（产物归档）
1. 产物卡飞入交付柜，玻璃门反光
2. 大印章"哐"盖下，纸面出现编号
3. 气球吊着文件缓缓升空入柜
4. 打包成快递箱贴标签上货架
> `prd-v2/v3` 等多版本：柜内同格叠放并露出版本角标。

### 5.9 test_result（测试轮结果）
1. 测试员屏幕弹出大绿勾或红叉
2. 通过率仪表盘指针扫动定格
3. 测试员对镜头点头（全过）/摇头（有挂）
4. `failures[]` 每条变一张红色便签贴到对应工位隔板

### 5.10 bug_assigned（工单砸头）
1. 红色工单纸飞机精准砸到责任人工位
2. 工位顶上警灯开始旋转闪烁
3. 测试员拖着工单走来"啪"放在桌上敲两下
4. 电脑屏幕弹窗 BUG×N 弹幕雨
5. 责任人头顶冒汗气泡，键盘开始冒烟
> `cycle/budget`：头顶汗滴数量=已用轮次，budget 到 3 时表情逐渐绝望。

### 5.11 retry（重试）
1. 揉一团纸扔向屏幕弹开
2. 挠头+头顶三条黑线
3. 灌一口水重新坐直
4. 疯狂按退格键，屏幕字倒着消
5. 静坐三秒突然灵光一闪拍桌

### 5.12 approval / human_vote_required（人工门禁·玩家交互）
1. 老板办公室电话铃大作，全屏弹出接听按钮
2. 红色大印章按钮从天而降等待点击
3. 信鸽飞进画面落在工位上带信封
4. 会议室大门上锁，门铃按钮闪烁
> 带 `comment`：便利贴动画贴上看板 + 对话气泡逐字显示评论。

### 5.13 exec（真执行验证）
1. 小人跑向实验台合闸，指示灯逐个亮
2. 终端机吐出打孔纸带
3. 机械臂抓取命令卡执行
4. 全员围过来看实验台（群像）

### 5.14 memory（组织记忆）
1. 走到档案柜抽出旧卷宗吹灰
2. 头顶灯泡亮起+回忆泡泡（上个项目缩略图）
3. 拉开抽屉取出相册翻两页
4. 戴上老花镜翻笔记本

### 5.15 story_started / story_done（story 工位岛）
1. 岛牌"叮"亮起，小人拖着行李箱入座
2. 完成时岛上插旗+小烟花
3. start：桌上摆出该 story 的需求卡；done：卡片盖"完成"章

### 5.16 done / failed / cancelled（终态）
- done：全员起立鼓掌+礼花+交付箱打封条，镜头拉远全景
- failed：目标工位黑屏冒烟，PM 趴桌，众人叹气气泡
- cancelled：广播"下班"，全员耸肩收拾背包关灯

### 5.17 idle_ms 摸鱼梯度（board 轮询驱动）
| idle_ms | 随机动作池 |
|---|---|
| 5–15s | 转笔 / 刷手机 / 伸懒腰打哈欠 / 玩桌上皮筋 |
| 15–40s | 起身泡咖啡 / 浇工位绿植 / 叠纸盒 / 擦眼镜 |
| 40–90s | 打瞌睡点头 / 戴耳机脚打拍 / 转椅转圈 |
| >90s | 趴桌睡+呼噜气泡 / 看窗外发呆 / 折纸飞机 / 喂鱼缸 |
- 彩蛋（低概率随机替换）：偷吃零食被同事看到缩回去 / 对屏幕傻笑 / 给绿植说话

### 5.18 cost_usd（成本氛围）
1. 任务头顶悬浮 "$0.0042" 数字翻牌
2. 每次增长时金色小金币飘向账本
3. 成本超过阈值（前端自定）老板办公室窗口探头

## 6. 实现要点

- **事件不丢**：SSE 断线重连后用 `Last-Event-ID`（=seq）或 `?since=` 补齐；
  board 轮询做最终一致兜底。
- **token 合并**：token 事件高频，前端可 100ms 批量合并后再驱动粒子。
- **快进回放**：历史事件按 `ts_ms` 排序，前端可压缩时间轴重放开场。
- **多任务同屏**：`GET /crew/board` 一帧拿全，agent 岛按 task 分区。
- **立绘映射**：`roster.persona` → deepseek/kimi/glm/mimo/minimax 立绘；
  `""` 用通用员工立绘；`human` 用老板立绘。
- **写操作**：WS 帧（approve/cancel/vote/resume）自带 ack，适合做按钮反馈。
- **成本预算（M16）**：`config.cost_budget_usd` 超限时任务在 phase 边界转
  `awaiting_approval`（gate 事件含限额）；每次放行限额翻倍——前端应把这类
  approval 与设计审批区分展示（detail 带 "cost budget"）。
- **followup（M16）**：子任务 `board`/`taskInfo` 带 `parent_task_id`；继承产物
  的 agent 为 `"inherited"`（v1），前端可画「传承」特效。
