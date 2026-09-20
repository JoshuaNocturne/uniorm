# 提案：标识符大小写策略——让一次生成的映射跨服务端可用

状态：**B 与 C 已落地**（§10 步骤 1、2，含 §5 那处前置修复），A 拒，D 仍是条件触发的
草案。本文描述的是设计与取舍，其中标了行号的地方是提案起草时的形状，改动落地后不必
然对得上；已交付的部分以 `docs/design.md` 为准（§4.8 拼法策略与它的持有者、§5.2 目录
按名字问不按 pattern 问、§6.2/§6.3/§6.4 生成器侧、§9 第 11 条给 D 留的位置、§8 测试
清单）。§12 的三处沉默也已全部落地。

## 1. 要解决的问题

`uniorm-gen` 产出的头文件在服务端之间能走多远，只被一件事挡住：它写死的 SQL
标识符字符串。生成物里没有 SQL、没有引号与分页规则（那些在查询时由连上的服务端
banner 决定，`src/dialect.cpp:42-53`），也没有 catalog/schema 名（只有表名，
`tools/uniorm-gen/generator.cpp:250`），但表名列名是**逐字抄生成时那张目录**的
（`generator.cpp:250`、`:255`）。运行时又无条件加引号
（`src/dialect.cpp:7-14`），于是这些字符串必须与目标服务端的存储拼法逐字符相同；
`find_column` 也是精确比较（`include/uniorm/schema.hpp:34-42`）。

结果是：从 Oracle 生成的头去连 MySQL，`USERS` / `USER_ID` 会以带引号的原样进
SQL，MySQL（Linux，`lower_case_table_names=0`）认不出 `USERS`，`validate()` 抛
`table not found: USERS`（`src/orm.cpp:91-93`）。方向反过来同样断，而且不必等到
Oracle：MySQL 上 `CREATE TABLE UserAccounts`（`lower_case_table_names=0`）目录里就
是 `UserAccounts`，同一条 DDL 在 PostgreSQL 被折成 `useraccounts`——一份头拿不动这
两家。

四条相关事实决定了方案的形状：

1. **各家在 CREATE 时的折叠规则不同，而且不是一条能对齐的规则。** Oracle 把未加引
   号的名字折成大写，PostgreSQL 折成小写，MySQL 表名看 `lower_case_table_names` 取
   值而列名不区分大小写。任何"生成期统一改成某一种拼法"只能蒙对一家。
2. **多数 schema 的拼法是统一的**（整库小写是默认习惯），所以"这一家的存储是大写
   还是小写"是一个部署级的事实，不是逐列的事实。
3. **结果集一侧不受影响。** 实体物化按序号绑定，不按名字
   （`include/uniorm/builder/builder.hpp:32-53`），所以只有出口的标识符耦合。
4. **本仓已经用约定解决过一次，也已经在用不敏感匹配挑名字。** 一份 golden 供三家
   跑绿，前提是夹具表三家的目录拼法相同（README「Shipped」）；而生成器挑表时先精确
   比、不中再按 ASCII 折叠比（`tools/uniorm-gen/schema_reader.cpp:44-67`，注释正是
   为 `lower_case_table_names` 写的）。后者已经是"声明名与目录名可以只差大小写"这一
   事实的承认，只是承认在生成期而不是运行期。

顺带一笔同类耦合，在生成器一侧：`[types]` 的键入库时折成大写、与列的 `type_name`
折后相比，因此大小写不敏感（`tools/uniorm-gen/config.cpp:164`，比较点
`generator.cpp:100-106`）；而 `[tables.NAME]` 与 `[tables.NAME.columns.COLUMN]`
原样入库（`config.cpp:158`、`:184`），与目录名精确 `find`（`generator.cpp:146`、
`:158`）。同一个配置文件里并存两套规则，跨家时后一套要按那家的拼法重写。（此段记
的是提案起草时的形状：两套现已并成一套，两侧都折叠，见 §12 第 2 条。）

## 2. 四个备选

### A. 生成期归一（`case_style = keep | lower | upper` 进配置文件）

在 `generator.cpp` 出标识符处套一层折叠。改动最小（十几行），但它把"猜目标服务端
的存储拼法"写进产物，而产物只有一份：MySQL Linux 默认下真表名是 `UserAccounts` 时，
`lower` 与 `upper` 都错，只有 `keep` 可能对——而 `keep` 就是现状。**拒**。注意下面
的 C 逃开了这条反驳：C 把同一个选择在连接建立之后做，那时目标已知。

A 与 C 组合（生成期一律写小写，运行期再由 `identifier_case` 决定发什么）也一并拒，
理由是可达集变小而不是变大：折叠放在出口，`keep | lower | upper` 三值作用在
verbatim 名上已经覆盖三种拼法；先降为小写，`keep` 与 `lower` 合成同一个值，唯一真正
丢掉的是 verbatim 那一档，也就是"源目录逐名拼法"这份记录本身。于是三处退化：

- 同库混拼法（`users` 与 `UserAccounts` 并存，MySQL
  `lower_case_table_names=0` 允许）原本 `keep` 就能两家通吃，降为小写后 `lower` 与
  `upper` 各错一半，运行期无法恢复。
- 只与大小写同名的两张表（`USER_ACCOUNTS` / `user_accounts`）在生成期折成同一个类名，
  现状是产物重名、消费者编译期响；降为小写后连 SQL 名也相同，两次
  `registry.map<T>("user_accounts")` 绑到同一张表，失败从编译错误降级成静默并表。
- 默认值反了：大写服务端（Oracle 的 `USERS`）现在 `keep` 即正确，降为小写后必须记得
  声明 `upper` 才对，等于把一次无声的破坏性变更放进已发布语义。

生成期要改的只有诊断口径，不改标识符拼法：见 §12。

### B. 约定 + 诊断（不改运行期行为）

约定是主答案的一半：跨服务端共享的项目按固定拼法写 DDL（本仓夹具已经这么做）。B 要
补的是另一半——让失败可读。`validate()` 已经读了 live shape 且手上就有全部候选名
（`src/orm.cpp:88-119`），只差把"找不到"换成"此库里是 `UserAccounts`，与声明的
`useraccounts` 仅大小写不同"。**这是任何后续方案的第一步**，因为把用户从一个策略指
到另一个策略靠的就是这句话；它自己不改变任何成功或失败的结果。

### C. 用户声明的运行期策略（把大小交给用户）

`keep`（默认，即现状）/ `lower` / `upper`，按部署声明一次，发 SQL 时应用。理由即 §1
第 2 条：拼法在绝大多数 schema 上是库级事实，因此它可以被声明，而不必被逐列解析。
是本文推荐的承接方案。

### D. 运行期延迟绑定（对活目录逐名解析）

不要求用户知道目标拼法，而是去问目录。正确性来自"目录即事实"，代价是需要保留声明名
并逐名匹配。降级为**条件触发**：当某个部署的 schema 拼法不统一（同库里既有
`UserAccounts` 又有 `users`），C 无能为力而 D 可以。设计留在 §6。

## 3. 为什么 C 盖住了 D 的大半

B 与 C 组成一个闭环，而 D 是这个闭环的自动化版本：B 告诉用户"库里是 `USERS`"，用户
据此声明 `upper`，C 就把它发出去。D 省掉的只是中间那句声明——它值多少，取决于有多少
部署的拼法不统一。按 §1 第 2 条，今天的答案是"不知道，因为只有一个库系"，而按代码
量，D 是 C 的五倍上下（§4 与 §6 之差）。因此顺序是 B → C，D 留条件。

C 另有一处比 D 强的地方：策略落在标识符出口（§4.1），于是连没有映射可声明的路径也
一并覆盖——动态 `orm::update("T_X")` / `remove("T_X")` 的表名与 `set()` 的列名都经
`quote_identifier`（`src/orm.cpp:168-199`）。D 以"声明名可与目录比对"为前提，那些路
径天然在它的范围外。两者都不碰手写 SQL 文本：动态 `where` 子句与 `orm::execute()` 的
语句是直接拼进去的（`src/orm.cpp:178`、`:199`），那里没有任何机制能插手。

## 4. C 的设计

### 4.1 出口只有一个：`quote_identifier`

11 处构造方言的地方（`src/orm.cpp:167/197/404/414/425/456/485/643/693/733`，
`src/builder/builder.cpp:7`）全都只为拿到一个 `dialect`，而实体路径与动态构建器
发出的每一个标识符都已经过 `dialect::quote_identifier`，出口唯一；那些调用点在
`src/orm.cpp:292-297`、`src/orm.cpp:363-390` 与
`include/uniorm/builder/builder.hpp:269-352`（手写 SQL 文本除外，§3）。所以折叠
放进那一个函数即可，发标识符的站点一行不改；但新字段要有地方被赋值，策略得先落到那
11 份 `dialect` 上，这要求构造处唯一（§4.2）。`dialect` 是三个公开字段的值类型
（`include/uniorm/dialect.hpp:14-23`），加一个 `identifier_case` 字段
（枚举 `keep | lower | upper`，默认 `keep`）就带过了全部路径。

折叠只按 ASCII，不走 locale 与 `char_traits`：同一个名字因进程 locale 不同而指向不
同的表，是比大小写更糟的失效。这也与 `schema_reader.cpp:44-67` 既有的折叠同一口径。

### 4.2 谁持有策略

策略是部署级事实，而部署由 `connection` 表示；`dialect` 的两个输入（banner 与策略）
也都在 `connection` 上。所以：

- `connection` 持有一个 `identifier_case`，并暴露 `sql_dialect()`——把 banner 探测与
  策略合在一处、结果缓存；11 个站点改为取它，`src/builder/builder.cpp:5-11` 自己那
  份探测缓存随之消失（顺带的收敛，不是新抽象）。
- 入口是程序化的：`orm` 与 `connection` 上各一个 setter（构造后、连接上设置，跨
  `connect()` 保留在 `orm` 侧并在租到连接时转发）。
- **不先从 DSN 解析。** 连接串里多出来的键会连同驱动关键字一起交出去，为一个枚举引
  入一套自己的转义与冲突规则不值。

默认 `keep` 意味着不认识这套的人行为完全不变。

### 4.3 `validate()` 必须校同一个名字

策略若只作用于 SQL，`validate()` 就会拿未折叠的声明名去查目录（`src/orm.cpp:91`
传的是 `meta.table` 原样），于是查询能成、校验反抛错——一个自相矛盾的组合。规则写
死：**`validate()` 校验的是查询即将发出的那个名字**，即先按策略折叠再查目录；查不到
时，B 的诊断按折叠后的目录侧报告候选。策略正确时校验随之通过，策略错时两边一起响。

**落地后仍剩一格**：目录那一问走 §5 的折叠匹配，所以表名只差大小写时校验会放过，而
发出去的 SQL 引的是原样或折叠后的名字——两边是否一致由服务器的比对规则决定。列名一
侧是精确比，`keep` 与服务器不合在那里才真响。文档因此不写"校验收的就是查询会发出去
的那个名字"，只写 `validate()` 按同一策略问目录、落空时报出目录侧的拼法。

### 4.4 不在范围内

B、C 都不改 DDL，也不生成任何迁移脚本：它们只让"已经存在的两套拼法"共用一份映射。

## 5. 一处前置修复（B 与 D 需要，C 不需要）

`SQLColumns` 的表名参数按 ODBC 是 pattern value，`_` 与 `%` 是通配符，而
`src/odbc/schema_catalog.cpp:119-128` 原样传入（列名传 `nullptr`，即该表全部列）。
`md.shape()` 把返回行串成 `table_shape`，于是声明 `user_id` 这张表的读取可以顺带命中
`userXid` 的列，把两张表的列约进同一个 shape；`tables(catalog, schema)` 的两个参数同
理（`schema_catalog.cpp:76-83`）。现状已在文档里承认了合并（`table_ref` 留空
catalog/schema 时跨 schema 约成一个，README:230-234），pattern 让它在同 schema 内也
成立。修法：拿回的行按名字再筛一遍——不是 `==`，而是**只差大小写算同一个名字**（折叠
是运行期策略的事，见 §4.2 的 C），驱动把名字报成空、或那一项本就留空不限定也留着，
那是驱动的沉默不是别人的表。不开 `SQL_ATTR_METADATA_ID`（各家驱动对它行为不一）。
判据是一个纯函数，挂在 `test_odbc_catalog` 里钉住，不需要真库。这是独立的正确性修
复；B 要列候选、D 要逐名解析，都建立在"目录行就是我要那张表"上，所以是它们的前提。C
不读目录，不依赖它。**已落地**：`catalog_name_matches()`（`schema_catalog.hpp`）用在
五处读取上，契约同时写进 `schema_meta` 的注释（design.md §5.2）。筛的是每行行首那三列
（catalog、schema、table），`SQLForeignKeys` 的外键侧那一组偏到第 5 到 7 列，五处
读取共用一个 `reported_names`；由此 `uniorm-gen` 的 `--catalog` / `--schema` 不再是
pattern，值里带 `%` 会在读取之前出一条告警说明它不参与匹配。

## 6. D 的设计（留作条件触发）

四条规则，对一个声明名与其候选集合：精确命中优先；否则 ASCII 折叠比较，唯一命中才采
纳该候选的活拼法；折叠命中多于一个 → `mapping_error` 列出全部候选，绝不代挑；无命中
→ B 的消息。

结构代价只有一处：解析是就地改写 `entity_meta`（`include/uniorm/orm.hpp:284` 的
`entities_` 是每 `orm` 私有，`map<T>()` 写入，`:71-80`），因此第二次解析（换服务端重
连）不能再拿第一次的产物当输入，得另存 `declared_table` / `declared_column` 两份原始
名。SQL 读活拼法，诊断与再解析读声明名。时机与线程：解析只在 `validate()`（连接后、
查询前）发生，不引入锁。

## 7. 测试策略

不新增 CI 腿，也不动 golden：CI 没有 Oracle，而跨家的拼法差异与三条同族腿的夹具约定
（三家都认的写法）本来就互斥——夹具一旦混用大小写，一份 golden 就不再供三家。

- **C 的折叠是纯函数**：`(name, policy) → name`，单测直接钉 `quote_identifier` 与
  ASCII-only，不需要连接，挂 `unit_tests`（不链 ODBC 的目标）。**已落地**，另外
  B 的每条消息也逐字钉在同一目标里：`test_orm_validate` 自造一份 `schema_meta`，
  同一份映射与同一份目录只换策略，就在成败两侧来回。
- **C 的策略 + B 的诊断**要一次真库验证：任一腿建一张只与声明差大小写的表（例如活库
  `useraccounts` 对声明 `USERACCOUNTS`），`keep` 下 `validate()` 抛且消息点名候选、
  声明 `lower` 后通过。**先按更小的形状落过一次**：不建新表、不动夹具，只在已有那条
  腿的构建器用例末尾拿 `build_select()` 的文本重发一次——`upper` 下表名跟着升上去、
  原来那条小写断言反过来不成立，验的是"出口只有一处"，不执行任何语句。"声明错了会
  响"那一半现在也落了，就落在三条腿共用的那个集成用例里，只是要真建一张表：建
  `UNIORM_IT_CASE_USER` 的 DDL 不加引号，于是每家按它折叠任何名字的方式存下它。用例
  先读目录拿到存下的那一侧，把表名与两个列名都声明在相反的一侧，然后三件事：
  `keep` 下 `validate()` 必须落空——这一条不分腿成立，因为表名一侧被 §5 的大小写匹
  配放过时，逐字比的列名一侧（§4.3）必响；落空的消息按先响的那一侧核对，目录分得开两
  种拼法就断表名候选，分不开就断列候选点名报表那一侧；最后把策略折过去，断
  `validate()` 通过且实体读回那行数据。策略是两侧各自推一次再要求相等的：表名折法与
  事实，而一份策略要同时救整份映射，正是本提案的前提，`lower_case_table_names=1` 那种
  表折小写而列名照建表保留的配法就会当场把它撞开，而不是让假红炸进 `validate()`。每格
  的实际拼法无条件记一行 `note:`，于是 psqlODBC 折不折 pattern、那台服务端是哪值，第
  一次跑就在 CI 日志里可见——这两格此前都没量过。这条不进 golden 比对，只断言成败；
  `gen_e2e` 另用自己的夹具，多出的这张表动不到它。
- **D 若落地**才需要 fake 目录：`schema_meta` 是 public 抽象接口
  （`include/uniorm/schema.hpp:44-105`，虚析构），单测造一份目录喂 `USERS`/`USER_ID`
  即可断言四条规则；但匹配要能这样测，就得把 §6 那四条写成一个只吃 `schema_meta&` 与
  `entity_meta&` 的自由函数——`connection` 除了往 scheme registry 注册测试 scheme 没
  有注入口（同法见 `tests/unit/test_backend_registry.cpp:57-59`）。
- 三条同族腿在 B 与 C 下都是恒等（`keep` 默认，除上一例那张刻意反向的表之外夹具全小
  写），golden 不动就是"不改变现有语义"的证据。

## 8. 明确不解决

语义不由大小写决定。成员类型按中立的 `sql_type` 选（`default_cpp_type`，
`tools/uniorm-gen/generator.cpp:58-95`），所以两家逻辑类型相同即成员相同；但同一逻辑
列在两家可以读出不同 `sql_type`。一个按驱动的通常报法成立的例子（待 Oracle backend
落地实测）：无精度的 `NUMBER` 作主键报 decimal，生成的成员是 `std::string`，而
MySQL 的 `INT` 报 integer，成员是 `std::int32_t`。把后一份头拿到前一个库里，
`validate()` 严格模式比对 `accepted_types` 即抛（抛出点 `src/orm.cpp:106-111`，
位集 `include/uniorm/mapping/registry.hpp:196-230`）：

    column T_USER.ID is decimal, which the mapped member does not bind

那是"换一家重新生成"的职责，也是一次响亮的失败，正是想要的。B/C/D 只搬标识符，不搬
类型：成员类型进了 ABI，任何运行期策略都不该去改它。

## 9. 与本仓纪律的一致性

- **不新增 `capabilities` 标志位。** 清单只装核心真会分岔的地方（design.md §5.2，四
  个无读者的占位刚清掉）。策略与解析都不需要按家分岔的慢路，失败即抛。
- **`dialect` 的新字段仍在 `dialect` 的射程内**：它管的正是"这家 SQL 怎么写"，标识
  符大小与引号同属一处；`pagination` 不受影响。
- **生成物的命名规则仍然固定不可配**（design.md §6.3），C/D 都不给生成器加配置项。
- **不为此扩 CI 矩阵。** 驱动与服务的跨格配法仍是 design.md §9 记下的那笔缺口，本提
  案与它无关。

## 10. 分步

1. **步骤 1（B + §5 前提）**：目录读取按名字再筛（判据见 §5）；`validate()` 的候选诊
   断（含目录侧的 ASCII 折叠比较，步骤 2 复用同一个 helper）。无 API 变更。约 80–120
   行含测试。**已落地**：折叠单一来源进 `include/uniorm/detail/identifier.hpp`，核心
   与 `uniorm-gen` 的 `naming.cpp` 共用它，于是全库只有一处 ASCII 折叠规则；
   `validate_entity()` 的两条落空消息带上 `(only case differs from '…')`，假目录用例
   逐字钉住（`test_orm_validate`）。
2. **步骤 2（C）**：`dialect::identifier_case` 与 `quote_identifier` 里的 ASCII 折
   叠；`connection::sql_dialect()` 收敛 11 个站点；`orm`/`connection` 各一个 setter；
   §4.3 的 `validate()` 一致性。约 70–90 行。**已落地**，两处与草案不同：策略落在
   `connection` 上、`orm` 只存一份并在租到连接时重贴（与 `auto_commit` 同一个解法），
   网关自己那份探测缓存随之删掉，`src/builder/builder.cpp` 因此空了、整个文件连同
   `CMakeLists.txt` 里那一行一起移除；`dialect` 是公开值类型，多一个字段即改变
   `sizeof`——一笔认下的 ABI 变更，0.x 期间不另作处理。DSN 解析照旧不做。
3. **步骤 3（生成器侧）**：`cfg.tables` / `columns` 的键两侧折叠，与 `[types]` 对齐
   （`generator.cpp:146/158`、`config.cpp:158/184`），于是一份 `.toml` 跨家可用。约
   20 行 + `test_gen_config` 一个用例。**已落地**，并连带 §12 第 2 条的未命中段报错。
4. **步骤 4（D，条件触发）**：拼法不统一的部署真出现时再做，见 §6。

每步单独提交、单独可评审，均不动 golden。

## 11. 若采纳，文档怎么改

**已按此改过**，落点与草案略有出入：三值语义写在 design.md §4.8（`dialect` 自己住在
那里，持有者与"出口唯一"的论证跟着它走），§4.5 的 `connection` 草图与 §4.7 的 `orm`
草图各补一对访问器，§4.7 的 `validate()` 注释收下 §4.3 那条一致性规则；§5.2 写下"名字
按名字问，不按 pattern 问"（§5 的落地形状），§6.2/§6.3 各自接上生成器侧，§6.4 写明配置
键统一为大小写不敏感；§9 给 D 立了第 11 条并写明触发条件；§3 目录树收下新的
`detail/identifier.hpp`，§8 测试清单收下三个新用例。README 中英两侧同步也做了——策略
是一对访问器，不写进 README 就没人知道它存在。措辞上留了一处收敛：`orm` 的注释与
README 两侧都只说"`validate()` 按同一策略问目录，落空时报目录侧的拼法"，不写"校验收
的就是查询会发出去的那个名字"（§4.3 末段那一格）；§6.4 另写明那项检查要把整份目录列
一遍，所以配置里没有 `[tables.*]` 段时就不列。

## 12. 生成器侧的三处沉默（与跨服务端无关，可独立修）

大小写形状的问题不止跨服务端那一笔。用 `generate_header()` 离线构造目录跑一遍即可复
现，三条都不需要真库，也都不属于 A/B/C/D 任何一个选项：

1. **类名折叠无守卫。** `generate_header()` 只循环调 `emit_table()`
   （`generator.cpp:266-283`），类名取 `to_pascal_case(table.name)`（`:199-202`），
   全程没有已用名字表。只差大小写的两张表折成同一个 `struct` 与同一个
   `register_*_mapping`，产物编译不过，而 uniorm-gen 零警告零退出码。同一个函数里
   成员名碰撞反而有检测与改名（`:181-186`），严谨度不一致。修法是一张已用类名表，
   警告或报错二选一：警告贴合成员碰撞的既有口径，报错更合"产物编不过就别写"。**已
   落地**：选了报错，`check_class_names()` 在拼出任何文本之前比对已用类名，几组冲
   突一次报完，而每一组的消息说清它属于哪一种解法。差一个尾下划线的一对，其中一表的
   `class` 覆写就能解开（`tables collide on one class name: 'UserAccounts' names
   user_accounts, user_accounts_. Set class in one of their [tables.*] sections.`）；
   只差大小写的一对解不开，配置键两侧折成小写（本条第 2 项）使 `class` 与 `skip`
   都指不到其中单独一张，于是消息改口（`'Users' names users, USERS. users, USERS
   differ by case alone, which no [tables.*] section can split: ask for one of them
   with --tables.`）——那种库本来就得用 `--tables` 分开跑两次，两次各产一份头。
2. **配置键大小写敏感且从不反向校验。** §1 末段的两套规则之外还有一层：`main.cpp`
   不检查每个 `[tables.*]` 段是否命中真实表，于是 `class` 写错大小写等于没改名，
   列覆盖等于没覆盖，`skip` 写错大小写是最坏的一种——那张表照样进产物。这与
   `--tables=` 的宽容（`schema_reader.cpp:44-67`）在同一个工具里并存。步骤 3 的键
   折叠解决跨家，未命中段报错解决静默失效，两件事都要。**已落地**：`naming.cpp` 的
   `fold_lower` / `fold_upper` 两侧同折，`check_config()` 在写文件之前点名每一段落空
   的段；被 `--tables` 筛掉的表只查表名、不查列名（那次读没取过它的列）。
3. **主键标记跨两次目录读取用 `==` 联结，全不命中时零输出。**
   `read_primary_keys()`（`schema_reader.cpp:83-91`）拿 `SQLPrimaryKeys` 的名字与
   `SQLColumns` 的名字精确比，而 `primary_key()` 还会跳过 NULL 名列
   （`src/odbc/schema_catalog.cpp:181-207`）。一家驱动报出的 PK 名与列目录只差大小
   写，结果是该表一列主键都没有，而附近唯一的警告只在 `table.columns.empty()` 时触
   发（`schema_reader.cpp:136-138`）。这与当年 MariaDB 3.1.12 丢 `COLUMN_KEY='pri'`
   是同一类失效：今天只被夹具 golden 挡住（`.column` 还是 `.primary_key`），三家 CI
   之外的库里静默错。归进步骤 1——B 的立论就是"落空要说得出候选"。**已落地**：
   `read_primary_keys()` 回报落空的名，只差大小写时把该表的实际拼法带进警告文本，走
   `--config` 那条警告通道；`tests/unit/test_gen_reader.cpp` 的假目录钉住三种形状
   （命中、只差大小写、根本没有）。

