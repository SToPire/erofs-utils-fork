# 基于 EROFS Index 镜像的增量块快照架构

## 1. 文档目的

本文描述一种面向 microVM 的分层快照与分发架构。该架构使用两套彼此独立、粒度不同的存储层：

- 以 OCI 镜像分发、以 EROFS 表示的不可变 rootfs lower；
- 以块设备承载 overlayfs upper，并按写入世代生成的块级增量快照。

本方案的核心不是把每次快照重新物化为完整的 upper 块设备镜像，而是：

1. 将每一代新产生的非零 exception payload 保存到独立的 raw external blob；
2. 为每一代快照生成一个累计的 EROFS index-only 镜像；
3. 由 index-only 镜像把一个逻辑 upper 文件的不同数据块映射到不同代的数据 blob；
4. 通过 file backend 将累计索引解析为连续块设备视图，再经 vhost-user-blk 提供给 microVM。

本文侧重最终的数据模型、索引语义和快照流程。dm-snapshot persistent exception store 是增量构建的输入形式，不是最终分发格式；用于接收该输入的 mkfs.erofs 修改只是实现过程中的一个步骤。

配套的 microVM 内存增量快照设计见 [基于 EROFS Index 的 microVM 内存增量快照](erofs-indexed-memory-snapshot.md)。

## 2. 背景与问题

microVM 的 rootfs 由 OCI 镜像提供，OCI 各层使用 EROFS 作为文件系统格式。它们是文件级、只读且可共享的不可变层。

VM 运行时仍需要可写根文件系统。Host 通过 vhost-user-blk 向 VM 提供一块可写块设备，该设备上包含一个可写文件系统。Guest 将该文件系统中的目录作为 overlayfs 的 `upperdir` 和 `workdir`，并与 OCI/EROFS lower 合成为最终根文件系统。

如果每次快照都保存一份完整的 upper 块设备镜像，会带来以下问题：

- 快照创建和分发的数据量与整个 upper 设备大小相关，而不是与实际修改量相关；
- 不同快照之间存在大量重复块；
- 物化完整镜像会增加 I/O、存储和发布延迟；
- 多个快照或分支难以共享未变化的数据。

本方案希望保留 OCI/EROFS lower 的文件级不可变语义，同时为 overlayfs upper 引入独立的块级不可变增量层。两者在 VM 中通过 overlayfs 合并，但在分发、索引和生命周期上互不混淆。

## 3. 目标与非目标

### 3.1 目标

- 每一代 upper 快照只保存相对于上一代新产生的块 payload。
- 每一代 index 都能作为该快照的稳定语义入口；配合其数据依赖闭包即可恢复，不需要先合并出完整 raw 镜像。
- 运行时读取任意逻辑块时，可以通过累计索引直接定位到拥有该块的数据 blob，无需逐层重放 dm exception chain。
- 历史快照和不同分支能够共享同一批不可变数据 blob。
- 数据 blob 和 index 镜像都能作为内容寻址对象纳入 OCI 分发体系。
- 本层 payload 的读取和写入量原则上与 exception 覆盖范围相关，而不是与完整 upper 设备大小相关。总构建成本还包括累计索引的生成与校验。

### 3.2 非目标

本文暂不展开以下内容：

- raw data blob 和 index-only 镜像的 OCI artifact/layer 封装方式；
- Guest 文件系统冻结、在线切层和崩溃一致性协议；
- vhost-user-blk 控制面及运行时热切换流程；
- 快照链压缩、垃圾回收和配额策略；
- 加密、远端按需读取和内容校验的具体实现；
- 某个特定 mkfs.erofs patch 的接口与代码细节。

这些问题会影响完整产品实现，但不改变本文定义的核心索引语义。

## 4. 术语与符号

### 4.1 OCI/EROFS lower

microVM 的不可变 rootfs 目录树。它由一个或多个 OCI/EROFS 文件级 layer 组成，并作为 Guest overlayfs 的 lower。

### 4.2 初始基础 upper 设备 `U0`

一个预先格式化的基础可写文件系统镜像，用作第一代块快照的 origin。它包含建立 overlayfs `upperdir` 和 `workdir` 所需的初始文件系统状态。

`U0` 本身必须能够被 index 引用，但其具体分发和封装方式不在本文展开。

### 4.3 第 i 代 dm 增量 `Δi`

以第 `i-1` 代累计 upper 视图为 origin，在第 i 个写入世代中产生的 dm-snapshot persistent exception store。它包含：

- exception metadata，用来描述哪些 origin chunk 被本层覆盖；
- exception payload，保存这些 chunk 在第 i 代中的新内容。

`Δi` 是构建输入，不直接作为运行时分发格式。

### 4.4 第 i 代数据 blob `Di`

一个由 `mkfs.erofs --blobdev` 生成的不可变 raw binary，紧凑承载从 `Δi`
提取出的非零 exception payload；一般模型还允许为组成完整 EROFS 映射块而
复制必要的 parent 边界字节。全零完整映射块由 index 中的 hole 表示，不占用
`Di` 空间。当前严格等粒度实现不需要复制边界字节。

`Di` 不是 EROFS 文件系统，也不是完整 upper 块设备；它不包含目录树、
文件大小或累计映射。单独读取 `Di` 不能得到有意义的 upper 文件系统视图。

### 4.5 第 i 代累计视图 `Vi`

应用前 i 代块增量后得到的完整逻辑 upper 块设备：

```text
Vi = U0 ⊕ Δ1 ⊕ Δ2 ⊕ ... ⊕ Δi
```

其中 `⊕` 表示：对 dm exception 覆盖的 chunk 使用新 payload，对其他范围继承上一代内容。

`Vi` 是连续、可被文件系统解释的逻辑设备字节序列，但不要求在 Host 上以完整 raw 文件的形式存在。

在 EROFS index 中，`Vi` 由一个 regular-file inode 表示。该文件下称 **upper backing file**：它的字节序列等于 upper 块设备的内容，file backend 将该文件的字节范围导出为块设备。其具体路径不属于本文定义的接口。

### 4.6 第 i 代 index-only 镜像 `Ii`

一个累计的 EROFS 索引镜像，用来描述完整 `Vi`。对于 upper backing file 中的每个逻辑数据块，`Ii` 记录：

- 数据所在的 external device，即 `U0` 或某个 `Dk`；
- 数据在该 device 中的物理位置；
- 解释该文件所需的 inode、文件大小和布局信息。

因此，`Ii` 表示 `D1...Di` 叠加后的第 i 代状态，而不是只表示第 i 代的增量。它是元数据自足的语义入口，但恢复数据仍然需要它实际引用的 external-device 闭包。

本文约定 `Vi` 和 `Ii` 对 `i ≥ 0` 定义，且 `V0 = U0`；`Δi` 和 `Di` 对 `i ≥ 1` 定义。公式中的 `i` 表示一条选定快照 lineage 内的深度，不是 artifact 的全局身份。实际产物应使用唯一 snapshot ID 或内容摘要，并显式记录 parent。

## 5. 总体架构

```text
                              Host

    Ii ──resolve──► frozen Vi (read-only origin)
     │                         │
     │                         ├── active COW store
     │                         ▼
     │                 writable dm-snapshot view
     │                         │
     │                  vhost-user-blk
     │                         │
     ├──► U0                   ▼
     ├──► D1          Guest writable filesystem
     ├──► D2                   │ upperdir/workdir
     └──► Di                   ▼
                        Guest overlayfs root
                               ▲
                               │ read-only lower
                       OCI/EROFS rootfs layers
```

该架构包含两个互相独立的数据平面：

1. OCI/EROFS lower 提供文件级、只读的基础 rootfs；
2. `Ii` 及其数据依赖提供块级、可恢复的 overlayfs upper 状态。

块级 upper 并不是针对 OCI lower 的块差异。它保存的是 Guest 可写 upper 文件系统自身的演化历史。overlayfs whiteout、opaque xattr、copy-up 文件、目录变更等语义均由 upper 文件系统编码，并自然落入其块级增量。

`Ii` 解析出的 `Vi` 是已经封存的只读基线。若要恢复一个可继续写入的 VM，必须以 `Vi` 为 origin 创建新的活动 COW store，再把合成后的可写 dm-snapshot 设备交给 vhost-user-blk。直接暴露 `Vi` 只适用于只读检查或校验。

一个完整 VM 快照需要同时绑定：

- 对应的 OCI lower 身份；
- 累计 upper index `Ii`；
- `RuntimeDeviceBindings(Ii)` 中按 ordinal 绑定的完整 data-blob slot 集合。

## 6. 累计索引语义

upper backing file 使用字节偏移作为规范逻辑地址空间。令 `Ri` 表示 `Δi` exception table 覆盖的字节区间集合，令 `B` 表示 EROFS index 的数据映射粒度。再定义 `Ei` 为与 `Ri` 相交的 EROFS 逻辑块集合。

只有当某个 EROFS 逻辑块的整个字节范围都被 `Ri` 覆盖时，才能从 exception payload 直接切出该映射块。若只覆盖其中一部分，构建器必须从 `Vi` 组装完整的 EROFS 映射块：变化部分取自 `Δi`，其余部分继承 `V(i-1)`。dm chunk 大小是 `B` 的整数倍且边界对齐时，除文件尾部外通常可以避免这种边界补全；文件尾部只在 `Vi` 的固定逻辑大小内有效。

以上描述的是一般架构。本文采用的当前 mkfs 接口有意只实现严格子集：
dm chunk size、EROFS chunk size 和 `--chunksize` 必须完全相等，TARGET 大小
必须 chunk 对齐，因此当前实现不执行拆分、合并或边界补全。

本文的索引公式均以 EROFS 逻辑块为单位。对任意逻辑块 `b`，定义它在第 i 代的所有者：

```text
owner(i, b) = max { k | 1 ≤ k ≤ i 且 b ∈ Ek }
```

如果不存在这样的 `k`，该块继承自 `U0`。

于是 `Ii` 对逻辑块 `b` 的映射为：

```text
Ii[b] = location(U0, b)                         if owner(i, b) 不存在
        hole                                    if owner(i, b) 存在且完整块全零
        location(Dk, payload(k, b))             if owner(i, b) = k 且完整块非零
```

这里的 `location` 至少包含 external-device 标识和设备内物理块地址。

定义 `ReachablePayloads(Ii)` 为 `Ii` 的 chunk indexes 实际可达的数据集合。
它是 `{U0, D1, ..., Di}` 的子集：如果某个旧 data blob 的所有 payload 都已
被后续层覆盖，则它仍可能被历史 index 使用，但不再属于该集合。

当前 mkfs 实现为了保持 device ID 稳定，会原样复制 parent device table 而不做
compaction。因此实际恢复接口仍要求按顺序绑定 device table 的全部 slots，
本文把当前部署时必须绑定的完整 slot 集合称为 `RuntimeDeviceBindings(Ii)`；
其中可能包含已不属于 `ReachablePayloads(Ii)` 的历史 blob，且发布和 GC 必须
以该完整集合为准。erofs-utils 当前最多绑定 256 个 external devices；达到
该上限前必须做 compaction/squash，不能继续按“每代追加一个 slot”的方式生成
新一代。只有重写 index 并重编号 device IDs 后，二者才能相等。

### 6.1 最新层覆盖旧层

同一逻辑块可以在多个世代被修改。`Ii` 对最新的非零内容直接指向最后一次
修改它的 `Dk`，对最新的全零内容记录 hole。旧 payload 继续存在于旧 data
blob 中，以保证历史 index 仍然可用，但不会出现在新视图的读取路径中。

### 6.2 累计索引而非运行时链式查找

`Ii` 已经归并了前 i 代 exception 的结果。恢复 `Vi` 时，backend 不需要依次查询 `Δi`、`Δ(i-1)`，直到命中某个 exception；它只需读取 `Ii` 给出的最终映射。

因此，数据层数增加主要影响 index 构建、依赖管理和 external-device 数量，而不应让单次数据读取退化为 O(i) 的链式查找。

### 6.3 同一文件跨多个 external device

传统的单设备或整文件设备归属语义不足以表示 `Vi`。本方案要求 EROFS 索引允许同一个 regular file 的不同数据块指向不同 external device。例如：

```text
Vi block 0..127       → D1
Vi block 128..255     → U0
Vi block 256..383     → D3
Vi block 384..511     → D2
```

这是本架构依赖的关键 EROFS 能力。EROFS 8-byte chunk index 磁盘格式和读
路径已经具备逐 chunk device ID；本分支补齐了 mkfs rebuild 对 parent 混合
映射的继承和 file-delta 覆盖能力。

## 7. 映射示例

假设 `U0` 包含五个逻辑块。第一代修改 `b1` 和 `b3`，第二代再次修改 `b3`，并修改 `b4`：

| 逻辑块 | `I0` | `I1`：应用 `D1` 后 | `I2`：应用 `D2` 后 |
| --- | --- | --- | --- |
| `b0` | `U0:b0` | `U0:b0` | `U0:b0` |
| `b1` | `U0:b1` | `D1:p0` | `D1:p0` |
| `b2` | `U0:b2` | `U0:b2` | `U0:b2` |
| `b3` | `U0:b3` | `D1:p1` | `D2:p0` |
| `b4` | `U0:b4` | `U0:b4` | `D2:p1` |

在这个例子中：

- `D1` 单独只包含 `b1` 和第一版 `b3` 的 payload，不能表示 `V1`；
- `D2` 单独只包含第二版 `b3` 和 `b4` 的 payload，不能表示 `V2`；
- `I1 + U0 + D1` 可以恢复完整 `V1`；
- `I2 + U0 + D1 + D2` 可以恢复完整 `V2`；
- `I2` 读取 `b3` 时直接指向 `D2:p0`，不需要先检查 `D2` 再检查 `D1`；
- `D1:p1` 对 `I2` 已不可见，但必须保留，直到不再需要恢复 `I1` 或其派生分支。

## 8. 快照生成流程

### 8.1 初始化

1. 创建固定大小、预格式化的基础 upper 文件系统 `U0`。
2. 建立可以描述 `U0` 的初始索引 `I0`。
3. 通过 file backend 解析 `I0`，得到只读 origin `V0 = U0`。
4. 在 `V0` 上创建第一份活动 COW store，形成可写 dm-snapshot 视图。
5. 通过 vhost-user-blk 向 Guest 提供该可写视图。
6. Guest 挂载其中的文件系统，并将其目录用作 overlayfs upper。

### 8.2 运行第 i 个写入世代

1. 以上一代累计视图 `V(i-1)` 作为 dm-snapshot origin。
2. 创建新的 persistent COW store `Δi`。
3. 所有本世代写入只进入该 lineage 的活动 snapshot，不允许修改 `V(i-1)` 依赖的任何已有 data blob。
4. Guest 继续看到一个完整、可写的块设备，而不是一组离散增量。

### 8.3 封存本世代

封存协议至少必须建立一个明确的写入截止点：完成所需的 Guest flush 或 quiesce，把后续写入停止或原子地导向新的活动 COW store，并保证 `Δi` 从此不再变化。完成这一外部前置条件后：

1. 校验 dm persistent header、chunk size、exception table 和 payload 范围；
2. 由 `Ri` 展开得到需要更新的 EROFS 逻辑块集合 `Ei`；
3. 对每个 `b ∈ Ei` 生成完整映射块：被 `Ri` 覆盖的字节取自 `Δi`，未覆盖的边界字节取自 `V(i-1)`；
4. 将这些完整、非零映射块紧凑写入新的 raw data blob `Di`，并记录其物理位置；全零块记录为 hole；
5. 以 `I(i-1)` 为基础继承所有已有块映射；
6. 对 `Ei` 覆盖的逻辑范围更新映射：非零块指向 `Di`，全零块写成 hole；
7. 生成累计 index-only 镜像 `Ii`；
8. 校验 `Ii` 能够与 `RuntimeDeviceBindings(Ii)` 共同还原 `Vi`；
9. 先发布 `Di`，再发布 `Ii`，最后发布包含全部 slot 绑定的顶层 manifest。

Guest 文件系统如何冻结、如何原子切换 COW store，以及如何实现 application-consistent 快照，属于独立的一致性协议，本文不展开。本架构只要求输入 `Δi` 对应一个确定且已封存的 cutoff；产物的一致性等级由外部封存协议决定。

### 8.4 创建下一代或分支

`Vi` 可以作为后继增量的 origin。由于 `Ii` 是一个可独立寻址的累计状态，多个新 COW store 也可以从同一个 `Vi` 分叉，形成多个共享历史 data blob 的快照分支。

本文的 `i+1` 记号只适用于其中一条选定 lineage。实际分支节点使用唯一 snapshot ID，并记录 parent；每条分支可以各有一个活动 COW head，所有共享祖先保持不可变。

## 9. 增量构建模型

构建 `Ii` 时，mkfs 同时掌握：

- `I(i-1)`：前 `i-1` 代所有变更归并后的累计映射；
- `Ri`：本层 dm exception table 给出的修改字节区间；
- `Ei`：由 `Ri` 展开得到的 EROFS 逻辑块集合；
- exception payload：本层新增的数据内容。

因此不需要通过比较 `Vi` 与 `V(i-1)` 来推断哪些块发生变化。映射更新可以直接表示为：

```text
Map_i(b) = hole                                  if b ∈ Ei 且完整映射块全零
           location(Di, new_payload(b))           if b ∈ Ei 且非零
           Map_(i-1)(b)                           otherwise
```

其中 `Map_i` 是 `Ii` 中对 `Vi` 的累计块映射，`new_payload(b)` 是按 EROFS 映射粒度生成的完整块；必要时它已经包含从 `V(i-1)` 补齐的边界字节。判定 hole 的对象是补齐后的完整映射块，而不是 exception 覆盖的局部字节。当前等粒度实现中两者相同。

一个合并视图的 `pread(Vi)` 接口仍然有价值：它为现有 mkfs 数据读取路径提供完整文件语义，也可用于校验和兼容性处理。但 payload 数据路径不需要读取整个 `Vi`；构建器可以只读取 `Ei` 覆盖的数据，并对其他范围继承 `I(i-1)`。累计 index 的继承、序列化或全量校验仍可能遍历完整映射，因此总构建复杂度不保证只与 `|Ei|` 成正比。

dm-snapshot 输入适配层的职责是把 persistent exception store 转换成以下两种能力：

- 按逻辑偏移读取 `Vi`；
- 枚举本层 dirty ranges 及其 payload 位置。

它是构建数据源的一部分，不定义最终的 EROFS 多设备索引格式。

## 10. 恢复与运行时读取

恢复第 i 代快照时：

1. 根据快照描述取得 OCI lower、`Ii` 和 `RuntimeDeviceBindings(Ii)`；
2. 验证 index 中记录的 external-device 身份与实际镜像一致；
3. backend 打开 `Ii` 并注册它所依赖的数据设备；
4. EROFS 根据 `Ii` 中的块映射从对应设备读取 payload；
5. backend 对外提供连续但已冻结的 upper backing file `Vi`；
6. 如果 VM 需要继续写入，以 `Vi` 为只读 origin 创建新的活动 COW store；
7. vhost-user-blk 将合成后的可写 dm-snapshot 设备暴露给 Guest；
8. Guest 挂载其中的文件系统，并与对应 OCI lower 组成 overlayfs root。

运行时的语义入口始终是 `Ii`，而不是任何一个 `Di`。如果缺失 `Ii` 或
`RuntimeDeviceBindings(Ii)` 中的任一 slot，backend 应拒绝建立视图，而不是
返回部分数据。未进入 `ReachablePayloads(Ii)` 的 slot 当前仍需绑定，是
device table 保序造成的部署约束，不表示读取路径会访问它。

## 11. Data blob 与索引镜像的职责分离

### 11.1 Raw data blob：不可变数据平面

`Di` 负责保存第 i 代 exception 引入的数据。它具有以下特征：

- 其业务数据只包含本代非零 exception payload，以及映射粒度不对齐时为形成完整 EROFS 映射块所必需的边界数据；
- 不包含原始 dm COW metadata 或累计逻辑映射；
- 除映射粒度不对齐所需的边界补全外，不重复保存从旧层继承的块；
- 发布后不可修改；
- 可以被多个 index 和多个快照分支共享；
- 不包含 EROFS 容器和索引元数据，不能单独挂载；payload 本身仍可能是
  Guest upper 文件系统的 superblock、inode、目录或 journal 等普通块内容。

### 11.2 Index-only 镜像：累计控制平面

`Ii` 负责解释数据。它具有以下特征：

- 描述完整 `Vi`，而不是只描述 `Δi`；
- 把逻辑块直接映射到最终拥有者；
- 保存 index-local device ordinal、块地址及必要的文件元数据；
- 本身不承载 upper payload；
- 是恢复、分支和运行时读取的稳定入口。

这种分离使 raw data blobs 可以保持增量和不可变，同时让每个快照仍然拥有一个元数据自足的累计逻辑描述；当前恢复仍需提供 `RuntimeDeviceBindings(Ii)`。

`Ii` 内的 device ordinal 和块地址是数据读取的权威映射。快照 manifest 负责把每个 ordinal 解析为内容寻址的实际 artifact，并同时绑定对应的 OCI lower；manifest 不能用另一套块映射覆盖 `Ii`。两者身份不一致时必须拒绝恢复。

## 12. 核心不变量

为了保证任意 `Ii` 都能稳定恢复对应 `Vi`，实现必须维持以下不变量：

1. `U0` 和所有已发布 `Di` 永远不可原地修改。
2. `Δi` 的 origin 必须精确等于 `V(i-1)`，不能是内容未知或后来被修改的近似副本。
3. 每条 lineage 只有活动 head 的 COW store 可写；多个分支可以各有活动 head，但任何共享祖先和已发布层都必须封存。
4. EROFS `device_id` 是单个 index 内稳定的顺序编号；顶层 manifest 必须把
   每个编号绑定到可靠的内容摘要，不能仅依赖偶然的本地文件顺序。
5. 每个逻辑块只能解析到一个确定的最终位置，覆盖优先级由最新世代决定。
6. `ReachablePayloads(Ii)` 中所有 payload 必须存在并满足块大小、对齐和边界要求；`RuntimeDeviceBindings(Ii)` 的每个 slot 都必须有稳定绑定。
7. upper 逻辑设备大小在一条快照链中保持稳定；扩容需要显式定义新的迁移语义。
8. dm chunk、EROFS block 和 Guest 逻辑扇区之间的换算必须无歧义。
9. `Ii` 发布时，其引用的所有 data blob 必须已经可用且不可变，且对应写入世代已有确定的 cutoff。
10. 一个 VM 快照必须关联正确的 OCI lower；不能只保存 upper index 而忽略 lower 身份。
11. 补齐后的完整 EROFS 映射块全零时必须生成 `EROFS_NULL_ADDR` hole，
    不能与未修改块的“继承 parent 映射”混淆；discard 的语义仍需单独定义。

## 13. 失败处理与校验原则

- dm store header 无效、exception 重复、payload 越界或 origin 大小不匹配时，构建必须失败。
- index 引用的 device 缺失、身份不匹配或大小变化时，恢复必须失败。
- data blob 应先于引用它的 index 发布，避免产生可见但不可满足的累计视图。
- 顶层 manifest 必须原子绑定 `Ii` 和完整 `RuntimeDeviceBindings(Ii)`，不能让消费者观察到半更新状态。
- 选择旧的 `Ik` 即表示回滚到第 k 代；回滚不应改写任何已有 data blob。
- 校验工具应能够遍历 `Ii` 的全部映射，确认所有 external-device 地址均有效。

具体摘要算法、签名机制和 OCI 发布事务不在本文定义。

## 14. 方案特性与代价

### 14.1 主要收益

- 数据增量：每个 `Di` 的业务数据只保存本世代新增 payload 及必要的边界数据。
- 快照共享：历史快照和分支共享相同的不可变数据层。
- 无需 flatten：创建、分发和恢复快照都不要求生成完整 raw upper 镜像。
- 直接读取：累计 index 将逻辑块直接映射到最终数据设备，避免运行时逐层查找 exception。
- 明确分层：OCI lower 保持文件级语义，upper 快照保持块级语义。
- 快速回滚和分支：选择不同 index 即可选择不同累计状态。
- 分发统一：raw data blob 和 EROFS index 都可以作为内容寻址 artifact 发布。

### 14.2 代价与约束

- 本实现依赖扩展后的 mkfs rebuild 来保留 parent 的逐 chunk device 映射并按
  exception 覆盖；EROFS 磁盘格式本身已有逐 chunk device ID。
- backend 必须同时管理 index 和多份 data blob，并验证依赖关系。
- 快照层数增加时，external-device 数量、index 元数据和依赖管理成本会增长。
- 单个 data blob 不具备独立恢复价值，运维工具必须以 index 为入口。
- dm chunk 与 EROFS block 不一致时，需要拆分或合并映射。
- 被新层覆盖的旧 payload 仍需为历史快照保留，空间回收依赖全局引用关系。
- 完整方案需要额外解决在线快照一致性、发布原子性和链生命周期问题。

## 15. 与现有增量构建工作的关系

dm-persistent file-delta、rebuild 和 blobdev 共同组成最终构建接口：

```shell
mkfs.erofs -Eforce-chunk-indexes \
  --chunksize=B \
  --blobdev=Di.blob \
  --file-delta=dm-persistent:TARGET:DELTA_STORE \
  Ii.erofs Iprev.erofs
```

rebuild 从 `Iprev` 继承目录树、device table 和所有 clean chunk indexes；
file-delta 枚举 exception 并读取 payload；blobdev 紧凑保存本代非零数据；
mkfs 只覆盖 dirty chunk indexes。详细接口和约束见
[基于 file-delta、blobdev 和 rebuild 的累计块索引构建](erofs-file-delta-blob-index.md)。

## 16. 后续需要单独设计的问题

以下问题应在后续文档中分别展开：

1. external-device 身份、编号、摘要和发现机制。
2. `U0`、`Di`、`Ii` 与 OCI manifest/artifact 的组织方式。
3. 在线创建 `Δi`、Guest quiesce、切换写层和故障恢复协议。
4. index 元数据规模、最大设备数量和读取缓存策略。
5. dm chunk 与 EROFS block 的拆分、合并或边界补全。
6. discard、部分 chunk 和设备尾部的表示方式。
7. 快照分支、引用计数、垃圾回收与链压缩。
8. 数据完整性、签名、加密及远端按需拉取。
9. 构建性能、启动延迟、随机读放大和存储节省的基准测试。

## 17. 总结

本方案将 microVM 根文件系统拆分为两个正交的层次：OCI/EROFS 提供文件级不可变 lower，块快照链提供 overlayfs upper 的可写历史。

每一代 dm snapshot 只作为构建输入。其非零 exception payload 被紧凑提取到独立、不可变的 raw data blob `Di`；累计 index-only 镜像 `Ii` 则通过
`RuntimeDeviceBindings(Ii)` 组织出完整的逻辑 upper 块设备 `Vi`。data blob
负责保存数据，index-only 镜像负责赋予数据位置和累计快照语义。

该设计的关键价值在于：数据按修改量增量分发，历史层可共享，而任意快照仍可通过累计 index 及其 `RuntimeDeviceBindings` 直接恢复，无需物化完整镜像，也无需在运行时逐层重放 dm exception chain。
