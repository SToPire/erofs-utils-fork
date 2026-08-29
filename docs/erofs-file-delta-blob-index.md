# 基于 file-delta、blobdev 和 rebuild 的累计块索引构建

## 1. 决策

`mkfs.erofs` 使用以下现有接口组合构建一代不可变增量：

```text
rebuild parent + --file-delta input + --blobdev output
```

命令形式为：

```shell
mkfs.erofs \
  -Eforce-chunk-indexes \
  --chunksize=4096 \
  --blobdev=DELTA_BLOB \
  --file-delta=dm-persistent:TARGET:STORE \
  OUTPUT_INDEX PARENT_INDEX
```

其中：

- `PARENT_INDEX` 是上一代累计、index-only EROFS 镜像；
- `TARGET` 是 `PARENT_INDEX` 内需要更新的 regular-file 绝对路径；
- `STORE` 是只读、已封存的 dm-snapshot persistent exception store v1；
- `DELTA_BLOB` 是本代新建的 raw external blob，只保存本代非零 payload；
- `OUTPUT_INDEX` 是本代新建的累计、index-only EROFS 镜像。

`mkfs.erofs` 没有单独的 `--rebuild` 开关：最后一个位置参数是普通目录时
进入 local-directory 模式，是 EROFS 镜像文件时自动进入 rebuild 模式。
这里的 index-only 表示 regular-file payload 全部外置；`OUTPUT_INDEX`
本身仍保存 superblock、device table、inodes、xattrs、目录和符号链接数据。

本模式不使用 `--incremental`，不修改 `PARENT_INDEX`，也不把 `STORE`
发布为运行时依赖。

初始基线使用同样的固定 chunk 几何创建：

```shell
mkfs.erofs \
  -Eforce-chunk-indexes \
  --chunksize=4096 \
  --blobdev=M0.blob \
  J0.erofs BASE_DIRECTORY
```

`BASE_DIRECTORY` 中应包含后续 file-delta 要更新的固定大小、chunk 对齐
TARGET。该命令
把所有 regular-file payload 放入 `M0.blob`，使 `J0.erofs` 满足 index-only
parent 约束。

## 2. 接口语义

`--file-delta` 只描述逻辑更新：

```text
dm-persistent:TARGET:STORE
```

对 `STORE` 中的每条 exception：

```text
old_chunk -> new_chunk
```

`old_chunk` 标识 `TARGET` 中需要替换的逻辑 chunk，`new_chunk` 标识新内容
在 `STORE` 中的位置。构建器从 `STORE` 读取该 payload，把它紧凑写入
`DELTA_BLOB`，再把 `OUTPUT_INDEX` 中对应的 chunk index 更新为
`DELTA_BLOB` 的新位置。

`STORE` 中的 `new_chunk` 地址不会写入最终 index。它只用于读取输入；
最终物理地址由 payload 在 `DELTA_BLOB` 中的追加位置决定。

file-delta rebuild 是通用 rebuild 的特例：通用 rebuild 可以把 source image
本身注册为 external device；本模式不会这样做，而是只复制 parent device
table 和逐 chunk 映射，历史 index 镜像本身绝不成为数据 device。

dm-persistent v1 的 header 和 exception 字段均为 little-endian。header 由
`magic`、`valid`、`version`、`chunk_size` 四个 32-bit 字段组成；合法值为
`magic=0x70416e53`、`valid=1`、`version=1`，`chunk_size` 单位为
512-byte sectors。令 `C = chunk_size * 512`，则每个 metadata area 是一个
`C`-byte chunk，包含 `C / 16` 个 `{u64 old_chunk, u64 new_chunk}` 条目；令
`stride = C / 16 + 1`，第 `a` 个 metadata chunk 编号为
`1 + a * stride`。payload 字节偏移为 `new_chunk * C`，任何
`new_chunk % stride == 1` 都非法。`new_chunk=0` 终止 exception table。
解析器按 `old_chunk` 排序并拒绝重复、越界 payload、非法 metadata 引用和
缺少终止条目的 STORE。

## 3. 输入约束

本实现有意只支持单一、明确的构建形态，不保留旧 file-delta 行为：

1. 必须提供且只提供一个 EROFS rebuild source，即 `PARENT_INDEX`；
2. 必须同时提供 `-Eforce-chunk-indexes`、`--chunksize`、`--blobdev` 和至少一个
   `--file-delta`；`-Eforce-chunk-indexes` 同时禁止连续 chunks 被自动合并，
   从而保证 `--chunksize` 是稳定的累计映射粒度；
3. 不得使用 `--incremental`；
4. rebuild data mode 必须为默认的 blob-index 模式；
   `--xattr-inode-digest` 以及带 inode-digest compat feature 的 parent 均不支持，
   因为本模式不打开历史 blobs，无法为完整累计文件重新计算内容摘要；
5. `TARGET` 必须已存在、非空、是非硬链接 regular file，并使用带 device ID
   的 `CHUNK_BASED` 布局；
6. `TARGET` chunk size、dm chunk size 和 `--chunksize` 必须完全相等；
   dm-persistent 输入的 chunk 最大为 64 MiB；命令行会在创建或打开
   `OUTPUT_INDEX`/`DELTA_BLOB` 之前拒绝更大的 `--chunksize`；
7. `TARGET` 大小必须是 chunk size 的整数倍；
8. parent 中所有非空 regular file 都必须是带 device ID 的 `CHUNK_BASED`
   inode；每个已映射 chunk 必须显式引用 parent device table 中的 external
   device，不能引用 parent primary device，且映射范围不能超出该 device slot
   声明的 blocks；
9. 调用方负责保证 `OUTPUT_INDEX`、`PARENT_INDEX`、`DELTA_BLOB` 和所有
   `STORE` 不会发生破坏性路径/对象别名；mkfs 不为本模式额外执行对象身份
   判定。输入内容在完整构建期间不可变，两个输出由本次构建独占写入；同一
   STORE 可以由多个兼容 TARGET 复用。`DELTA_BLOB` 必须是可 seek 的普通文件
   或块设备，字符设备、FIFO 等对象会在写入前被拒绝；`STORE` 同样只接受
   普通文件或块设备，并以 nonblocking open 完成类型校验，避免 FIFO 在校验
   前阻塞；
10. `TARGET` 必须已存在且大小固定；本模式不创建、删除、重命名或调整文件
    大小，也不合并额外的目录树。
11. exception table 中 `old_chunk` 必须唯一；解析器会排序 exception 并拒绝
    重复逻辑 chunk、越界 payload、非法 metadata chunk 和未终止的 metadata。
12. `TARGET` 不能包含冒号；`STORE` 是第二个冒号之后的全部剩余文本，因此
    STORE 路径可以包含冒号。
13. 调用方负责证明 STORE 与指定 TARGET、parent generation 和 cutoff 的
    血缘匹配；dm 格式校验和 chunk 几何相等不能证明 lineage 正确。
14. mkfs 只读取 parent index metadata，不打开 parent device table 对应的
    blobs；调用方负责保证这些继承 blobs 已存在、大小和身份未变，并在发布
    前完成闭包校验。
15. 输出最多包含 `EROFS_MAX_BLOB_DEVS`（当前为 256）个 external devices；
    parent 已有 256 个 device slots 时不能再追加新 blob。

这些约束使 parent 成为真正的 index-only 镜像，并保证每一代运行时依赖
只包含实际数据 blobs，而不包含历史 index 镜像本身。

## 4. 输出结构

假设 parent 已有 `N` 个 external devices：

```text
device 1..N   = 原样继承 parent device table
device N + 1  = 本代 DELTA_BLOB
```

若 parent 使用 48-bit block/device fields，输出继承该 incompat feature；若
新写入的 blob chunk 首次越过 32-bit block address，inode fixup 也会同步设置
输出 superblock 的 48-bit feature，避免 device slot 高位被 reader 忽略。
parent feature 会在 importer 时间编码初始化前传播，因此 compact inode 的
mtime 不会因 late 48-bit transition 发生 32-bit 下溢；落在 48-bit compact
时间窗口之外或纳秒部分不能由全局 `fixed_nsec` 表示的 inherited mtime 会改用
extended inode 保存。

`OUTPUT_INDEX` 的 primary device 只保存 EROFS 元数据、目录数据、符号链接
数据和 chunk indexes，不保存 regular-file payload。

因为本模式只有一个无歧义 parent，输出 root inode 继承 parent root 的
mode、uid、gid、mtime、xattrs、opaque/whiteout 状态；目录数据和 nlink 仍由
输出树重新生成。空的非 TARGET regular inode 统一规范化为 `FLAT_PLAIN`
hole，不保留无意义的空 `CHUNK_BASED` layout。

`DELTA_BLOB` 按构建器遍历 TARGET 和排序后 exceptions 的内部顺序紧凑保存
非零 dirty chunks；该物理顺序不是接口 ABI。相同 payload 提交给 blob
writer 后可以在本代 blob 内去重，因此不保证每条 exception 都追加一个新块；
全零 payload 不写入 blob，而在 index 中
表示为 `EROFS_NULL_ADDR`。

本实现不压缩 device table：即使某个历史 blob 已不再被任何 chunk 引用，
它的 slot 仍原样保留，以保证后续 device ID 不变。因此恢复端当前必须按
device table 顺序注册全部 slots。本文将 chunk indexes 实际可达的数据集合
称为 `ReachablePayloads(OUTPUT_INDEX)`，将部署时必须按顺序提供的完整 slot
集合称为 `RuntimeDeviceBindings(OUTPUT_INDEX)`；当前后者可能严格大于前者，
不能用 `ReachablePayloads` 做发布闭包或 GC。device-table compaction 属于
后续能力。

## 5. 累计映射算法

对每个从 parent rebuild 的非空 regular file：

1. 读取 parent inode 的 chunk size 和 chunk indexes；
2. 为 output inode 分配同样数量的 8-byte chunk indexes；
3. 对 hole 原样生成 hole；
4. 对已映射 chunk 原样复制 `{device_id, physical_block}`；
5. 如果该 inode 不是 file-delta TARGET，映射构建完成；
6. 如果该 inode 是 TARGET，遍历对应 dm exceptions：
   1. 从 `STORE[new_chunk]` 读取一个完整 chunk；
   2. 将 payload 交给 blob chunk writer；
   3. 若 payload 全零，将 `old_chunk` 更新为 hole；
   4. 否则将 `old_chunk` 更新为
      `{device_id = N + 1, physical_block = appended_block}`。

非 TARGET inode 保留它自己的 parent chunk size，不要求与命令行
`--chunksize` 相同；`--chunksize` 只约束本命令所有 TARGET、STORE 和新写入
`DELTA_BLOB` 的粒度。

公式为：

```text
Jnew[b] = location(DELTA_BLOB, compact_payload(b))  if b is dirty and non-zero
          hole                                      if b is dirty and zero
          Jparent[b]                                otherwise
```

一次命令可以重复指定 `--file-delta`，但同一 `TARGET` 只能出现一次。
TARGET 成功匹配、校验并完成 overlay routine 即视为已应用；exception 数量
可以为零，此时仍成功生成带一个新空 device slot 的累计 index。

## 6. 示例

parent 映射为：

```text
/memory/slot0.img:
  b0 -> device 1:block 10
  b1 -> device 1:block 11
  b2 -> device 2:block 20
  b3 -> device 2:block 21
```

本代 STORE 包含：

```text
old_chunk=1 -> new_chunk=8, payload=X
old_chunk=3 -> new_chunk=9, payload=ZERO
```

如果 parent 有两个 devices，则 `DELTA_BLOB` 是 device 3。输出为：

```text
DELTA_BLOB:
  block 0 = X

OUTPUT_INDEX /memory/slot0.img:
  b0 -> device 1:block 10
  b1 -> device 3:block 0
  b2 -> device 2:block 20
  b3 -> hole
```

`STORE` 的 `new_chunk=8/9` 只用于读取输入，不出现在输出映射中。

## 7. 发布与恢复

发布顺序必须是：

1. 完成并封存 `DELTA_BLOB`；
2. 完成 `OUTPUT_INDEX`；
3. 发布 `DELTA_BLOB`；
4. 发布 `OUTPUT_INDEX`；
5. 最后发布快照 manifest；manifest 本身包含 index device ordinal 到所有
   已发布 blob digest 的解析表，不存在独立的解析 sidecar。

恢复端按 `RuntimeDeviceBindings(OUTPUT_INDEX)` 的顺序注册 parent blobs 和本代 blob。
历史 index 不是数据依赖。由于当前不压缩 device table，发布 manifest 必须
为每个保留 slot 提供 blob 绑定，包括已经没有 chunk 引用的历史 slot。
只有在未来重写 index 并重编号 device IDs 后，才可以从运行时绑定和发布
闭包中删除这些 slots。

例如可用以下方式校验一代两设备的结果：

```shell
fsck.erofs \
  --device=M0.blob \
  --device=M1.blob \
  J1.erofs
```

如果本代所有更新均为全零或没有 exception，`DELTA_BLOB` 可以是 0 字节，
但本实现仍为它追加一个 device slot；发布和恢复时仍需提供这个空 blob。

## 8. 故障与原子性

- `PARENT_INDEX` 和所有 `STORE` 只读打开；
- `OUTPUT_INDEX` 和 `DELTA_BLOB` 是新文件；
- 任一输入校验失败时，构建失败，不产生可发布的 index；
- `DELTA_BLOB` 在成功前只是临时产物；上层发布者应使用临时路径加原子重命名；
- 调用方负责保证输出路径不会与输入或另一输出发生破坏性别名；
- 任一 file-delta 未在 parent 树中匹配并完成 overlay routine 时，构建失败；
  STORE 含零条 exception 时，TARGET 匹配且校验完成即算成功应用。

CLI 沿用 mkfs 的覆盖行为，可能截断已存在的输出并在失败后留下部分文件；
“不产生可发布产物”不表示自动删除路径。mkfs 不额外检测路径或对象别名，
调用方必须传入彼此独立的临时路径，在 mkfs 成功、fsync 和校验完成后原子
重命名。

## 9. 非目标

本次实现不支持：

- 旧的 local-directory `--incremental --file-delta` 完整文件合并行为；
- 多个 rebuild parents；
- parent primary-device regular-file 数据；
- compressed、flat、inline 或无 device ID 的 TARGET；
- dm chunk 与 EROFS chunk 的拆分、合并或边界补全；
- 在同一命令中生成可独立挂载的 EROFS 数据镜像；
- 自动发布 OCI artifacts 或管理 snapshot lineage；
- 运行时按内容摘要自动发现 external devices。

on-disk `extra_devices` 字段虽为 16 bit，本实现仍受
`EROFS_MAX_BLOB_DEVS` 的 256-slot fd table 限制；parent 已有 256 个 devices
时即拒绝追加新 blob。

## 10. 验证要求

实现至少验证以下场景：

1. 单个 dirty chunk 只向新 blob 写一个 chunk；
2. clean chunks 保持 parent device ID 和物理块地址；
3. dirty zero 生成 hole 且不增长 blob；
4. 连续两代构建能继承包含多个 external devices 的 parent；
5. 无效 dm header、越界 exception、chunk size 不匹配和缺失 TARGET 均失败；
6. 零 exception 的有效 STORE 成功生成不含 payload 的新一代；
7. parent chunk 映射越过其 device slot 末尾时构建失败；
8. 大于 4 GiB 的逻辑偏移仍继承正确 clean chunk，48-bit parent feature 不丢失，
   高于 `UINT32_MAX` 的 build epoch 不改变较老的 inherited mtime，且
   `--ignore-mtime` 使用完整 build time 而不是 compact-time epoch base；
9. 非 TARGET inode 保留与 `--chunksize` 不同的 parent chunk size；
10. 512/1024/2048-byte parent block size 无需重复传入 `-b`；
11. root mode/uid/gid/mtime/xattrs 被继承，空 chunk-based 非 TARGET 被规范化；
12. ILP32 上内存 chunk pointer array 使用 pointer stride，并独立序列化为
    8-byte on-disk indexes；
13. 使用 `fsck.erofs --device=...` 能读取并校验最终逻辑文件内容。
14. 同一命令重复两个 `--file-delta` 时，非零和全零更新分别落入共享新 blob
    和 hole，两个 TARGET 均能正确恢复；字符设备不能作为 `DELTA_BLOB`，FIFO
    不能作为 `STORE` 或 parent，inode-digest 交叉功能会被拒绝；非法
    chunksize 不触发未定义移位，超过 64 MiB 的 file-delta chunksize 在输出
    创建前失败，无法表示的超大 chunk-index 数组不会发生整数截断。
15. 旧 local-directory/incremental 形式和多个 rebuild parents 均被拒绝。
