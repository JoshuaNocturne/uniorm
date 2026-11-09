# 代码写法

- **变量名要读出它是什么，尽量少出现单字母名字。** 循环计数器、下标、临时列名也写成
  `index` / `slot` / `column` 这一类，而不是 `i` / `j` / `c`。模板形参名是例外：`T`、
  `M` 这一类照惯例保留。被改动的那几行里已有的单字母变量顺手换掉，不为一件小事整仓
  扫一遍。
- **改完代码要用 `clang-format` 格式化。** 流水线不校验格式，但格式以它为准：按仓库根的
  `.clang-format`（`ColumnLimit: 80`、两格缩进）跑 `clang-format -i <改过的文件>`。
  整仓已按 `.clang-format` 对齐过，日常跑它只会动改过的行；formatter 版本与仓库对齐时
  不一致而带出无关重排的，那种重排别夹进功能提交——另起一条 `style:`，或先只跑改动行段
  （`--lines=<起>:<止>`），由用户定。全仓一次性对齐格式是单独一件事，要在动手前跟用户
  确认。
- **注释默认不写。** 只在原因不显然时写一行：隐藏的约束、微妙的不变量、某个 bug 的绕法。
  不解释代码在做什么，也不写"用于 X 流程""为 issue#123 添加"这类会随代码腐烂的话；
  一行说得完的不要写成多段。

# 构建与测试

- 构建树按 backend 各留一棵就够，别为一次改动新建第 N 棵，也别动不属于本次任务的树。
  一棵把三家 backend（ODBC、Oracle、libpq）全开，日常与集成腿都在那儿跑；需要只针对
  libpq 的量测时另留一棵只开 libpq。
- 日常一跑：`cmake --build <tree>`，`ctest --test-dir <tree> -E "integration|perf"`。
  连不上库的腿自己报 SKIP，不是失败。
- 集成腿要 `UNIORM_IT_DSN`、`UNIORM_IT_USER`、`UNIORM_IT_PWD` 三个都设上；Oracle 与
  libpq 那两条另按 `UNIORM_IT_BACKEND` 选服务端，target 分别读
  `host[:port]/service` 与 `host[:port]/dbname`。
- perf 腿按 `UNIORM_PERF_ROWS` 发行数（默认 10000），`ctest -LE perf` 跳过。ctest 已给
  每个测试设好 `UNIORM_PLUGIN_PATH`，所以经 ctest 跑不必管 plugin 在哪。
- 独立跑测试程序或探针时才需要 `UNIORM_PLUGIN_PATH`：它只指**一个目录**，一条腿同时要
  odbc 与 postgresql 就得把两个 backend 库拷进同一目录再指过去，否则报
  `unknown_scheme`。

# 测量

- 输出重定向到文件是块缓冲的：量测与取表格一律让 stdout 不缓冲（`stdbuf -o0 -e0` 或
  等效手段），否则外层 timeout 一杀，跑了很久的结果一个字都没落盘。
- 耗时数字只在没有旁路观察者的那一轮作数；带 watcher 的那几轮是用来认形状的，不是用来
  计时的。反复取最小值（best of N），首轮常带一次性开销。

# 连库与凭据

- 数据库凭据只在本机的客户端配置里（ODBC 的用户侧 DSN 文件那一类），不在仓库里。需要
  它时读进 shell 变量再用：任何时候都不打印、不写进仓库、不贴进 commit 正文或文档。
- 探针与临时程序**不要自己解析那个配置文件**——程序里只写目标 DSN 名，UID 与 PWD 让
  unixODBC 从 DSN 补；非 libpq 不可的场合，由 shell 把口令交进环境变量再拼 conninfo。
- 只拿项目自建的试验库做试验；名字看着像生产库的 DSN 一律不碰。与用户会话共用的实例
  上：绝不 kill 服务端会话，也不留长期对象——探针造的表用完就 `DROP`。
- 探针程序和它的产物放在仓库之外（系统临时目录），别在仓库里造文件。

# 提交

- 提交由用户发起。没被明说就不要 commit，更不要 push；一次历史请求不是长期许可。
- 被要求提交时按功能切开（backend / 测试 / 文档各自成一条），别把别人已有的、与本次
  改动无关的工作区内容混进自己的提交里。
- subject 用英文小写命令式，`feat:` / `fix:` / `test:` / `perf:` / `docs:` 起头；正文写
  为什么这样改、原来的形状错在哪、实测的数字，每行不超过 80 列。

# 文档同步

- 行为变了要同时落三处：`docs/design.md` 记设计，`README.md` 与 `README.zh.md` 各一份且
  条目对得上；只改代码不改记录，就是把过时的说法留给下一个人。
- 中文记录写"现在怎么做"，不写"这次改了什么"——后者属于 commit 正文，会随代码腐烂。
