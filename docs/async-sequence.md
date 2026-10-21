# 异步表面的时序图

`async.hpp`（design.md §4.11）里那几个协程相关类型——`task<T>`、
`detail::operation_core<T>`、`task_awaiter<T>`、`operation_queue`、`executor`、
`callback_guard<R>`——在两条路上各自怎么搭手。不带文件名的行号都在
`include/uniorm/async.hpp`：那里只剩声明与模板。函数体——队列那五个成员、会话与
句柄、`executor` 的虚析构——都在 `src/async/async.cpp`，写作
`async.cpp:NNN`。

## 一、`co_await session.insert(rows)`（默认池，未设 run_on / resume_on）

```mermaid
sequenceDiagram
    autonumber
    participant TH as 调用方线程
    participant PA as 父协程帧 task(void)
    participant SE as async::orm
    participant CO as operation_core（堆上，shared_ptr 计数）
    participant TK as task(size_t)
    participant Q as operation_queue
    participant WK as executor 的 worker
    participant BD as run_sync 帧（body）

    TH->>PA: drive 父协程（block / start / 外层 await）
    PA->>SE: insert(rows)
    SE->>BD: 造协程：分配帧 + core
    Note over BD: initial_suspend = suspend_always（:298）<br/>body 一行都没跑
    SE->>CO: bind(&queue_, run_on_, resume_on_)（:708）
    SE-->>PA: 惰性 task（未驱动 = 无副作用）

    PA->>TK: operator co_await（:428）
    TK->>CO: await_ready() 恒为 false（:359）
    Note over TK: 恒为真挂起：队列空着也走一次 submit
    TK->>CO: await_suspend(parent)（:365）
    CO->>CO: continuation = parent；claim(awaited)（:367、:369）
    CO->>Q: push(starter)（:187）
    alt 队列已满（waiting + in_flight 到 submit_limit）
        Q-->>TK: 抛 submit_overflow
        Note over TK,PA: await_suspend 接住它（:371）：unclaim（:375）
        Note over TK,PA: 存进 failure（:376）后交回父帧，不穿过这条线程
    else 收下
        Q->>Q: drain()：in_flight_ = true（async.cpp:68）
        Q->>WK: begin()：target.submit(frame.resume)（:201）
        Q-->>TK: 返回
        TK-->>TH: 父协程挂起，控制权回到 drive 它的线程<br/>调用方线程不 park
    end

    WK->>BD: frame.resume()
    BD->>BD: work() = db_.insert(copy)：同步批扫，整批一个操作
    alt 跑成
        BD->>CO: return_value(v) → value.emplace（:285）
    else 抛出
        BD->>CO: unhandled_exception() → failure（:276）
    end
    BD->>CO: final_suspend() → final_awaiter（:317、:304）
    CO->>Q: finish()（:329）
    Note over Q: 先交出这一格、启动下一条请求，再交付结果<br/>钉的是开始的先后，不是完成的先后
    CO->>PA: 对称转移 return continuation（:346）
    Note over CO,PA: 设了 resume_on 就改走 hand_over()（:212）：<br/>续体交给点名的 executor
    PA->>CO: await_resume() → take()（:382）
    CO-->>PA: size_t，或原样重抛同一个异常对象
    Note over CO: 帧由 core 的析构销毁（:150）：<br/>等结果那条线程与拆帧那条线程碰不上

    TH->>SE: 会话出作用域
    SE->>Q: ~orm() → wait_idle()（async.cpp:162）
    Q-->>SE: waiting_ 空且不 in_flight_ 才返回
```

## 二、三个 drive 动作与回调形，在同一格 FIFO 上的分工

```mermaid
sequenceDiagram
    autonumber
    participant TH as 调用方线程
    participant TK as task
    participant CO as operation_core
    participant Q as operation_queue
    participant WK as worker
    participant CB as callback_guard

    rect rgb(238,242,248)
    Note over TH,WK: block()：park 本线程（:451）
    TH->>TK: t.block()
    TK->>CO: claim(blocked)；launch(requested)
    CO->>Q: push → drain → submit
    TH->>TH: finished.acquire() 停住（信号量 :139）
    WK->>CO: body 结束 → finish() → finished.release()（:333）
    CO-->>TH: 醒来 take()：值或重抛
    end

    rect rgb(238,248,240)
    Note over TH,WK: start(handler)：交出去，本线程继续（:462）
    TH->>TK: t.start(on_failure)
    TK->>CO: on_failure = handler；claim(detached)；launch
    TH-->>TH: 立刻返回（被拒时 unclaim 再抛，:469）
    WK->>CO: body 结束 → finish()，driven 是 detached（:335）
    CO->>CO: 只有 failure 才调 handler：跑成不响
    end

    rect rgb(248,242,236)
    Note over TH,CB: 回调形（第二条路）：答案在 body 里响（:531）
    TH->>Q: insert(rows, on_done) → answer（:742）
    TH->>CO: bind + start(settle_failure)：只占一格
    TH-->>Q: 立刻返回，不等
    WK->>CO: answered 帧：work() 外面一圈 try/catch
    WK->>CB: settle(value 或 R{}, error)（:516）
    CB->>TH: 在这条 worker 上调用 on_done
    Note over WK,Q: 回调握着这一格直到响完：慢的回调挡住下一条；<br/>~orm() 的 wait_idle 因此覆盖"每个回调都响过"
    WK->>Q: finish()
    end
```

## 三、类型分工与生命周期

- **`task<T>`（`:394`）**：装一个 `shared_ptr<operation_core<T>>`，加上三个
  drive 动作与 `co_await`。可移动不可复制；没驱动就丢掉等于无副作用。
- **`operation_core<T>`（`:135`）**：结果槽、`failure`、`finished` 那枚信号量、
  `continuation`、`on_failure`、`queue`、`run_on`、`resume_on`、`driven`。协程帧
  挂在它上面、由它的析构销毁（`:150`），所以它一定比 promise 活得久——promise
  里那个裸指针不能被析构读。
- **`task_promise<T>`（`:295`）**：`initial_suspend` 挂起与 `final_suspend` 的
  对称转移。它是帧的一部分，从不自毁。
- **`task_awaiter<T>`（`:356`）**：`await_ready` 恒 false，`await_suspend`
  里 claim 加 launch，被拒时就地折成这一次 await 的失败。它挂在父帧的
  挂起点上。
- **`operation_queue`（`:85`，函数体 `async.cpp:21-90`）**：每会话一条 FIFO——
  `waiting_`、`in_flight_`、`draining_`、`limit_`。会话按值持有它，`~orm()` 等它
  安静。
- **`executor`（`:36`）**：只有 `submit`，交出活就返回；它的虚析构在
  `async.cpp:17`。`run_on` 说阻塞那一段去哪儿，`resume_on` 说续体回到
  哪儿。
- **`callback_guard<R>` 与 `answered<R>`（`:510`、`:531`）**：第二条路的出口，
  `settle` 幂等。帧带着调用方的回调，响完才交格。

## 四、三个 drive 动作与回调形的差别，在这四点上看

- **`co_await`**（`:356`）：挂起的是父协程帧，不是线程。结果落在 `await_resume`
  （`:382`），失败在 `co_await` 那一点原样重抛。被 `submit_limit` 拒时
  `await_suspend`（`:365`）自己接住那一次抛出：先 `unclaim()`（`:375`），
  再把异常存进 `failure`（`:376`），交回的是父帧而不是穿过跑这一帧的那条
  线程——溢出因此成为这一次 await 自己的失败，从 body 的 `catch` 里出来，
  和驱动报的错走同一条路。这条路没有调用点可抛。task 虽然回到未驱动状态，
  `failure` 却不会再清掉，所以接住之后要重造一次请求，不能对着同一个具名
  task 再 `co_await`。
- **`block()`**（`:407`）：park 在这条线程的那枚二进制信号量上（`:139`）。结果走
  `take()`，失败从 `block()` 重抛。被拒时先 `unclaim()`（`:456`）再原样抛，task
  留在未驱动状态，过一会儿再 drive 是合法写法。
- **`start(handler)`**（`:419`）：调用方立刻返回，`T` 不交付。失败唯一的出口是
  `on_failure`，跑成的那次不响。被拒时同样 `unclaim()` 再抛（`:469`）。
- **回调形**（`:742`）：调用方立刻返回。结果交付给 `on_done` 的第一个参数，
  失败在第二个参数、此时第一个是 `R{}`。空的 `on_done` 同样抛在调用点，被拒的
  那次也抛在调用点且都不响——请求根本没进队列。

两条最容易踩的线：会话必须活过它交出去的操作（这条由库代守，`~orm()` 里那句
`wait_idle()`（`async.cpp:162`）；同样的等待也能显式取
`orm::wait_all_operations()`（`:623`），会话不散就把交出去的都等齐——它只等
**已受理**的那些，被 `submit_limit` 拒的那次不在里面）；配置面与 `sync()`
只属于没有在飞操作的时刻——它们改的正是这条队列要保护的那份状态，两边都不检查。
