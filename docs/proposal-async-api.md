# 提案：异步操作接口——把"等驱动"这件事从调用线程挪走

状态：**已采纳，§10 步骤 1–4 与步骤 6 已落地**（步骤 5 那两件事没做）。落地形状记在
`docs/design.md` §4.11，那一份的口径是"文档承诺 = 当前实现"；本文剩下的价值是那
些理由与实测数字，它们不属于设计文档那种口径。所有关于 ODBC 异步的说法都是
2026-10-16 在本机（unixODBC 2.3.12 + MariaDB Connector/ODBC `ma-3.1.12`，服务端
10.6.4，DSN `docker_maria_zd`）实测的，不是从规范推的；复现办法见 §1.2。协程那一
条在同一台机器的 GCC 11.5.0 上验过，MSVC 一侧还没人验（§7.1 记下谁还没验）。

`docs/design.md` 也已经按 §11 那份清单改过——那一节逐条标了落在哪。正文里凡是
"第一版里没有它""落地的只有这一侧"的说法，都在原处补了它后来落地与否，删掉的是操
作序列本身留下的过期句子。

## 1. 要解决的问题

### 1.1 现在谁在等

库里所有 I/O 都压在调用线程上，一条语句从发出到物化完最后一行，调用线程全程
不能干别的：

- `connection::execute()` 与 `execute_update()`
  （`include/uniorm/connection.hpp:52-54`）各自走完 prepare、bind、
  `SQLExecute`；
- 块取循环在调用线程上一块一块地要
  （`include/uniorm/detail/fetch_rows.hpp:36-45`，步骤 1 之后 `orm::query<T>`
  与构建器的 `all()` 都调它）；
- 动态行取值时可能**再发一次往返**——缓冲装不下的长文本/长二进制由
  `read_long_text()` / `read_long_bytes()` 续读，而它们藏在
  `result_set::current()` 里面（`src/result_set.cpp:247-254`，调
  `:185` 与 `:197`）；
- 一次批量写是**一串**往返：按 `paramset_size` 一块块 execute，外加首尾各一次
  autocommit 切换（`src/orm.cpp:677-681` 的 `begin_batch`，三个调用点在
  `:705`、`:755`、`:795`；扫描本体在 `:501-515` 与 `:576-603`）。

一个 N 路并发的服务今天就占 N 条线程，每条都卡在驱动里。这是本提案要动的东西。

但要说清它换到什么。**仿真异步不减少阻塞线程的总数，只改变是哪条线程在阻塞**：
调用线程不再进驱动，改由库持有的 worker 进，worker 的条数就是并发上限。它对
"我的线程不能被占住"这一类部署直接成立（事件循环节点、GUI 线程、请求线程要去
干别的）；对"我要用一条线程撑一千个连接"那一档**不成立**，那档要等原生异步
（§9 第 1 条）。这条区分是本提案全部取舍的根据，所以写在最前面，免得读者按后
者的收益来评估前者。

### 1.2 四条实测事实决定方案的形状

探针是一段一百二十行的独立程序，链 `-lodbc`：`SQLDriverConnect` 上活连接，问四
个异步信息字，再分别在 dbc 与 stmt 上请求异步、回读、发一条 `SELECT 1`。跑出
来的是：

1. **连接器自己报了没有异步。** `SQLGetInfo(SQL_ASYNC_MODE)` → `SQL_SUCCESS`，
   值 `0x0`，即 `SQL_AM_NONE`；`SQL_MAX_ASYNC_CONCURRENT_STATEMENTS` 也是
   `0x0`。
2. **ODBC 3.8 那两个异步信息字被驱动拒。** `SQL_ASYNC_DBC_FUNCTIONS` 与
   `SQL_ASYNC_NOTIFICATION` 都回 `SQL_ERROR`，诊断
   `HY096 [ma-3.1.12]Invalid information type`。
3. **"设了"不等于"设上了"，而且是静默的。**
   `SQLSetConnectAttr(SQL_ATTR_ASYNC_ENABLE=ON)` 回
   `SQL_SUCCESS_WITH_INFO`，诊断 `01S02 [ma-3.1.12]Option value changed`；
   语句侧同一句多带了服务端名：
   `[ma-3.1.12][10.6.4-MariaDB-1:10.6.4+maria~focal]Option value changed to`
   `default (SQL_ATTR_ASYNC_ENABLE)`。两边 `SQLGet*Attr` 回读都是 `0x0`，随后
   `SQLExecDirect("SELECT 1")` 回 `SQL_SUCCESS`——不是
   `SQL_STILL_EXECUTING`，即它照旧同步跑完了。
4. **驱动管理器根本没有异步入口点。** `nm -D --defined-only /lib64/libodbc.so.2`
   导出 155 个 `SQL*`，`SQLFetch`、`SQLExecute`、`SQLGetInfo`、`SQLSetStmtAttr`
   都在，`SQLCompleteAsync` 不在；安装头 `/usr/include/sqlext.h` 里连它的声明
   也没有。异步请求就算被哪个连接器接受，也没有 API 去把那个"还在执行"的调用
   收尾。

第 1、2、4 条叠起来，`docs/design.md:334` 那句"刻意不设
`SQL_ATTR_ASYNC_ENABLE`（无异步）"第一次有了数字，而它同时是本提案的前提：**第一
版异步只能是同步契约之上的仿真**。接口要按"以后某个 backend 能拿原生异步把这
一层替掉、调用方一行不改"来切，但那一层现在不建（§2 E）。

顺带一条编译事实：本机 GCC 11.5.0 用 `-std=c++20 -Wall -Wextra` 直接编译、链接、
跑通一个 `<coroutine>` 的最小 `task<int>`，加 `-fsanitize=address` 同样过，**不
需要 `-fcoroutines`**（GCC 10 才要，见 §7.1）。

### 1.3 与既有决策的关系

`docs/design.md:56` 的基础决策写"接口风格 纯同步"，`:31` 把异步列进 Out of
Scope，同时写下"未来可在线程池上包装"——本提案就是那句"未来"的具体化，也接住
§9 路线图第 4 条（`:2329`"异步包装层（libpq backend 可用原生异步）"）。所以它
不是推翻决策，是把决策留下的那条路铺法写清楚。

## 2. 五个备选

### A. 用 ODBC 自己的异步

**拒**。§1.2 第 1、2、4 条：没有能力，也没有收尾入口点。第 3 条最坏——请求被
`SQL_SUCCESS_WITH_INFO` 接走、回读却是 OFF，行为与不开异步逐字节相同。把接口架
在一个驱动可以静默拒绝的开关上，等于把"是不是真的异步"交给部署环境决定。

### B. `std::future<T>`

**拒**。`future::get()` 自己就阻塞，而 C++20 没有 `then()`（那一路要到 C++26），
于是"拿到一个 future"仍然要么轮询要么占一条线程等它——问题原样搬一遍。
`std::async` 每次调用起一条线程，且最后一个 future 析构会 join 共享状态，正好把
worker 数量变成不可控的那个东西。库内不用它做对外交付形状。

### C. 回调（`then(cb)`）

**不作对外主表面，但它是内部唯一的机制**。串两个数据库操作在回调形状下就是嵌套
闭包，异常要交付就得每个回调里再包一层 `catch`；而且"结果什么时候被谁拿"散进每
个调用点，§3.4 那条"失败必须有名字"的规矩在这种表面上只能靠约定。它的好处在别
处：错误交付与线程交接在实现里就是一次 `operator()`，不必为它另造
machinery——`start()` 的 handler 用的正是它。

**事后（§3.5）**：它确实又成了一条并列的路，但不是主表面——范围收在四个 CRUD
名字上，一次调用一个回调。上面那句"散进每个调用点"的担心并没有被推翻，只是从协
程链收进了一个闭包：调用方只在这一条路上写一句回调，多操作仍回到 D。

### D. 协程 `task<T>` + 核心持有的 worker executor（推荐）

C++20 自带协程、三个编译器都能编（§7），`co_await` 把"发出去—等回来"写成一句
话，`start()` 把"发出去—不管"也写成一句话（§3.4），异常按既有的
`uniorm_error` 家族穿过挂起点（与 `docs/design.md:58` 那格"错误处理 = 异常"同
一口径），一次操作的步骤天然是个函数体而不是状态机。代价是要手写五百行上下
（落地后数到 1251 行，其中代码 855——`include/uniorm/async.hpp` 855、
`src/async/` 那两件 396）：`task` / awaiter / executor / worker 池 /
会话队列（§10 步骤 2、3）——这是本提案唯一新造的抽象，且不外溢到同步路径
（§7.3）。

### E. 现在就把异步塞进 backend 契约

**拒**，两条独立的理由各自都够：

1. **没有消费者。** `backend::capabilities` 上四个没人读的占位刚被清掉
   （`docs/design.md` §9 第 9 条），§5.2 的边界纪律就是"清单只装核心真会分岔的
   地方"。今天没有任何一个 backend 能答异步（§1.2），加进去的第一个读者是本提
   案自己造的。
2. **它要动 vtable，而 D 不用。** 往 `statement_iface`
   （`include/uniorm/backend/backend.hpp:121-180`）或 `connection_iface`
   （`:182-223`）里插虚函数槽会移动其后所有槽位，按 §5.1 的规矩要升 minor ABI，
   已装的 plugin 全部重编。D 的第一版**不动这两个接口**（§7.2），于是 ABI 上是
   中性的一次加法。

原生异步留给 libpq 落地那一次。届时加的是"socket 就绪"这一族东西，形状与今天
这层不同（§9 第 1 条），那时按它自己的读者升 ABI。

## 3. 交付形状

一句话：异步侧**不新写一条数据库通路**——每个 task 的函数体就是既有的那次同步
调用，包这一层只为回答两件事：这次跑在哪条线程上、结果怎么交回去。所以读下面
的形状时，把 `async::orm` 看成"同步 `orm` 加一份并发表达"，而不是第二个 ORM。

```cpp
namespace uniorm::async {

// 一个待办的工作。worker 跑完它，它自己负责把结果交给等待者。
class UNIORM_API executor {
public:
  virtual ~executor() = default;
  virtual void submit(std::function<void()> work) = 0;
};

// 核心默认的那一条池：N 条线程，线程的存在就是为了阻塞在驱动里。
// 尺寸即并发上限，不是队列深度。
UNIORM_API executor& default_executor();
UNIORM_API void default_executor_size(std::size_t threads);

template <class T>
class task {
public:
  // 挂起等待者，把整个操作交给 executor；被唤醒即取结果或重放异常。
  T block();
  // 指定跑在哪条 executor 上——给测试留的缝，也是 §8 第 2 层能钉住
  // FIFO 顺序的原因。不带 executor 的那两个重载走
  // default_executor()。
  T block(executor& run_on);
  // 交出去就不管：结果丢掉，失败必须有名字（§3.4）。
  void start(std::function<void(std::exception_ptr)> on_failure);
  void start(
    executor& run_on, std::function<void(std::exception_ptr)> on_failure);
  // co_await 用的 awaiter 三件套（await_suspend / await_ready /
  // await_resume）从略。
};

}  // namespace uniorm::async
```

`task<T>` 是**惰性的**：函数返回时什么都没发生，闭包在 `block()` / `start()` /
被 `co_await` 那一刻才 submit。这一条决定了"造了个 task 又扔掉"是合法的（没发
出去就是没发生），也是 §3.4 那个 `start()` 形状能成立的前提。

被 `co_await` 的那条路现在就在 awaiter 的 `await_suspend()` 里 submit 到
`default_executor()`，续体靠对称转移回到父帧——所以 awaited 形状下"跑"这一侧
暂时无从注入，注入点要等 §5 那条会话队列把两种驱动形状收在同一份 FIFO 上（step
3 的 `async::orm`）。`resume_on` 同理：它是每会话一个的旋钮，不是每 task 一个的
参数，第一版里没有它。

**上面那两句是提案当时的形状，步骤 3 与 4 已经越过它们**：队列落地后，awaited
那一条的 submit 取自会话的 `run_on`（没设过就仍是默认池），三种驱动形状在同一份
FIFO 上排同一个先后；`resume_on`（`include/uniorm/async.hpp:608`）也立了起来——
设过它，`final_suspend` 那个 awaiter 改走 `hand_over()`（`:212`）把续体投给点名
的 executor，投不动就地在当前线程把它续上；没设过，仍是这一段说的对称转移，一条
链因此保持在一帧深。

同步一侧不动：`orm` 与 `connection` 的全部签名保持原样。一个 async 操作用
`block()` 也能取结果，于是测试与一次性脚本可以只写异步一种形状。

`uniorm::async::orm` 按值持有一个 `uniorm::orm`，因而**不新增状态宿主**——租约仍
在那条既有链上（`orm` 里的 `std::optional<pooled_connection> pooled_conn_`，
`include/uniorm/orm.hpp:302`）：

```cpp
namespace uniorm::async {

class UNIORM_API orm {
public:
  orm() = default;
  // 与同步对象同款的三条入口：都在建会话那条线程上把连接开出来。
  explicit orm(std::string_view);
  explicit orm(connection_spec const&);
  explicit orm(connection_pool&);
  explicit orm(uniorm::orm db);      // 接管已连好的（含从 pool 租的）
  task<void> connect(std::string_view);
  task<void> connect(connection_spec const&);
  task<void> disconnect();

  // 阻塞那一段跑在谁身上；默认是核心的 worker 池（§3.3）。
  void run_on(executor&);
  // 续体回到谁身上；不指定就在跑完那一趟的 worker 上继续。
  void resume_on(executor&);

  // 这条会话上允许排多少个还没轮到的请求（§5）。
  void submit_limit(std::size_t);

  // 配置面里只改内存的那几个，按名字转发到内层那一个对象（下面第一段）。
  template <class T> mapping_builder<T> map(std::string_view);
  std::size_t row_array_size() const;
  void row_array_size(std::size_t);
  std::size_t paramset_size() const;
  void paramset_size(std::size_t);
  bool auto_commit() const;
  dialect::identifier_case identifier_case() const;
  void identifier_case(dialect::identifier_case);

  // 目录读没有异步门，由这一句交回内层对象（§4.3 第 2 条）。
  uniorm::orm& sync();

  // 会不会发一次往返，是"进 task"与"转发/走 sync()"的唯一判据。
  task<std::size_t> execute_update(std::string_view, params const& = {});
  template <class Entity> task<std::size_t> insert(
    std::vector<Entity> const&);
  template <class T> query<T> query();
  template <class T> task<std::vector<T>> query(
    std::string_view, params const& = {});
  task<void> auto_commit(bool);
  task<void> commit();
  task<void> rollback();
  task<transaction> begin();
};

}  // namespace uniorm::async
```

`async::orm` **只装异步自己的东西**：task 形状的那一组、`run_on` /
`resume_on` / `submit_limit` 三个旋钮、以及 §5 那条队列与 `pending_` 计数。配置
面里只改内存的那几个（`map` / `row_array_size` / `paramset_size` /
`identifier_case`，外加 `auto_commit` 的读档）按名字转发给 `db_` 那一个对象，目
录读（`validate` / `schema` / `resolve_identifiers`）留给 `sync()`。`db_` 仍按值
住在会话里，所以两边之间没有第二份状态，转发与 `sync()` 都不新增状态宿主
（`orm.hpp:305` 那条链不变）。

转发的代价就是上一版拒绝它的理由，记在这：**同步侧每加一个只改内存的 setter，
就欠下一处可能悄悄漏掉的同步**。这一笔由用例钉住——在会话上写下的四个值
`sync()` 读回同一个，反向也一样，所以漏掉的那一处一旦漏了就红。

判据仍然是**会不会发一次往返**：`auto_commit(bool)` 要走一次 `set_autocommit`
（`src/orm.cpp:328-334`），所以它在 task 那一组，读它才是转发；`row_array_size`
与 `paramset_size` 只改内存（`include/uniorm/orm.hpp:70`、`:77`），转发的就是这
一个内存写。`validate()` / `schema()` / `resolve_identifiers()` 是目录读，留在
`sync()`，是本提案承认的阻塞点（§4.3 第 2 条）。

上一版不转发，省的是"欠一处同步"；换来的是配置写法多一段
（`db.sync().map<User>("users")…`）。这一版反过来付那笔账，因为先难看的是构造：
要立起一把会话，调用方得先造一把 `uniorm::orm`，而"把配置写进会话"正是它的活儿。
入口因此与同步对象同款三条——连接串、四件套、`connection_pool`——都在建会话那条
线程上开出连接，`uniorm::orm` 不再出现在调用方眼里。一份注册配方喂两个面这件事
因此换了一个写法：`map()` 在两个对象上同名同参，配方只写一遍、对着手里那把写就行，
集成用例里那一个 `map_entities(Session&)` 同时喂同步与异步两侧。接管那条
（`explicit orm(uniorm::orm)`）仍留着——它说的是"我已经配好了一把同步对象，
连里面的租约一起拿走"，而现在没有任何用例或非样例代码走它。

一条规矩随配置面而来：**它只能在没有在飞操作的时候用**（配置动作改的是注册
表与连接属性，与 §5 那条队列保护的是同一份状态）。第一版不做运行期检查——同步
侧今天也没检查，异步不新增一条禁令就管不住的东西；这条写进头文件注释。

那为什么不把这些成员直接长在 `uniorm::orm` 上，省掉第二个类型：

1. **异步要带新状态**。除了 task 形状，会话上还多了 §5 那条队列、在飞计数、两
   个 executor 指针与一个上限，今天 `orm` 一个都不需要。给 `orm` 加数据成员就改
   变它的 `sizeof`，按 §7.2 那套规矩是一次 ABI 事件——上一轮只往
   `connection_iface` 插一个虚槽就已经欠下一格（design.md §5.2 末尾记着），长
   在 `async::orm` 上则一个已发布类型都不动。
2. **协程会进每个消费者的编译单元**。`orm.hpp` 在伞形头里，awaiter 三件套连着
   `<coroutine>` 一起进去；§7.3 定的是"不用异步的人不付编译时间"，长在 `orm` 上
   就把这条作废。
3. **§1.3 那条对外承诺**：`design.md:56` 记的接口风格是同步为主，同步一侧签名全
   部保持原样是这一版的前提。把 `orm::query<T>()` 的同名成员改成返回 task，今天
   写 `db.query<User>().all()` 的消费者连编译都不过。

反过来也要划清：`async::orm` 里**没有第二条数据库通路**（§3.2 第 3 件）——它是
一层形状加一份状态，不是第二个 ORM。

名字走"命名空间带异步、类型名与同步那一侧同字"这条路（`async::query<T>`、
`async::transaction` 与 `uniorm` 侧同名）。在命名空间之外再给类型名挂一遍
async（于是念成两个 async）在自己家门外要重复两遍，`session` 这种换一个词的
说法则逼消费者重学一遍"它对应 `orm`"；代价是同时 `using namespace` 两家会撞
名，所以两侧的名字都写限定名——这与 `std::pmr::string` 与 `std::string` 并存的
口径一致。

### 3.1 调用方看到的样子

外部拿到的只有四样东西：一个头（`<uniorm/async.hpp>`，不进伞形头）、一个类型
（`uniorm::async::orm`）、一种交付（`uniorm::async::task<T>`）、一个把任务跑起
来的动作（要结果就 `block()`，不要结果就 `start()`，§3.4）。写起来是：

```cpp
#include <uniorm/async.hpp>

struct User { std::int64_t id; std::string name; };
enum class Order_state { open, shipped };
struct Order { std::int64_t id; Order_state state; decimal_t amount; };

using namespace uniorm;

// 业务写成一个协程；库里没有任何一处知道它的结果要给谁。
async::task<std::vector<User>> friends_with_orders(async::orm& db) {
  auto users = co_await db.query<User>()
    .where(gt(&User::id, 1LL)).all();       // task<std::vector<User>>
  if (users.empty()) {
    co_return users;
  }
  co_await db.execute_update(               // task<std::size_t>
    "update users set note = ? where id > ?",
    params{ "x", std::int64_t{ 1 } });
  co_return users;
}

int main() {
  async::default_executor_size(8);   // worker 条数就是并发上限（§9 第 1 条）
  async::orm db;                     // 这把会话同一时刻最多一个操作在飞

  db.map<User>("users").primary_key("id", &User::id)
    .column("name", &User::name);          // 配置：转发的名字
  db.map<Order>("orders").primary_key("id", &Order::id)
    .column("state", &Order::state)
    .column("amount", &Order::amount);

  db.sync().validate(validation_mode::strict);  // 目录读：同步（§4.3）
  db.connect(connection_spec{ "odbc", "docker_maria_zd", "", "" })
    .block();   // 一次性脚本：起任务并等它；凭据在 DSN 里

  auto rows = friends_with_orders(db).block();
}
```

扇出靠**多把 `orm`**，不靠一把 `orm` 上排多个 await（§5）：

```cpp
async::task<std::size_t> total(async::orm& db) {
  auto rows = co_await db.query<Order>().all();
  co_return rows.size();
}

// 各用一把 orm 才叫并行；排队不等于并行（§5），每条仍只占一条 worker。
void run_two(async::orm& first, async::orm& second) {
  std::thread lhs([&] { auto count = total(first).block(); });
  std::thread rhs([&] { auto count = total(second).block(); });
  lhs.join();
  rhs.join();
}
```

同一把 `orm` 上先后 await 两个操作不会并行，但也不会报错：第二个排进 FIFO，第一
个落地才轮到它（§5）。要真并行就各用一把 `orm`；`task` 上没有 `when_all` 那一族
组合器（§9 第 7 条）。

事务的写法与作用域一致，只是收尾显式：

```cpp
async::task<void> ship(async::orm& db, std::int64_t order_id) {
  auto tx = co_await db.begin();
  co_await db.execute_update(
    "update orders set state = ? where id = ?",
    params{ Order_state::shipped, order_id });
  co_await tx.commit();   // 漏这一句不是错，但兜底那次 rollback 会阻塞
}
```

失败形状与同步那一侧同一个家族：`co_await` 处抛出 `backend::backend_error`（带
驱动诊断那三行），队列满时在调用点抛 `submit_overflow`（§5），`try`/`catch` 照常
写；一个 `task` 从创建到取走结果之间不碰 ODBC 句柄，句柄只在 worker 上被碰。

### 3.2 async 里面装了哪些东西

分界线沿用既有那条规矩：公开的进 `include/uniorm/async.hpp`（安装、带
`UNIORM_API`），实现细节与源文件同放 `src/async/` 且**不安装**——
`CMakeLists.txt:69-70` 写的"私有头挨着源文件、`src/` 不在任何 include 路径上"
正是为此。唯一一处越不过去的例外：`task<T>` 是模板，它的驱动动作直接读写那块
共享状态，所以状态本身、promise 与 awaiter 只能跟着模板进安装头，落在
`uniorm::async::detail` 里——`detail` 是"形状写在头里但不是承诺"的那一层。下面
仍按原编号把它们列在私有侧那一格里，因为它描述的是实现形状，不是调用方的界面。

**公开侧六样**：

- `async::orm` — 会话：task 那一组、三个旋钮、§5 那条队列；入口与同步对象同款
  三条，配置面里只改内存的那几个按名字转发，目录读由 `sync()` 交回内层对象
  （§3）；
- `async::query<T>` — 三个终端 `all` / `one` / `count` 返回 task；
- `async::transaction` — 显式 `commit` / `rollback`；
- `task<T>` — 交付类型；它是模板，签名出现在每个终端的返回类型里，只能公开；
- `executor` — 一个虚函数 `submit(std::function<void()>)`，公开的理由见 §3.3；
- `default_executor()` / `default_executor_size()` — 核心那条 worker 池的句柄
  与尺寸；不接任何池的调用方也要起得了任务。池在第一次用到时才立起来，所以
  `default_executor_size()` 写在 §3.1 那种启动位置就是"决定创建尺寸"，写在之后
  就只是"往上加"——它从不减，0 一律抛。

**不在 async 里的**：新错误 `submit_overflow` 落在 `include/uniorm/error.hpp`
（§5）。住在错误家族那一头的理由与 `pool_timeout` 同一条：它说的是"用量不对"而不
是驱动报的错，而错误家族不分同步异步两侧。ODBC 句柄、backend 契约、目录接口一概
不碰（§7.2）。

**私有侧四件**（`src/async/`，随库编译、不安装；第 2、4 件按上面那段进安装头）：

1. `worker_pool` — `default_executor()` 的实现：N 条 `std::thread` 加一条队列。
   线程的唯一用途是阻塞在驱动里（§1.1），所以它的尺寸就是并发上限。
2. 每个操作一块堆上的共享状态（`detail::operation_core<T>`），由
   `std::shared_ptr` 引用计数：结果槽、`std::exception_ptr`、待 resume 的那个
   `std::coroutine_handle`、detached 那条路上的 `on_failure`、`block()` 那一侧用
   的一个 `std::binary_semaphore`，再加"被哪一种动作驱动"的那个枚举。协程帧挂在
   core 上、由 core 的析构销毁，promise 反过来只持一个裸指针——core 总比帧活得
   久，所以 promise 的析构不许读那个指针。结果住在 core 而不是帧里，等结果的那
   条线程与最后销毁帧的那条线程就碰不上（§3.4 第 2 条）。
3. 提交物：一个闭包，函数体就是既有的那一段同步代码（`orm::execute_update`、
   `fetch_rows` 那个循环、整段批量扫描）。**async 里没有第二条数据库通路**——
   异步只回答三件事：这次同步调用跑在哪条线程上、结果怎么交回去、以及这条连接
   上谁排队谁在飞。
4. 每会话那条 FIFO、`pending_` 计数与 `submit_limit` 那个上限（§5、§3.4）。
   它与第 2 件住同一处，理由也是同一条：core 的模板要往队列里 push、跑完要
   finish，会话又按值揣着一条队列，所以 `detail::operation_queue` 的声明只能
   跟着头安装（也因此要导出）；后来这一步之后，它连函数体也不留在头里了——
   `src/async/async.cpp` 一个编译单元装下三样：队列那五个成员、会话与句柄除
   模板外的一切、`executor` 的虚析构（后者放库里是让那张 vtable 落在库中，
   调用方自己派生 executor 时靠链接取得它）。`src/async/` 里另一个文件是
   `worker_pool.cpp`，它单开是因为要 include 私有头 `worker_pool.hpp`。
   安装头因此只剩声明与模板。

### 3.3 executor 公开的理由：跑与回是两个方向

- **跑**（`run_on(executor&)`，默认 `default_executor()`）：这条线程会进驱动里
  阻塞，所以它得是"可以被阻塞的线程"。调用方自带池时，上限归调用方算；
- **回**（`resume_on(executor&)`，默认跑完这一趟的 worker）：续体落在谁身上。
  事件循环那一族的调用方要的是"回到我这一条"。

两侧各收成一个指针加一个 setter，默认值让 §3.1 那种一次性脚本一个都不必写。
"跑"这一侧不给注入的话，§8 第 2 层落不了地：FIFO 的顺序与"在飞的最多一个"这两
件事要用可数的 `submit()` 来钉，拿真 worker 反而测不准（§8 第 4 层是另一件事）。

第一版落地的只有"跑"这一侧，而且先长在 drive 动作上而不是会话上：
`block(executor&)` / `start(executor&, handler)` 这两个重载就是 §8 第 2 层要用
的那两个缝（`tests/unit/async_executors.hpp` 里的 `same_thread` 与 `hand_pump`
——前者让 `block()` 决定性地在调用线程上跑完，后者证明惰性，并且在 `task` 对象
死掉之后才把活 drive 完）。§5 那条队列把 awaited 与 detached 收进同一份 FIFO
时，这个缝升格成了会话上的 `run_on()`，两个重载留下来当机制层的缝，规矩是会话
定过的事优先、drive 动作上那个只在会话没定时有发言权。反过来，会话挂着手泵时对
着它 `block()` 就是吊死：这个形状用例里不出现，出现的地方是注释。

**"回"这一侧随步骤 4 落地**：会话上多一个 `resume_on(executor&)`
（`async.hpp:608`），`operation_core` 多揣一个指针（`:144`），只在
`final_awaiter::await_suspend()`（`:323`）那一处读它。§8 第 4 层要钉的"续体真落
到指定线程"因此有了一条不靠真 worker 的写法：一个手泵负责跑、另一个手泵负责接，
两边各数自己的 `submit`。

### 3.4 交出去就不管：`start()`

调用方其实有两种人：要在结果上接着算的（写协程，`co_await` 或 `block()`），和
只要把事情交出去的（记一次访问、更新一个计数）。后者要的不是更聪明的 await，
是**别逼他收结果**：

```cpp
void log_visit(async::orm& db, std::int64_t user_id) {
  db.execute_update("insert into visits(user_id) values (?)",
      params{ user_id })
    .start([](std::exception_ptr bad) {
      if (bad) {
        // 记一条日志、加一个计数；这一次写丢了不能没有痕迹
      }
    });
}
```

三条规矩让它成立，每条都换掉一件原本靠纪律维持的事：

1. **task 惰性。** `execute_update(...)` 返回时什么都没发生，闭包在 `block()` /
   `start()` / 被 `co_await` 那一刻才 submit（`await_ready()` 恒 false，
   `await_suspend()` 里提交）。于是"造了 task 又扔掉"从非法变成合法。
2. **`start()` 交出帧的所有权。** core 由 `shared_ptr` 计数，提交时的闭包、
   `task` 对象、await 它的父协程各持一份；`final_suspend` 返回的是一个自定义
   awaiter，它把该接的续体交出去，但**帧从不自毁**——谁放下最后一个引用，谁通过
   core 的析构销毁帧。所以"临时对象上直接 `.start(...)`"是安全写法：`task` 的析
   构只是放下自己那一份引用，不必把 task 存起来，也不需要 `suspend_never` 那条
   自己拆自己的路。三种驱动动作在这一点上同形。
3. **失败必须有名字。** `start()` 只接受一个 `void (std::exception_ptr)`，没有
   无参重载——也就没有"静默扔掉一次写失败"这条路。handler 自己抛出则没人接：它
   是失败与这条线程之间的最后一段，而跑它的两处都不许抛（final awaiter 是
   `noexcept`，starter 展开会让队列停在半路），于是吞在 `deliver_failure()`
   这一层——吞的是 handler，不是那次失败本身。

代价落在两处：

- `async::orm` 上加一个 `pending_` 计数，`~orm()` 等到它归零。**这是本提案里唯
  一一处把"析构会阻塞"当机制用**（§6 那两处只是兜底）；换来的是 §6 那条"会话必须
  活过它交出去的操作"从纪律变成库替你等。
- 一次 `start()` 一百条是合法的（§5 那条 FIFO 承接），但排得上不等于跑得快：同
  一把 `orm` 上的 detach 流是串行的，吞吐就是"这条连接一串往返"的速度；堆过
  `submit_limit` 才响 `submit_overflow`。要并发仍得多把 `orm`——§9 第 1 条那句
  "不省线程"在 detach 形状下还是这个样子。

### 3.5 第二条路：回调形增删改查

采纳之后回过头看，D 那条"主表面"对**一次调用、一个结果**的人要求太多：他得知道
task 是惰性的、只能驱动一次、三个动作各有各的出口，还想知道自己的闭包会落到哪条
线程上——这一串跟"把这几行写进去"没关系。于是并列开第二条路，形状就是每个操作
带一个回调：

```cpp
session.insert(users, [](std::size_t added, std::exception_ptr bad) {
  if (bad) {
    report(bad);   // 这时候 added 是 0，不是"部分成功"
    return;
  }
  std::printf("%zu rows in\n", added);
});

session.query<User>("select id, name from users where id > ?",
  params{ std::int64_t{ 9000 } },
  [](std::vector<User> rows, std::exception_ptr bad) {
    if (bad) {
      report(bad);
      return;
    }
    use(rows);
  });
```

公开的是 `async::orm` 上四个名字的五个重载：`insert` / `update` / `remove` 各一
个（结果 `std::size_t`），`query<T>` 两个（带 `params` 与不带，结果
`std::vector<T>`）。返回 `void`，收与不收都由调用方决定——没有 task 这个对象，
也就没有"驱动第二次"这件事可犯。`execute_update`、`connect`、`begin`、builder 的
`all`/`one`/`count` 一律不做：**这一条只覆盖 CRUD，主表面仍是 D**。

六个代价，都写在这里而不是留给用法去发现：

1. **双通道，先看第二个参数。** 失败时第一个参数是 `R{}`（`0` / 空
   `vector`），不是部分结果。本仓此前没有任何"交付结果的回调"——`start()` 那个
   handler 只交失败（§3.4 第 3 条），所以这条规矩得硬写：判断的顺序不能反过来。
   空的 `on_done` 在调用点就被拒：它会死在 body 里，那一格被答掉却没人听，于是
   `~orm()` 等的是一件永远不会发生的事。
2. **每个重载把同一个工作函数交给队列**（`insert_work` 那一层），回调形不另写一
   份 body。"异步不改发出去的 SQL 一个字节"（§5）在这一条上是由构造保证的，不是
   靠两处保持一致。
3. **回调在 body 里响**，所以它跑在"跑了这次操作的那条线程"上，而且是在这一格
   还握着的时候：回调慢，同一会话的下一条请求就跟着等。这一句是它与主表面唯一
   的时机差别。
4. **`submit_limit` 满了从调用点抛**，与 route 1 同形：请求根本没进队列，因此
   也不会有回调来——`pending()` 与溢出的算术都把它当一次普通操作看。
5. **`on_done` 自己抛出没人接**：它已经是这一格答案的最后一段，抛出成为这个操作
   自己的失败，而报失败的 handler 一看已经答过就原样返回——异常止在这里，回合
   照样交还，会话照样接下一条。
6. **executor 拒收这一格时，回调照样响**：body 从没跑过，失败由请求自带的那个
   handler 递出去——这是唯一一条不经过 body 的出口。

`~orm()` 因此**已经是**回调屏障，不需要第二枚计数：答案在 body 里响，而 body 就
是 `in_flight_` 那一格，`wait_idle()` 原来的谓词（`waiting_` 空且 `!in_flight_`）
等到的一定是"所有回调都响完"之后。换来的用法是调用方可以按引用捕获局部变量。
回调形没有 continuation，所以 `resume_on` 对它没有话语权；§3.3 那条"会话设了一台
只有测试会泵的手泵，析构就卡住"的 footgun 于是少了一个入口、没少掉：body 仍交给
`run_on` 那台 executor，它没人泵，`~orm()` 就等不到那一格交回来。

这不是最先想到的形状。最先想的是外面套一个不绑队列的适配器协程，`co_await` 里面
那个操作，答完再 `start()` 它——那样回调落在这一格放完之后，慢回调不挡后面的请求。
它死在自己的生命周期上：detached 协程挂起时，握着它帧的只有执行器那一枚正在
`resume()` 的 closure，closure 一销毁帧就没了，而里面那个操作还攥着它的句柄当续体。
单测里第一次跑到这条路就是段错误。要修得动 `operation_core`（detached 时自持一枚
引用，final suspend 里放掉），那是 route 1 的机器。既然规则不能改，就把答案搬进
body：代价是第 3 条那句"回调握着这一格"，收益是机器一行没动。

ABI 上这是最便宜的一次加法：五个重载全是头文件里的 inline 模板，一次调用既不新增
导出符号也不新增安装头。会话后来添的成员（析构那本账：一把门加一份弱句柄
清单）也不动谁——`async::orm` 是这一版才出现的导出类型，没有已发布的布局可
破坏，`UNIORM_ABI_VERSION` 照样不动（§7.2）。

## 4. 哪些操作进，哪些明确不进

### 4.1 进：粒度是"一次操作 = 一串往返"

- `connect` / `disconnect` → `task<void>`，提交的是既有的 `orm::connect()` /
  `orm::disconnect()`；
- `execute_update` → `task<std::size_t>`，提交的是同名同步函数；
- 实体批量 `insert` / `update` / `remove` → `task<std::size_t>`，提交的是**整段
  扫描**，连 `begin_batch` 那对 autocommit 往返一起；
- `query<T>(sql, params)` → `task<std::vector<T>>`，提交的是 `fetch_rows` 那个
  循环整体；
- 构建器三终端 `all` / `one` / `count` → `task<…>`，SQL 与 params 由构建器交出
  （§4.2），执行同上；
- `begin` / `commit` / `rollback` → `task<…>`（§6 说清析构那一半）。

把一次批量扫描当作**一个**操作而不是 N 个，是这套形状里最要紧的一个决定。扫描
的正确性靠"整段被同一个事务罩住"（`src/orm.cpp:677-681`）；切成 N 个可交错
await 的小块，就得让 §5 那条 FIFO 的"一个在飞"横跨整段扫描——一次批量于是占着
队列直到全批跑完。同步路径今天已经在一次调用里跑完整段扫描，异步照抄这个边界，
实现与推理都最省。

### 4.2 前置收敛：块取物化循环现在有两份

**这一步已落地**（§10 步骤 1）：那段循环现在只剩 `detail::fetch_rows` 一处，构建
器交出的渲染结果是 `(sql, bound)` 一对。下面两段留的是它当初的形状，里面的文件行
号随之失效。

`orm.hpp` 里的 `fetch_rows` 与 `builder.hpp` 里 `query<T>::all()` 曾是同一段代码
的两份手写（同一个 `resize` + `fill_into` + 异常回退）。异步终端不该写第三份，所
以第一步是把这段并成一处，同时把构建器的渲染结果 `(sql, bound)` 交出去——当时
`render_select` / `render_count` 是私有的，公开的 `build_select()` 刻意把 bound
丢掉了。

并完之后再分链式部分：`where` / `order_by` / `limit` / `offset` 与渲染搬进一个
基类，同步终端留在 `uniorm::query<T>`，异步终端留在 `async::query<T>`。**不让
`uniorm::query<T>` 的终端返回 task**，因为那会把已发布给消费者的签名换掉
（`db.query<User>().all()` 现在返回 `std::vector<User>`）。两份终端的函数体都是
"把同一个闭包交给 executor"，重复的是四行，不是那串链式方法。

### 4.3 不进：三样，各有各的理由

1. **`result_set` 这个活游标。** 它按值持有一条语句（`impl` 里的
   `std::unique_ptr<backend::statement_iface>`，`src/result_set.cpp:61`），
   `next()` 是一块块向驱动要（`:234-245`），而 `current()` 里可能又发一次
   `SQLGetData` 续读（§1.1）。把它交给异步调用方，就要同时承诺游标的线程归属、
   `next()` 的 awaitable 化、以及那一次隐藏往返也被调度。第一版只给"整表物化"
   的 `task<std::vector<row>>`（动态行本身不异步——它的取值是在内存里搬
   variant）。异步流式游标是它自己的一个提案，等有人要读装不下的结果集时再提。
2. **目录一侧：`validate()`、`schema()`、`resolve_identifiers()`。**
   它们是往返，但只在启动/生成期各跑一次；`schema_meta` 是一族**同步返回 vector
   的虚函数**
   （`include/uniorm/schema.hpp:45`、`:88-105`），异步化它等于改那个 vtable
   （ABI），而收益落在一个不在热路径上的调用上。第一版把它列为**唯一一个调用方直
   接调上去的阻塞点**（§6 那两处是析构兜底，不算调用点）：协程里调 `validate()`
   就阻塞那个线程。写清它，比替它造一个假的异步外壳好。
3. **长数据续读不单独成操作。** 它已经在 `current()` / `fill_into()` 里，跟着宿
   主操作一起进 worker，不另立接口。

## 5. 一条连接上同时只能有一个操作：排队，不是抛

`src/statement_cache.hpp` 自己写得很清楚："Not thread-safe; the owning
connection is not thread-safe either."（`:7-8`）。`connection` 与 `orm` 全链无
锁，ODBC 句柄也不允许两个线程同时用同一条 stmt/dbc。**不变量照抄**，变的是它的
表达方式：轮不到你的时候等，而不是报错。

做法是 `async::orm` 上一条 FIFO：每个 drive 过的 task 进队，轮到它才向 executor
`submit()`；跑完（含抛出）后 drain 下一个。排队中的请求不占 worker，所以并发度
仍然是"同时在飞的 driver 调用数 = worker 条数"，§1.1 那句话一个字不改。

这条队列与 executor 自己那条队列（§3.2 第 1 件）不是重复的东西，问的问题不同：
会话队列决定"这条连接上下一个是谁"（顺序、上限、事务内的先后），executor 队列
决定"哪条线程现在有空"。前者每个 session 一条、由完成事件 drain；后者全局一条、
由 worker 取。

这里要承认一次改口。初稿写的是"撞闸立刻抛 `concurrent_operation`"，理由三条，
其中两条经不起推：

- "一旦排队，并发上限就从 worker 条数变成每连接队列深度，与 §1.1 矛盾"——这条
  是错的。队列深度不是并发度：队列一次也放不出第二条并发，它只是把没轮到的请求
  放在内存里。
- "一个 await 还挂着就再发第二个，多半是生命周期 bug"——只对要结果的那一族成立。
  `start()` 那一族的正常用法**就是**"上一个还没落地再发一个"（§3.4），拿它当 bug
  报是把方便性错误地钉成了纪律。
- 剩下"要一套每连接的队列与顺序语义"是真成本，约几十行；它换掉的正是"批量写要
  自己备 N 把 `orm`"这件事，这笔账算得过来。

而且**只给 detach 一条路上队列是自相矛盾的**：`block()` / 被 await 不进队就会插
到已排着的 detach 请求前面，"先 `start()` 一条写、再 await 一次 commit"当场翻出
顺序错乱。所以是一条通道、两个入口共用：

- `block()` / `co_await` = 进队 + 压住自己直到那一轮跑完；
- `start()` = 进队 + 交出帧的所有权（§3.4 第 2 条）。

于是 §3.4 那个 `pending_` 计数正好是"队列长度 + 在飞的那一个"，`~orm()` 等的就
是排空。

"抛"剩下的唯一用处，是把一类用法错从吊死变成当场响：在续体里对同一把 `orm` 调
`block()`——单 worker 时那条 worker 正是被压住的这一条。这个不再由闸来救，改由
两件事兜住：一条写进头文件注释与 README 的禁令，加一个队列上限。

**上限**：`submit_limit(std::size_t)`（默认一个定值，量级 1024，与
`pool_options` 那族旋钮同档），满了 `block()` / `start()` 都在调用点同步抛
`submit_overflow`（`include/uniorm/error.hpp`，与 `pool_timeout` 同档，属
`uniorm_error`——它不是驱动报的，是我们自己的用量错）。队列换掉的是"不用多把
`orm`"，不是"可以无限堆"：没有上限的 detach 流就是把内存当成驱动写缓存，而那件
事没人替调用方判断。

两处例外写在代码里，不留给用法去猜。`co_await` 没有调用点可抛：那一次 push 发生
在 `await_suspend()`（`async.hpp:365`）里，它自己接住，把它折成这一次 await 的
失败，于是溢出从函数体的 `catch` 里出来、和驱动报的错走同一条路，也不再穿过正在
跑这一帧的那条 worker。上限设成 0 当 1 用（`async.cpp:26`），而 `limit_` 是一把
relaxed atomic（`async.cpp:36` 读它）：并发改它不会撕裂，只是哪一次 push 开始按
新数算不确定，所以它仍是配置期的一次性动作。

副作用是"丢任务"这件事本身被惰性消掉了（§3.4 第 1 条）：没 drive 过的 task 析
构，就是把一次还没进队的操作连同参数副本扔掉，合法且不留副作用；drive 过的都由
那块 `operation_state` 活到完成，§6 是同一族问题。

因为"同时只有一个在飞"，**不需要线程亲和**：任何 worker 都能跑下一轮，只要不同
时有两轮。strand / serial-executor 这一族概念整个省掉，是异步层停在六百行代码
（连注释 962 行）而不是两千行的原因之一。

## 6. RAII 的析构：异步里说不圆的那一块

本仓三处依赖"析构时替你收拾"的设施，收拾动作本身是数据库往返：

- `pooled_connection::~pooled_connection()` → `connection_pool::release()`，未提
  交的先 rollback 再复位 autocommit（`src/pool.cpp:257-279`、`:284-288`）；
- `transaction::~transaction()` → 未提交则 rollback
  （契约就写在 `include/uniorm/transaction.hpp:9-11`）；
- 批量写里那个 `std::optional<transaction>`（`src/orm.cpp:677-681`）。

在 worker 线程上这些都没问题（阻塞的是 worker）。麻烦的形状是"续体跑在调用方
的事件循环上，而一个作用域对象在循环线程里析构"——那一刻循环线程进了一次驱动，
正是 §1.1 要挪走的东西。规则两条，写进头文件注释与 README：

1. 异步路径上的事务**必须**显式 `co_await tx.commit()` 或
   `co_await tx.rollback()`；析构里那次 rollback 只作失败兜底，**允许阻塞**。
2. `async::orm` 必须活过它交出去的所有操作，同"池必须活过所有未还的借用"
   （`include/uniorm/pool.hpp:10-11` 写过的那条规矩）；操作句柄只持 session
   的引用，不持池指针。`start()` 那一族没人盯着，所以这条改成库代守：
   `~orm()` 等到队列空（§3.4）。**事务句柄是这一条上唯一被松开的一环**：内层
   `uniorm::transaction` 握的是会话里那条连接的裸指针，于是会话析构先在连接还
   活着时把没闭合的句柄逐个回滚、再让连接走，句柄此后 `commit()` /
   `rollback()` 报的是"背后的会话没了"。这一环要一把门加一份弱句柄清单——正是
   本条初稿说"不需要"的那一族 machinery，它在句柄这一处被证明是需要的。

两处既有陷阱，一处在异步下变大、一处反而被队列压住，先记下：

- **size-1 池上重连会自锁。** `orm::connect()` 是在旧租约还握着的时候去求
  `registry.acquire()`，同一条 size-1 池上必然撞到 `acquire_timeout`
  （`src/pool.cpp:236-240`）。同步下它抛 `pool_timeout`；异步下这一趟挂在 worker
  上，等待期间那条 worker 就废了。修法（先放旧租约再取新的、失败时恢复）要和
  `tests/unit/test_identifier_resolution.cpp` 依赖的"连接失败保留旧租约"一起设
  计，不在本提案范围内动它；第一版把"重连前先 `co_await disconnect()`"写成要求。
- **在 worker 上等池会饿死 worker。** `acquire()` 阻塞等名额时，等待者自己也占一
  条 worker；池容量大于 worker 条数时这一点必然发生。§5 那条队列把同时想
  `acquire()` 的数量限在了 worker 条数以内（排队的还没占线程），所以这一格比初
  稿设想的小。第一版再靠 `acquire_timeout` 兜住（默认 5s，
  `include/uniorm/pool.hpp:35`），响得比吊死好。真要治好，得把 `pool.cpp` 的
  `condition_variable` 等待换成一张"可唤醒的等待者表"（存 `std::function` 而不
  是 `notify_all`），那是异步池自己的一步（§10 步骤 5）。

## 7. 编译与 ABI

### 7.1 编译器

`CMakeLists.txt:4-6` 已是 C++20 且 `CXX_EXTENSIONS OFF`。实测本机 GCC 11.5.0 不
需要 `-fcoroutines`（§1.2 末），ASan 下同过。Linux CI 腿跑 runner 自己的镜像，
那台 gcc 的版本还没量过——它比本机新，`<coroutine>` 只会更宽松。
`core-windows`（`/W4`）才是还没被看过的一格，按惯例先在 Windows 的 Visual
Studio 上编一次。两处要留意的不是标准库而是警告：MSVC 对协程的 `/W4` 诊断，和
`CMakeLists.txt:12-14` 那条 `/EHsc`（无它则栈展开跳过局部析构，而 §5 那一轮的
放行——含抛出路径——正靠析构）。若哪天要支持 GCC 10，加一次
`check_cxx_source_compiles` 探测再挂 `-fcoroutines`；今天三个编译器都不缺这个
开关，先不为它写分支。

### 7.2 ABI 影响

第一版按 D 的形状，`connection_iface`、`statement_iface`、`capabilities`、
`pool_options`、`entity_meta`、`schema_meta` **一个都不改**。新增只有导出类
（`async::orm`、`async::query<T>`、`async::transaction`、`task<T>`、
`executor`）、导出自由函数（`default_executor()`、`default_executor_size()`）、
`error.hpp` 里一个新的错误类型，以及一个新的安装头。
按 §5.1 的规矩，这不移动任何 vtable 槽、不改变任何已发布类型的 `sizeof`，**已
装的 `libuniorm_odbc.so` 不必重编**，`UNIORM_ABI_VERSION`
（`CMakeLists.txt:34` 算、`:94` 定义）不动，仍是 0.4.x 的一格功能加法。

把非模板实现挪进 `src/async/` 之后，库里新添的导出符号多了会话与句柄的那一圈
成员、`detail::operation_queue` 的五个函数与 `executor` 的虚析构：类型清单不
变、vtable 槽不变，版本号因此照旧不动。换掉的只是失败的样子——新头配旧库如今
在链接期就断，不再各自带着半份实现跑起来。

这条对比要写下来，因为它正是 §2 E 第二条理由的实际内容：上一轮往
`connection_iface` 插了一个 `open(connection_spec const&)` 虚槽
（`include/uniorm/backend/backend.hpp:192-195`），`UNIORM_ABI_VERSION` 本该进一
格，那一版按决定留在了 0.4.0（design.md §5.2 末尾记着这笔偏差）。异步若从契约
切，第一次就再欠一笔；从 `async::orm` 切，一笔不欠。

### 7.3 依赖与安装

`find_package(Threads)` 已经在（`CMakeLists.txt:29`），worker 就是
`std::thread`，不新增外部依赖，延续"不引 Boost"那条决策。新头
`include/uniorm/async.hpp`（装 `task` / `executor` / `async::orm` /
`async::query` / `async::transaction`，即 §3.2 那六样）单独按需 include，**不进
`uniorm.hpp` 伞形头**：不用异步的消费者不该为协程付编译时间，这与模板密集代码留
头文件的既有做法一致。私有那四件落在 `src/async/`，按 `CMakeLists.txt:38-53` 的
写法进 `core` 的源列表、不进安装清单。`core` 腿那道把 `sql.h` 毒掉的守卫
（`.github/workflows/ci.yml:102-123`）继续成立——异步头链的是核心，不碰 ODBC。

## 8. 测试策略

分层按"哪一层不需要真库"来切，与本仓既有手法一致。

1. **`task` / `executor` 机制本身，零 I/O**：一个手泵 executor（测试自己
   `submit`、自己决定何时执行那个 `std::function`），钉住六件事——未被 drive 时
   不 submit、`block()` 与被 `co_await` 各 drive 一次、未 drive 的 task 析构不
   留副作用、结果与异常都穿过挂起点、`block()` 重放同一个 `exception_ptr`、
   `start()` 的 handler 拿到的是同一个 `exception_ptr`。这层不需要线程，全部确
   定性。**步骤 2 已落地**：这六条，加上"同一个 task 上第二次 drive 被拒"，写在
   `tests/unit/test_async_task.cpp` 里；`exception_ptr` 那两条按"两次 rethrow 落
   在同一个地址"来钉——按地址比才证得出"没有另包一层"。那两个 executor 缝本身在
   `tests/unit/async_executors.hpp`，第 2 层用同一份。装 `<uniorm/async.hpp>`
   的那段还进了安装后冒烟的消费方（`tests/install/main.cpp`），它自带一个
   executor 实现并把链式 await 交给默认池跑，于是"外人只拿装好的树也能编译并跑
   通协程"这一条不再靠推断。同一段现在也立起一个 `async::orm` 会话，在没有连
   接时 drive 一次 `execute_update`——它的函数体在 `src/async/async.cpp` 里，冒烟
   落到"无连接"那个 throw 就说明会话面的导出符号对一个外人也是解得开的。
2. **假 scheme 接管异步**：沿用 `tests/unit/test_backend_registry.cpp` 与
   `tests/unit/test_pool.cpp` 里"注册一个测试 scheme + 记录型 connection"的现成
   手法，把 `async::orm` 接到假 backend 上、`run_on` 挂到手泵 executor 上，钉
   住：FIFO 按提交顺序跑（`start()` 与 await 混着发也保先后）、在飞的最多一个、
   超过 `submit_limit` 在调用点抛 `submit_overflow`、`sync()` 改过的注册表 task
   那一侧立刻看得见（钉住"只有一份状态宿主"）——手泵下 `submit` 次数与它的先后
   就是可数的证据。这几条都不需要线程。`unit_tests` 目标不链 ODBC，于是这两层
   在 `core` 与 `core-windows` 两条腿上都在跑。**步骤 3 已落地**：三十四项用例在
   `tests/unit/test_async_orm.cpp`（步骤 3 的十项加步骤 4 的五项，另加 §3 那两条
   改过的入口，再加上屏障那三项——一项钉 `wait_all_operations()` 把交出的与
   回调的都等齐，一项钉被 `submit_limit` 拒的那次不在等里，一项钉一条链夹
   在两次请求之间时手里没有回合——再加第二路那八项，末了是 review 之后补的
   七项：`submit_limit` 设成 0 就当 1 用、会话从四件套与从一把同步对象各立一次、
   同一批行从两个面写出去落到假连接上的语句逐字相同、句柄活过会话而 `commit()`
   报的是会话没了、`start()` 的 handler 抛出不外溢、空的 `on_done` 在调用点被
   拒、一条链在 `co_await` 处接到被拒的回合），假 scheme
   记下"哪条语句按什么顺序落到了连接上"，于是先后与"在飞的最多一个"是数出来的
   ——三份请求都 drive 过之后手泵里只有一份可跑，这一条没有队列写不出来。上限
   那项钉两件事：`submit_overflow` 在调用点响，且被拒的那个 task 之后还能
   drive（被拒不等于被消费）。批量那项钉"一次扫描是一个操作"——两个块、一次交
   接。§8 末那条 `validate()` 负面用例也在这层；用到真 worker 的是 `~orm()`
   那两句等待与屏障等交接那项，摘掉屏障它们就红。这一层现在也从
   `async::orm(连接串)` 直接起会话，`uniorm::orm` 不在用例里出现：转发出的
   `map` 与三个旋钮由 `sync()` 读回同一个值，`sync()` 改过的注册表 task 那一侧
   立刻看得见，两个方向都钉，所以"欠一处同步"那笔账一漏就红。步骤 4 那五项里一条钉
   `resume_on`：一个手泵跑、另一个手泵接，续体落在后者的计数上，于是一条链跨了
   几条线程是可数的，不必猜。另四条钉 `begin()` 交出的句柄：协程里开、经同一把
   会话提交、`active()` 转假；`commit()` 排在它要收尾的那次写之后（先清掉池借
   出时那次 autocommit 的账，剩下的先后才是这层自己的）；没提交就出作用域的那
   个回滚；以及兜底回滚先把会话等安静——去掉 `~transaction` 里那句等待，这条连
   着 `~orm()` 那条一起红。§3.5 那条第二路也在这层，八项：`on_done` 收到真值与
   空 `exception_ptr`（`bad` 先埋一枚活的异常，回调没响就红）；失败时第一个参数
   是 `R{}` 而第二个与 body 抛出的是同一个对象（仍按两次 rethrow 的地址比）；回
   调形的读排在一条 detach 的写之后，而且它只占一格（`queued()==1` 与
   `pending()==2` 一起看，多占一格的写法在这里就红）；`submit_limit` 满时从调用
   点抛 `submit_overflow`，那条回调不响；executor 拒收这一格时回调照样响（body
   从没跑过，唯一的出口是请求自带的 handler）；`on_done` 自己抛出不外溢也不影响
   下一次请求；析构屏障那项要真 worker——回调挂在一枚二进制信号量上等测试放行，
   放行后才置标志，`~orm()` 返回时标志必须已经置上。它的对照做在库那一侧：摘掉
   `~orm()` 里的 `wait_idle()`，
   这条连同 route 1 的析构项一起红，标志在析构返回时还是假的——答案响在 body 里，
   而 `finish()` 在 body 之后，所以屏障不需要第二枚计数。
3. **真库一条腿一段**：现有三条驱动腿的集成用例里各加一段 `co_await`，读同一个
   夹具（一次批量 insert + `all` + `one` + `execute_update` + 一次
   `query<T>(sql, params)`），断言与同步段读回同一份数据。**不新增 CI 腿，不动
   golden**；异步不改变发出去的 SQL 一个字节，这一点由 golden 不动来自证。**已落
   地**：`test_async_reads`（`tests/integration/test_integration.cpp`）里那五个
   终端各 drive 一次，每一个异步读回的行都与同步读回按字段比一遍；对不上的只有
   本会话 `execute_update` 之前那一次 `all`，用例就把那一处钉在名字上。集成用
   例是一份二进制跑三条腿，这一段因此每条腿都跑；本机已在 MariaDB 10.6.4 上跑
   绿，另两格等的还是 CI。§3.5 那条第二路也在这段里读一次：同一个夹具、同一句
   SQL，从回调形拿回来的行与同步读回逐字段比，于是"两个面交给队列的是同一个工作
   函数"这句不只由源码相邻性自证，也到了活库；那一次读不回来看任何泵，它等的就是
   析构那一下。第二路在这段里还多跑了一串（四个回调形名字按先后排在一起：insert、
   读、update、一次撞主键的 insert、再读、remove——撞键那次第一个参数是 `0` 而第二个
   非空，队列在它之后照样答得出），第一路也补了失败的四条出口（`block()` 抛出、
   协程 body 自己接住、不接就穿过 drive 动作、`start()` 的 handler 与回调的第二个
   参数各递一遍，文本与同步那一抛的 `what()` 整串相等）、`async::transaction` 的三种
   收场、一次从空会话起步的 `connect()`/`disconnect()`，以及两条会话两个线程各写各的
   （每次读只数自己那段的 id，钉的是库最终装着四行，不是两者怎样交错）。活库上这些
   断言不是空转，另有一份一次性探针在库上直接数过：提交那行在、显式回滚那行不在、
   什么都没写的句柄出作用域后那行也不在、成功那次 `start()` 的 handler 没响而写入照样
   在。集成用例是一份二进制跑三条腿，这些段因此每条腿都跑；本机已在 MariaDB 10.6.4
   上跑绿，另两格等的还是 CI。
4. **只有真 worker 才现身的两样**另开：默认池"worker 条数即并发上限"这句核心
   承诺（手泵证不了它），与跨线程交接——`resume_on` 是否真回到指定线程、§5/§6
   的时序。做法是把第 2 层那批用例换成默认 executor 再跑一遍，挂在一条
   `-fsanitize=thread` 的一次性构建上；它是本地一次验证，不进流水线
   （`-fsanitize` 目前不在任何一条腿上）。**步骤 4 抽走了头一样的一半**：
   `resume_on` 是否落到点名的 executor，两个手泵各数自己的 `submit` 就能钉死，不
   必等 TSan 构建；这一层因此只剩"默认池那条上限本身"与时序那两样。
   **已跑过**：本机 GCC 11 的 `-fsanitize=thread` 链不起来——它的 `libtsan.so`
   是一个 38 字节的桩，指向缺文件的 `/usr/lib64/libtsan.so.0.0.0`；换 clang
   21.1.8 才起得来。clang 不接受 `row.hpp:45` 那个泛型 lambda：`To` 是
   `double` 时 `std::make_unsigned_t<To>` 落在被丢弃的 `if constexpr` 分支里
   仍被实例化，libstdc++ 15 对此只留一个未定义的
   `__make_unsigned_selector<double>`；GCC 11 与 MSVC 都不报，CI 三条腿也没有
   clang，所以这是本地工具的口径，不是缺陷。跑的是不带 ODBC 的那份核心构建，
   `test_row.cpp` 一个文件抽掉，其余原样。压测两段在本地树里另写：4 个会话各
   12 条 detached，`~orm()` 收干后每份 `executed` 与提交顺序逐条相等，在飞峰
   值 4、请求的 worker 数 8；另 4 个线程各自 `block()` 排在自家那条在飞写之
   后，收干后各 9 条。10 遍 0 报告；同一次构建里塞一个故意未同步的计数器，
   TSan 立刻报，所以那个 0 是量出来的，不是空跑。那两段压测只写在本地树里，没
   进仓库：它们要给 fake backend 加"在飞数"这一格，第 2 层那批用例用不上它。

一条负面用例要有：`validate()` 在 `async::orm` 上仍然是同步的（§4.3 第 2 条）。
用例把它调在协程里并断言结果，钉住"这是我们承认的阻塞点，不是遗漏"。

## 9. 明确不解决

1. **不省线程**（§1.1）。并发上限 = worker 条数，与今天的"= 请求线程数"同一量
   级。真省要原生异步，而原生异步第一格在 libpq：`PQsendQuery` + 非阻塞 socket
   就绪，那时要加的是"socket 等"这一族 executor 能力与 backend 契约槽位——§2 E
   那两条理由届时都不成立，因为终于有了第一个读者。
2. **没有取消。** 操作跑在 worker 上，线程此刻在驱动内部；`SQLCancel` 要求对同
   一条 stmt 句柄做跨线程调用，而 §1.2 第 4 条已经说明这套 DM 连异步收尾的入口
   都没有，取消的语义无法承诺。第一版只有"发出去就发到底"。超时只在池借用那一
   格有（`acquire_timeout`），语句级超时不在本提案内（它要在属性侧加新东西）。
   §5 那条队列顺手开了一个便宜的子集——还没轮到的那一半本就可以摘掉——但这一条
   仍算不解决，因为真正要取消的从来是已经在飞的那一个。
3. **没有异步流式游标**（§4.3 第 1 条）。
4. **不引入 sender / receiver（P2300）。** C++20 没有它，三个编译器也都不现成
   可用；`task<T>` + `executor` 是让 `co_await` 编得过的最小形状，不为一个还没
   进标准的抽象预留第二套表面。
5. **不动 backend 契约、不动目录接口、不改 `orm` 的既有签名**（§7.2）。
6. **不做 `async_pool` 这种第二套池。** 池仍是那一个 `connection_pool`，异步只
   是把它的 `acquire()` 搬进 worker（§6 的饿死风险即由此来）。批量 detach 曾经
   像是这一格的第一个读者，现在不是了：§5 的队列把它接住了，代价是串行而不是
   加并发——真要"一批写各跑各的"，等的仍是 worker 条数与池容量，那是原生异步的
   事（§9 第 1 条）。
7. **驱动动作只有 `block()` 与 `start()` 两个，没有组合器。** 一次挂起里同时发
   好几个操作要靠多把 `orm`（§3.1），或在它们各自 `start()` 之后再自己汇合——
   `task` 上没有 `then()` / `when_all` / `await_single` 那一族。C++20 给不出
   `when_all` 的标准形状（§9 第 4 条），先造一个就是替调用方决定了他没要的同步
   语义。失败传播同理：`start()` 的 handler 里只拿到一个 `exception_ptr`，库不
   替它落地。§3.5 那条第二路也不算组合器：它一次交付一个操作的结果，两个操作要
   接起来仍得回协程，或者发两次回调各自收。

## 10. 分步

1. **前置收敛（无新 API）**：块取物化循环并成一处；构建器交出 `(sql, bound)`
   （`render_select` / `render_count` 的公开路径），链式部分抽进基类。同步行为与
   golden 逐字节不变，靠现有三条腿证明。约 120–180 行含测试。
2. **`task<T>` + `executor` + 默认 worker 池**（§3.2 私有侧第 1、2 件）：惰性
   提交与 `block()` / `start()` 两个 drive 入口都在这一步，只有机制与 §8 第 1 层
   用例，不接任何数据库对象。约 250–350 行。
3. **`async::orm` 第一版**：`connect` / `disconnect` / `execute_update` / 实体三
   类批量 / `query` 两式 / `commit` / `rollback`，加 `run_on`、§5 那条 FIFO 与
   `submit_limit`（新错误 `submit_overflow`）、`pending_` 的 drain；
   `async::query<T>` 的 `all` / `one` / `count`；§8 第 2 层用例。约 250 行。
4. **`begin()` → `async::transaction`、`resume_on`、README 中英两侧**，并把本
   提案落地处改进 design.md（§11）。
5. **被本提案点名、但不做在它里面的两件事**：池的 awaitable 等待者表（替掉
   `condition_variable`，§6）与 `orm::connect()` 的旧租约顺序（§6）。两件各是一
   个独立步骤，都要带上"同步路径行为不变"的证据。
6. **采纳之后回过头开的第二路（§3.5）**：`async::orm` 上五个回调形重载，四个操作
   各一枚共用的工作函数。它不在这份计划的原始五步里，因为它要解决的不是"异步还
   缺什么机制"，而是"一次调用一个人的场景凭什么要背 task 的规则"。原来还在这一步
   里备了一枚 `operation_queue` 的 `held_` 计数，落地时撤了：答案搬进 body 之后
   `in_flight_` 已经把它算在等待范围内，第二枚计数不提供任何保证。

每步单独提交、单独可评审；除步骤 1 外每步都不动 golden，也不动 backend 契约。

**步骤 1–4 已落地，步骤 5 没有，步骤 6 也已落地。** 步骤 1 的那两处收敛就是
`include/uniorm/detail/fetch_rows.hpp` 与构建器交出 `statement` 的
`render_select` / `render_count`（`include/uniorm/builder/builder.hpp:133`、
`:164`），同步侧的行为与 golden 由那三条腿自证；步骤 2 与 3 的落地情况写在 §8 第
1、2 层那两段加粗的注里；步骤 4 的形状在 design.md §4.11，README 中英两侧各多一
段"异步表面"，§11 那七条逐条对号。步骤 6 的形状在 §3.5，用例是 §8 第 2 层那七项
加集成段里那一次回调形读。步骤 5 那两件各是一个独立步骤，本提案没替它们排期。

## 11. 若采纳，文档怎么改

**下面八条已随步骤 4 逐条落地**（`design.md` 那几处分别在 Out of Scope 那行、
基础决策"接口风格"那格、§4.1、§5.2 末尾与新写的 §4.11；§3 目录树补了
`async.hpp` 与 `src/async/`，§8 测试清单补了两层用例和那条 `validate()` 负面
用例；§9 第 4 条改成指向 §4.11 与步骤 5 那两件；README 中英两侧各多一段"异步
表面"，五条承诺同词）。本节留着，是给"哪些形状从提案搬进了设计文档"对一张号。

- **已改** Out of Scope 那行（§1）删掉"异步 API"半句，换成"异步流式游标与原生
  异步 backend（本提案 §4.3、§9 第 1 条）"；
- **已改** 基础决策那格（§1 末）"接口风格 纯同步"改为"同步为主，异步是可选表面
  （`async.hpp`，§4.11）；两者共用同一条 backend 契约，异步侧不新增数据库通路"；
- **已改** §4.1"刻意不设 `SQL_ATTR_ASYNC_ENABLE`（无异步）"补上 §1.2 的实测数
  字，把"无异步"的依据从规范假设换成量过的值（与本仓其余"不是推的"同口径）；
- **已加** §4.11：落地形状整段写在那里，六个公开件各带行号——声明在
  `async.hpp`，函数体在 `src/async/` 那两个 .cpp；
- **已改** §5.2 记下"异步第一版不进 backend 契约"这条边界与它的两条理由；
- **已改** §9 第 4 条从"异步包装层（libpq backend 可用原生异步）"改成指向
  §4.11 与它留给 §10 步骤 5 的那两件事；
- **已改** §3 目录树收下 `include/uniorm/async.hpp` 与 `src/async/` 那一格；§8
  测试清单收下第 1、2 层用例与那条 `validate()` 负面用例，另在安装冒烟那条记下消
  费者工程里 drive 会话面的那一段；
- **已写** README 两侧各一段"异步表面"：worker 数即并发上限、交出去的失败只有一
  个出口、同一把会话上排队不等于并行（§5）、续体默认换线程、目录读 / 析构兜底 /
  `~orm()` 的 drain 是三个被承认的阻塞点。
