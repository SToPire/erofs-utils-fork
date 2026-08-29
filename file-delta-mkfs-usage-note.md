# mkfs.erofs file-delta 使用提醒

## 1. 首次创建基线

```bash
mkfs.erofs \
  -Eforce-chunk-indexes \
  --chunksize=4096 \
  --blobdev=M0.blob \
  J0.erofs \
  BASE_DIRECTORY
```

例如本地目录中存在：

```text
BASE_DIRECTORY/
└── memory/
    └── slot0.img
```

生成：

```text
J0.erofs    初始 index-only EROFS
M0.blob     初始 regular-file payload
```

`J0.erofs` 中 `/memory/slot0.img` 的每个 chunk 都显式指向 `M0.blob`：

```text
chunk 0 -> device 1, block ...
chunk 1 -> device 1, block ...
```

如果需要 512-byte block/chunk：

```bash
mkfs.erofs \
  -b512 \
  -Eforce-chunk-indexes \
  --chunksize=512 \
  --blobdev=M0.blob \
  J0.erofs BASE_DIRECTORY
```

## 2. 根据本代 dirty pages 构建新一代

```bash
mkfs.erofs \
  -Eforce-chunk-indexes \
  --chunksize=4096 \
  --blobdev=M1.blob \
  --file-delta=dm-persistent:/memory/slot0.img:epoch1.cow \
  J1.erofs \
  J0.erofs
```

位置参数顺序是：

```text
J1.erofs    OUTPUT_INDEX
J0.erofs    PARENT_INDEX
```

不需要 `--rebuild`。因为 SOURCE 是 EROFS 镜像，mkfs 会自动进入 rebuild mode。

参数含义：

```text
/memory/slot0.img
    parent EROFS 内的 TARGET 路径，不是 Host 文件路径

epoch1.cow
    Host 上已封存的 dm-persistent v1 exception store

M1.blob
    本代新 external blob，只保存本代非零 dirty chunks

J1.erofs
    本代新累计 index

J0.erofs
    上一代只读累计 index
```
