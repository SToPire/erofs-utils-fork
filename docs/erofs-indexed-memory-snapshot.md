# 基于 EROFS Index 的 microVM 内存增量快照

## 1. 文档目的

本文定义 microVM 内存增量快照的数据模型、累计索引语义、hypervisor 状态组织方式以及创建和恢复流程。

内存与磁盘使用两条独立、结构对称的快照链：

- `Di` 与 `Ii` 组成磁盘增量链；
- raw data blob `Mi` 与 EROFS index `Ji` 组成内存增量链；
- `Hi` 保存每代完整 hypervisor 原生状态；
- 顶层 snapshot manifest 将指定世代的磁盘、内存和运行状态绑定为一个可恢复的 microVM 快照。

磁盘链的详细设计见 [erofs-indexed-block-snapshot.md](erofs-indexed-block-snapshot.md)。
具体 mkfs 接口见
[基于 file-delta、blobdev 和 rebuild 的累计块索引构建](erofs-file-delta-blob-index.md)。

## 2. 数据模型

### 2.1 基础内存镜像 `M0`

`M0` 是一条内存快照 lineage 的完整数据起点。它是 raw external blob，
保存各 RAM slot 的初始页内容并以内容摘要作为稳定身份。初始累计索引
`J0` 将所有 RAM 映射块指向 `M0`。对应的完整 hypervisor 状态由 `H0`
保存，并由顶层 snapshot manifest 与 `J0` 绑定。

### 2.2 第 i 代内存变化集 `ΔMi`

`ΔMi` 是 hypervisor 在第 i 个运行世代中记录的 dirty page 集合及其在快照截止点的内容。每个 dirty page 由 RAM slot ID 和 guest frame number 标识。

`ΔMi` 是构建输入。构建器按 EROFS 映射粒度将 dirty pages 展开为完整映射块。

### 2.3 第 i 代内存数据 blob `Mi`

`Mi` 是由 `mkfs.erofs --blobdev` 生成的本代不可变 raw binary：

```text
Mi.blob
├── compact dirty block 0
├── compact dirty block 1
└── ...
```

`Mi` 只保存本代非零 dirty mapping blocks。各块按 file-delta exception 的
处理顺序写入并按 EROFS chunk size 对齐；重复块可以去重，全零 dirty block
由 `Ji` 中的 hole 表示而不写入 `Mi`。

### 2.4 第 i 代 hypervisor 状态 `Hi`

`Hi` 是独立的不可变 artifact，保存本代完整 vCPU、设备、虚拟中断控制器、
时钟、定时器和内存拓扑状态。内存数据和 hypervisor 状态不共享一个 EROFS
文件树；顶层 snapshot manifest 通过相同的 snapshot ID 和 cutoff ID 将
`Ji`、`RuntimeDeviceBindings(Ji)` 与 `Hi` 原子绑定。

### 2.5 第 i 代累计内存索引 `Ji`

`Ji` 是 index-only EROFS 镜像，描述第 i 代完整内存视图：

```text
Ji.erofs
└── memory/slots/<slot-id>.img
```

每个 RAM slot 由一个固定大小的 regular-file inode 表示。inode 的逻辑偏移对应 slot 内偏移，其数据块可以指向 `M0` 或任意已发布的 `Mk`。

### 2.6 第 i 代完整内存视图 `RMi`

`RMi` 由 `Ji` 及其数据依赖共同解析得到，包含全部 RAM slot 的连续字节视图。
完整 VM 恢复入口是顶层 snapshot manifest；它同时选择 `RMi` 和匹配的 `Hi`。

## 3. 累计索引语义

令 `Bm` 表示内存索引的映射粒度，`Ei` 表示第 i 代 dirty pages 覆盖的完整
`Bm` 映射块集合。一个映射块包含 dirty page 时，构建器从快照截止点的内存
视图读取该映射块的完整内容并提交判零；非零块写入 `Mi`，全零块在 `Ji`
中写成 hole。

当前实现要求 `Bm = --chunksize = dm STORE chunk size`，并要求每个 slot 大小
按 `Bm` 对齐。每个 RAM slot 使用独立 TARGET 和 STORE，并在一次 mkfs 命令中
重复指定 `--file-delta`；若 hypervisor dirty-page 粒度小于 `Bm`，采集端必须
先读取截止点上的完整 `Bm` block。

对于 RAM slot `s` 中的逻辑映射块 `b`：

```text
Ji[s, b] = hole                                 if (s, b) ∈ Ei 且 block 全零
           location(Mi, payload(i, s, b))       if (s, b) ∈ Ei 且非零
           J(i-1)[s, b]                         otherwise
```

`location` 包含 index-local external-device ordinal 和 blob 内物理块地址；
顶层 manifest 把 ordinal 绑定到稳定的内容摘要。`Ji` 直接指向每个内存块的
最新所有者。

`ReachablePayloads(Ji)` 表示 `Ji` chunk indexes 实际可达的内存 data blob
集合。它是 `{M0, M1, ..., Mi}` 的子集。

当前构建器不压缩 parent device table；`RuntimeDeviceBindings(Ji)` 是恢复端
必须按顺序绑定的全部 slots，即使其中某个历史 blob 已不再属于
`ReachablePayloads(Ji)`。发布和 GC 必须以 `RuntimeDeviceBindings(Ji)` 为准。
erofs-utils 当前最多绑定 256 个 external devices；达到该上限前需要先做
compaction/squash，不能继续追加 generation blob slot。

## 4. 快照生成流程

### 4.1 建立写入世代

1. 以 `J(i-1)` 表示的累计内存状态作为本世代起点。
2. hypervisor 开启新一代 dirty-page tracking。
3. 运行期间持续收集 RAM slot 的 dirty page 信息。

### 4.2 建立一致性截止点

1. Guest 完成应用和文件系统 quiesce，并提交存储 flush。
2. hypervisor 暂停 vCPU 和设备模型。
3. Host 排空块 I/O，并封存对应的磁盘 COW 世代。
4. hypervisor 固化最终 dirty-page 集合及其页内容。
5. hypervisor 序列化 vCPU、设备、时钟和内存拓扑状态。
6. 磁盘增量、内存增量和 hypervisor 状态记录同一个 cutoff ID。
7. 页内容和运行状态进入稳定 staging 后，Host 为磁盘安装新的活动 COW head，
   hypervisor 建立下一内存 dirty-tracking 世代，确认两条写路径均已切换后再恢复
   VM 运行。

### 4.3 构建与发布

1. 将 dirty pages 展开为 `Ei`，并读取每个映射块的完整内容。
2. 将完整 hypervisor 状态封存为独立 artifact `Hi`。
3. 对每个 RAM slot，把完整 dirty mapping blocks 写成一个已封存的
   dm-persistent file-delta STORE。
4. 调用 `rebuild J(i-1) + --file-delta STORE + --blobdev Mi`：从
   `J(i-1)` 继承 RAM slot 的累计映射，只把本代非零 payload 写入 `Mi`，
   并将 `Ei` 更新为 `Mi` 中的新位置或 hole。
5. 生成 `Ji`，校验全部 external-device 地址、文件大小和内容摘要。
6. 等待同 cutoff 的磁盘 `Di/Ii` 构建和校验完成；发布磁盘与内存 data blobs、
   `Ii`、`Ji` 和 `Hi`，最后发布顶层 snapshot manifest。

EROFS 构建使用稳定 staging 数据执行，VM 的暂停区间覆盖一致性截止点的建立过程。

## 5. 顶层快照绑定

顶层 snapshot manifest 将磁盘链和内存链绑定为同一个 microVM 状态，至少记录：

```text
snapshot_id
parent_snapshot_id
cutoff_id
oci_lower_digest
disk_index_digest
memory_index_digest
hypervisor_state_digest
disk_devices[]
memory_devices[]
hypervisor_type
hypervisor_state_format_version
architecture
cpu_feature_digest
machine_config_digest
memory_layout_digest
```

`disk_index_digest` 标识 `Ii`，`memory_index_digest` 标识 `Ji`，
`hypervisor_state_digest` 标识 `Hi`。`disk_devices` 和 `memory_devices`
分别将各 index 内的 device ordinal 解析为内容寻址的 OCI blob。snapshot ID、
parent snapshot ID 和 cutoff ID 只由顶层 manifest 绑定；不可变 `Ii`、`Ji`
或 `Hi` 可以被多个顶层快照引用，而不需要携带新的 cutoff ID。

manifest 中的兼容性字段支持恢复端在建立 VM 前完成 hypervisor、CPU、机器类型、设备配置和内存布局校验。`Hi` 保存完整的非 RAM 执行与设备状态；
manifest 的 `memory_layout_digest` 是启动前校验入口，若它与 `Hi` 内的内存
拓扑不一致，恢复必须失败。

## 6. 恢复流程

1. 读取顶层 snapshot manifest，验证内容摘要和兼容性字段。
2. 打开 `Ii` 及其磁盘依赖，建立冻结的 upper 块设备视图。
3. 打开 `Ji` 并按 device table 顺序注册全部内存 data-blob slots。
4. 根据 memory layout 创建 RAM slots，并从 `Ji` 的 slot inode 装载对应页内容。
5. 根据 machine config 创建 vCPU 和设备模型。
6. 从顶层 manifest 指定的 `Hi` 恢复 vCPU、设备、时钟和中断状态。
7. 为磁盘视图创建新的活动 COW 世代，并连接 VM 块设备。
8. 建立下一代内存 dirty-page tracking，并恢复 vCPU 运行。

## 7. 核心不变量

1. `M0`、所有已发布的 `Mi`、`Ji` 和 `Hi` 保持不可变。
2. 每个 `Ji` 精确继承其 parent `J(i-1)` 的累计映射。
3. 每个内存映射块解析到唯一的 external device 和物理块地址。
4. 一条 lineage 使用稳定的 RAM slot ID、guest physical range 和映射粒度。
5. `Ji` 引用的每个 data blob 均具有稳定身份、确定大小和内容摘要。
6. `Hi` 与 RAM 页内容共享 snapshot ID 和 cutoff ID。
7. 顶层 manifest 精确绑定对应的 `Ii`、`Ji`、OCI lower 和 machine config。
8. data blob 先于引用它的 index 发布，`Ii`、`Ji` 和 `Hi` 先于顶层 snapshot manifest 发布。

## 8. 分支与生命周期

`Ji` 可以作为新内存世代的 parent。多个分支从同一个 `Ji` 建立独立 dirty-page tracking 世代，并共享祖先内存 data blobs。

引用管理以 `Ii`、`Ji`、`Hi` 和顶层 snapshot manifest 为根计算磁盘、内存
和 hypervisor 状态的运行时绑定闭包。data blob 只有在不再出现在任何
`RuntimeDeviceBindings` 后才进入回收流程。
周期性基线快照可以缩短依赖集合并控制累计索引规模。

## 9. 总结

该方案使用 raw blob `Mi` 保存每代 dirty memory payload，使用 `Ji` 描述完整累计内存视图，使用 `Hi` 保存本代完整 hypervisor 状态。磁盘链 `Di/Ii` 与内存链 `Mi/Ji` 独立演进，顶层 snapshot manifest 通过统一 cutoff 将两条链和 `Hi` 绑定为可恢复的 microVM 快照。
