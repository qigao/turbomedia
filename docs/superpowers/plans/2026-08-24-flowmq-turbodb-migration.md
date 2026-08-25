# FlowMQ 与 TurboDB ORM 迁移实施计划

> 执行日期：2026-08-24
>
> 设计依据：`webrtc/docs/ivr-production-readiness-design-zh.md`

## 目标与兼容边界

- 从构建、运行时和测试中移除 `TurboFlow::FMQ`，IVR 内部消息只依赖 `FlowMQ::FlowMQ`。
- 以一个带类型帧的 `ROUTER/DEALER` 通道承载命令、结果和域事件；删除独立 PUB/SUB 端点及其配置。
- 从 RoomService 活跃存储中移除 `TurboFlow::FlowStore`，通过项目内适配器将 outbox/ledger 的 record-store 契约映射到 `TurboDB::ORM`。
- SQLite 必须提供事务性批量提交、revision CAS、确定性扫描和重启恢复。
- Redis 配置必须在创建存储时明确返回不支持，不允许在 TurboMedia 中直接引入 `libpq`
  或降级到非原子实现；PostgreSQL 配置传给 ORM，默认 SQLite-only runtime 明确拒绝，PostgreSQL 运行时
  若后续启用，只允许从 TurboDB 自身编译结果部署。
- 保留 outbox/ledger 上层领域接口和编码格式，避免把 ORM 类型扩散到领域模块。

## Task 1：建立失败测试

**文件：**

- 修改：`webrtc/ivr/tests/test_ivr_flowmq.c`
- 修改：`webrtc/ivr/tests/test_ivr_room_bridge.c`
- 删除：`webrtc/ivr/tests/test_ivr_flowmq_subscriber.c`
- 新增：`webrtc/apps/room_service/tests/test_iris_orm_store.c`
- 修改：`webrtc/apps/room_service/tests/test_iris_event_outbox.c`
- 修改：`webrtc/apps/room_service/tests/test_iris_command_ledger.c`

**步骤：**

1. 增加 ROUTER/DEALER 同通道返回域事件的断言，事件必须定向到已注册 worker route。
2. 增加 SQLite ORM 存储的 create/scan、revision 冲突、原子批次、destroy/reopen 恢复测试。
3. 增加 Redis 明确拒绝、PostgreSQL 透传到 ORM 且默认 runtime 明确拒绝的测试。
4. 先构建定向测试，确认测试因缺失新适配器或旧行为而失败，并保留失败输出作为 TDD 红灯证据。

## Task 2：迁移 IVR 通道到直接 FlowMQ

**文件：**

- 修改：`webrtc/ivr/src/ivr_flowmq_gateway.c`
- 修改：`webrtc/ivr/src/ivr_flowmq_gateway.h`
- 修改：`webrtc/ivr/src/ivr_room_bridge.c`
- 修改：`webrtc/ivr/src/ivr_room_bridge.h`
- 删除：`webrtc/ivr/src/ivr_flowmq_subscriber.c`
- 删除：`webrtc/ivr/src/ivr_flowmq_subscriber.h`
- 修改：`webrtc/ivr/CMakeLists.txt`
- 修改：`webrtc/apps/room_service/src/ivr_fmq_adapter.c`
- 修改：`webrtc/apps/room_service/src/ivr_fmq_adapter.h`

**步骤：**

1. Gateway 用 `flowmq_connect_endpoint_t` 建立 DEALER，收发 `flowmq_protocol_frame_t`。
2. Room bridge 用 `flowmq_router_endpoint_t` 建立 ROUTER；在 callback 边界复制 borrowed payload 和 route token，再交给 owner 队列。
3. worker 同步成功后保存唯一 route；命令回复和域事件都通过该 route 定向发送。
4. TLS peer identity 继续复用 `ivr_certificate_identity`，移除共享密钥和旧 TurboFlow security binding。
5. 删除 PUB/SUB 实现与测试，保持 queue item/byte 上限、stop/drain 和错误传播。
6. 运行 IVR 定向测试，确认消息、身份验证、乱序/重复和 shutdown 路径通过。

## Task 3：增加项目内 record-store 契约与 ORM 适配器

**文件：**

- 新增：`webrtc/apps/room_service/src/iris_record_store.h`
- 新增：`webrtc/apps/room_service/src/iris_orm_store.h`
- 新增：`webrtc/apps/room_service/src/iris_orm_store.c`
- 删除：`webrtc/apps/room_service/src/iris_flowstore.h`
- 删除：`webrtc/apps/room_service/src/iris_flowstore.c`
- 修改：`webrtc/apps/room_service/src/iris_event_outbox.c`
- 修改：`webrtc/apps/room_service/src/iris_event_outbox.h`
- 修改：`webrtc/apps/room_service/src/iris_command_ledger.c`
- 修改：`webrtc/apps/room_service/src/iris_command_ledger.h`

**步骤：**

1. 定义最小 `iris_record_store_t`：capabilities/limits、borrowed scan visitor、原子 commit 和 revision 常量。
2. 用 TurboParser 读取现有 YAML channel 配置；未知字段、重复 channel、非法 backend 和 Redis 均 fail fast；PostgreSQL 只透传给 ORM capability 边界。
3. 用 `orm_connect` 连接 SQLite，创建版本化 records 表。
4. `scan` 使用确定性 key 排序并在 ORM result 生命周期内提供 borrowed view。
5. `commit` 在 serializable transaction 中读取现有 revision、校验全部 mutation 和容量后统一写入；任意冲突 rollback。
6. 将 outbox/ledger 及其 fake store 测试迁移到本地接口，不改变领域行为和已持久化 payload 编码。
7. 运行 ORM、outbox 和 ledger 定向测试。

## Task 4：更新构建、配置与文档

**文件：**

- 修改：`CMakeLists.txt`
- 修改：`CMakeUserPresets.json`
- 修改：`webrtc/apps/room_service/CMakeLists.txt`
- 修改：`webrtc/apps/room_service/include/room_service/config.h`
- 修改：`webrtc/apps/room_service/src/config.c`
- 修改：`webrtc/apps/room_service/src/server.c`
- 修改：`webrtc/apps/room_service/config/room_service.toml.example`
- 修改：`webrtc/apps/room_service/config/room_service.flowstore.*.yaml.example`
- 修改：`webrtc/docs/ivr-production-readiness-design-zh.md`

**步骤：**

1. 根工程改为发现 `FlowMQ`、SQLite-only `TurboDB` 和 `TurboParser`，删除 `TurboFlow`/`libpq` 查找与运行时路径。
2. RoomService 链接 `TurboDB::ORM`，删除 FlowStore/StorageBackend/SQLiteStorage/Redis/PostgreSQL targets。
3. 删除 `pub_port`、`pub_topic`、`fmq_shared_secret` 配置和环境变量；配置入口遇到这些旧字段时明确报错。
4. YAML 样例改为 ORM backend/options 语义；Redis 样例改为不支持说明，避免生成看似可用的配置。
5. 文档记录新事实源、单通道拓扑、事务边界、Redis 限制和迁移风险。

## Task 5：验证与残留审计

1. `cmake --preset win-dev-user`
2. 构建相关 targets：IVR、RoomService、FlowMQ tests、ORM store/outbox/ledger/config tests。
3. `ctest --preset win-dev-user -R "ivr_flowmq|ivr_room_bridge|ivr_fmq_mtls|iris_orm_store|iris_event_outbox|iris_command_ledger|room_service_config" --output-on-failure`
4. 运行仓库允许范围内的全量相关 CTest。
5. 用 `rg.exe` 确认生产构建与源码中不存在 `TurboFlow::FMQ`、`TurboFlow::FlowStore`、`turbo_flow_fmq` 或 `turbo_flow_record_store`。
6. 检查 `git diff --check` 与 `git status --short`，记录未执行的平台验证与剩余兼容风险。
