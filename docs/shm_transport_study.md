# CyberRT SHM Transport 学习笔记

本文聚焦 CyberRT 中共享内存通信链路，重点解释：

- `ShmTransmitter` 如何发送消息
- `ShmDispatcher` 如何接收并分发消息
- `Segment / ReadableInfo / MessageInfo` 在闭环里的作用
- `Reader -> Receiver -> Dispatcher -> ListenerHandler -> 用户回调` 在 SHM 场景下如何串起来

---

## 1. 总体闭环

先看最核心的一条链路：

```text
Writer::Write()
  -> ShmTransmitter::Transmit(msg, msg_info)
  -> Segment::AcquireBlockToWrite(...)
  -> 把消息体写入共享内存 block
  -> 把 MessageInfo 紧跟写在消息体后面
  -> ReleaseWrittenBlock()
  -> notifier_->Notify(ReadableInfo)

ShmDispatcher 后台线程
  -> notifier_->Listen(..., &readable_info)
  -> 根据 channel_id / block_index 找到对应 Segment
  -> Segment::AcquireBlockToRead(...)
  -> 从 block 中读出消息体 + MessageInfo
  -> OnMessage(...)
  -> ListenerHandler<ReadableBlock>::Run(...)
  -> adapter 反序列化成 MessageT
  -> Receiver::OnNewMessage(...)
  -> 后续进入 Reader / DataDispatcher / 用户回调
```

一句话概括：

> 数据走共享内存，事件走 notifier 通知。

---

## 2. 核心角色

### 2.1 `ShmTransmitter`

定义：`cyber/transport/transmitter/shm_transmitter.h:46`

职责：

- 为某个 `channel_id` 准备共享内存段
- 将消息写入共享内存 block
- 将 `MessageInfo` 一起写入 block
- 发送 `ReadableInfo` 通知接收端

---

### 2.2 `Segment`

定义：`cyber/transport/shm/segment.h:43`

职责：

- 管理某个 channel 对应的共享内存区域
- 提供读写 block 的接口
- 负责 block 的读写锁
- 支持 shm remap / recreate

可以把它理解成：

> 某个 channel 的共享内存 block 管理器。

---

### 2.3 `ReadableInfo`

定义：`cyber/transport/shm/readable_info.h:32`

它不是消息数据，而是一条“可读通知描述符”，主要包含：

- `host_id`
- `channel_id`
- `block_index`
- `arena_block_index`

关键字段见：`cyber/transport/shm/readable_info.h:45-56`

含义是：

> 告诉接收端，哪个 host 的哪个 channel 上，哪个共享内存 block 已经写好了。

---

### 2.4 `MessageInfo`

定义：`cyber/transport/message/message_info.h:30`

核心字段：

- `sender_id_`
- `channel_id_`
- `seq_num_`
- `spare_id_`
- `send_time_`

在 SHM 场景里，它会和消息体一起写入 block 尾部。

---

### 2.5 `ShmDispatcher`

定义：`cyber/transport/dispatcher/shm_dispatcher.h:50`

职责：

- 后台线程监听 notifier
- 收到 `ReadableInfo` 后读取共享内存 block
- 解析 `MessageInfo`
- 把消息路由给 `ListenerHandler`
- 最终交给 `Receiver`

---

## 3. 发送端：`ShmTransmitter` 发送流程

## 3.1 构造时的初始化

位置：`cyber/transport/transmitter/shm_transmitter.h:96-108`

构造时做了两件关键事：

1. 计算 `host_id_`
2. 判断是否开启 `arena_transmit_`

```cpp
host_id_ = common::Hash(attr.host_ip());
arena_transmit_ = GlobalData::Instance()->IsChannelEnableArenaShm(...)
                  && 非 RawMessage
                  && 非 PyMessageWrap;
```

这意味着发送器有两种模式：

- 普通 SHM 序列化发送
- arena SHM 发送

---

## 3.2 Enable 时创建资源

位置：`cyber/transport/transmitter/shm_transmitter.h:159-183`

```cpp
segment_ = SegmentFactory::CreateSegment(channel_id_);
notifier_ = NotifierFactory::CreateNotifier();
this->enabled_ = true;
```

说明发送端依赖两种核心资源：

- `segment_`：数据面，共享内存
- `notifier_`：控制面，通知机制

---

## 3.3 发送入口：`Transmit(msg, msg_info)`

位置：`cyber/transport/transmitter/shm_transmitter.h:201`

发送流程一开始先构造通知对象：

```cpp
readable_info.set_host_id(host_id_);
readable_info.set_channel_id(channel_id_);
readable_info.set_arena_block_index(-1);
readable_info.set_block_index(-1);
```

初始值 `-1` 表示当前还没有成功写入任何 block。

---

## 4. 普通 SHM 路径

当 `arena_transmit_ == false` 时，发送逻辑走普通 block：

位置：`cyber/transport/transmitter/shm_transmitter.h:290-314`

### 4.1 申请可写 block

```cpp
std::size_t msg_size = message::ByteSize(msg);
segment_->AcquireBlockToWrite(msg_size, &wb)
```

底层实现：`cyber/transport/shm/segment.cc:40-69`

主要做这些事：

1. shm 尚未创建则 `OpenOrCreate()`
2. 如果需要 remap，则 `Remap()`
3. 如果消息太大超过当前 shm 配置，则 `Recreate(msg_size)`
4. 选择下一个可写 block
5. 返回 block 和 buffer 地址

block 选择逻辑见：`cyber/transport/shm/segment.cc:295-303`

```cpp
uint32_t try_idx = state_->FetchAddSeq(1) % block_num;
if (blocks_[try_idx].TryLockForWrite()) {
  return try_idx;
}
```

本质上是“轮转选块 + 写锁竞争”。

---

### 4.2 写入消息体

```cpp
message::SerializeToArray(msg, wb.buf, static_cast<int>(msg_size));
wb.block->set_msg_size(msg_size);
```

此时 block 前半部分存的是序列化后的消息体。

---

### 4.3 写入 `MessageInfo`

位置：`cyber/transport/transmitter/shm_transmitter.h:305-311`

```cpp
char* msg_info_addr = reinterpret_cast<char*>(wb.buf) + msg_size;
msg_info.SerializeTo(msg_info_addr, MessageInfo::kSize);
wb.block->set_msg_info_size(MessageInfo::kSize);
```

共享内存中的布局就是：

```text
[ serialized message ][ serialized MessageInfo ]
```

这是理解 SHM 收发闭环的关键。

---

### 4.4 释放写锁

```cpp
segment_->ReleaseWrittenBlock(wb);
```

底层实现：`cyber/transport/shm/segment.cc:91-97`

```cpp
blocks_[index].ReleaseWriteLock();
```

意味着：

> 只有消息体和 `MessageInfo` 都写完了，这个 block 才对读端可见。

---

### 4.5 记录 block 索引并发送通知

```cpp
readable_info.set_block_index(wb.index);
return notifier_->Notify(readable_info);
```

到这里，普通 SHM 发送结束。

---

## 5. arena SHM 路径

当 `arena_transmit_ == true` 时，发送逻辑走 arena 分支：

位置：`cyber/transport/transmitter/shm_transmitter.h:216-289`

### 5.1 申请 arena block

```cpp
segment_->AcquireArenaBlockToWrite(msg_size, &arena_wb)
```

对应实现：`cyber/transport/shm/segment.cc:72-89`

与普通 block 类似，只是使用 `arena_blocks_`。

---

### 5.2 构造 arena message wrapper

```cpp
auto msg_wrapper = arena_manager->CreateMessageWrapper();
arena_manager->SetMessageChannelId(msg_wrapper.get(), channel_id_);
message::SerializeToArenaMessageWrapper(msg, msg_wrapper.get(), &msg_p)
```

说明 arena 路径不是简单字节序列化，而是把 protobuf message 组织成 arena 管理结构。

---

### 5.3 将 wrapper 数据拷入 arena block

```cpp
memcpy(arena_wb.buf, msg_wrapper->GetData(), msg_size);
arena_wb.block->set_msg_size(msg_size);
```

---

### 5.4 再写入 `MessageInfo`

```cpp
char* msg_info_addr = reinterpret_cast<char*>(arena_wb.buf) + msg_size;
msg_info.SerializeTo(msg_info_addr, MessageInfo::kSize);
arena_wb.block->set_msg_info_size(MessageInfo::kSize);
```

布局仍然是：

```text
[ arena wrapper bytes ][ serialized MessageInfo ]
```

---

### 5.5 填充 `arena_block_index`

```cpp
readable_info.set_arena_block_index(arena_wb.index);
```

---

### 5.6 若存在普通 receiver，则额外写一个普通 block

位置：`cyber/transport/transmitter/shm_transmitter.h:262-287`

如果 `serialized_receiver_count_ > 0`，则 arena 发送之外，还会额外再写一份普通序列化 block。

所以 arena 模式下有两种情况：

- 只有 arena receiver：只填 `arena_block_index`
- 同时有普通 receiver：同时填 `arena_block_index` 和 `block_index`

即一次通知可能同时指向两份数据。

---

## 6. 接收端：`ShmDispatcher` 如何收取消息

## 6.1 初始化时启动后台线程

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:220-227`

```cpp
host_id_ = common::Hash(GlobalData::Instance()->HostIp());
notifier_ = NotifierFactory::CreateNotifier();
thread_ = std::thread(&ShmDispatcher::ThreadFunc, this);
```

这与发送端形成对偶：

- 发送端：`Notify(readable_info)`
- 接收端：`Listen(timeout, &readable_info)`

---

## 6.2 AddListener 时准备 segment

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:49-59`

```cpp
auto segment = SegmentFactory::CreateSegment(channel_id);
segments_[channel_id] = segment;
previous_indexes_[channel_id] = UINT32_MAX;
arena_previous_indexes_[channel_id] = UINT32_MAX;
```

说明只有当本进程确实有人监听某个 channel，Dispatcher 才会准备这个 channel 对应的 segment。

---

## 6.3 后台线程监听通知

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:151-157`

```cpp
while (!is_shutdown_.load()) {
  if (!notifier_->Listen(100, &readable_info)) {
    continue;
  }
}
```

---

### 6.3.1 先过滤 host_id

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:159-162`

```cpp
if (readable_info.host_id() != host_id_) {
  continue;
}
```

因为共享内存只适用于同机通信。

---

### 6.3.2 取出通知里的 block 索引

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:164-166`

```cpp
uint64_t channel_id = readable_info.channel_id();
int32_t block_index = readable_info.block_index();
int32_t arena_block_index = readable_info.arena_block_index();
```

---

### 6.3.3 按需读取普通 block / arena block

- 普通 block：`cyber/transport/dispatcher/shm_dispatcher.cc:174-194`
- arena block：`cyber/transport/dispatcher/shm_dispatcher.cc:196-215`

即：

- `block_index != -1` -> `ReadMessage(channel_id, block_index)`
- `arena_block_index != -1` -> `ReadArenaMessage(channel_id, arena_block_index)`

所以一次通知可能触发两次读取。

---

## 7. `ReadMessage()` 读取普通 block

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:61-85`

### 7.1 根据索引申请可读 block

```cpp
rb->index = block_index;
segments_[channel_id]->AcquireBlockToRead(rb.get())
```

底层实现：`cyber/transport/shm/segment.cc:107-136`

流程是：

1. 如果 shm 没打开，则 `OpenOnly()`
2. 如有 remap 需求则 `Remap()`
3. block 加读锁
4. 返回 block 与 buffer 地址

---

### 7.2 从消息尾部取出 `MessageInfo`

```cpp
const char* msg_info_addr =
    reinterpret_cast<char*>(rb->buf) + rb->block->msg_size();

msg_info.DeserializeFrom(msg_info_addr, rb->block->msg_info_size())
```

这与发送端写入布局严格一一对应：

```text
发送端： [ message ][ MessageInfo ]
接收端： buf + msg_size -> MessageInfo 起始位置
```

---

### 7.3 转入分发逻辑

```cpp
OnMessage(channel_id, rb, msg_info);
```

---

### 7.4 释放读锁

```cpp
segments_[channel_id]->ReleaseReadBlock(*rb);
```

实现见：`cyber/transport/shm/segment.cc:177-183`

---

## 8. `OnMessage()` 如何继续往下走

位置：`cyber/transport/dispatcher/shm_dispatcher.cc:113-123`

```cpp
if (msg_listeners_.Get(channel_id, &handler_base)) {
  auto handler = std::dynamic_pointer_cast<ListenerHandler<ReadableBlock>>(
      *handler_base);
  handler->Run(rb, msg_info);
}
```

注意此时分发的还不是最终业务消息，而是：

- `ReadableBlock`
- `MessageInfo`

---

## 9. `ListenerHandler` 做真正的消息路由

位置：`cyber/transport/message/listener_handler.h:163-173`

```cpp
signal_(msg, msg_info);
uint64_t oppo_id = msg_info.sender_id().HashValue();
...
(*signals_[oppo_id])(msg, msg_info);
```

作用：

1. 广播给该 channel 的普通监听者
2. 如有需要，按 `sender_id` 做定向分发

---

## 10. adapter 将 `ReadableBlock` 转回 `MessageT`

普通 SHM adapter 定义在：`cyber/transport/dispatcher/shm_dispatcher.h:238-265`

核心逻辑：

```cpp
auto msg = std::make_shared<MessageT>();
message::ParseFromArray(rb->buf, rb->block->msg_size(), msg.get());
listener(msg, msg_info);
```

因此：

> `ShmDispatcher` 本身只分发共享内存块，真正的业务消息恢复由 adapter 完成。

---

## 11. 最终进入 `Receiver::OnNewMessage()`

`ShmReceiver::Enable()` 注册的是：

- `cyber/transport/receiver/shm_receiver.h:78-80`

```cpp
dispatcher_->AddListener<M>(
    this->attr_, std::bind(&ShmReceiver<M>::OnNewMessage, this,
                           std::placeholders::_1, std::placeholders::_2));
```

最终调用：

- `cyber/transport/receiver/receiver.h:61-65`

```cpp
msg_listener_(msg, msg_info, attr_);
```

然后再进入更上层的：

- `Reader`
- `DataDispatcher`
- `DataVisitor`
- 用户回调

---

## 12. SHM 完整时序图（普通 block）

```text
[发送端]
ShmTransmitter::Transmit(msg, msg_info)
  -> Segment::AcquireBlockToWrite(msg_size, &wb)
  -> SerializeToArray(msg, wb.buf)
  -> wb.block->set_msg_size(msg_size)
  -> msg_info.SerializeTo(wb.buf + msg_size, MessageInfo::kSize)
  -> wb.block->set_msg_info_size(MessageInfo::kSize)
  -> Segment::ReleaseWrittenBlock(wb)
  -> readable_info.set_block_index(wb.index)
  -> notifier_->Notify(readable_info)

[接收端]
ShmDispatcher::ThreadFunc()
  -> notifier_->Listen(..., &readable_info)
  -> ReadMessage(channel_id, block_index)
  -> Segment::AcquireBlockToRead(&rb)
  -> msg_info.DeserializeFrom(rb.buf + rb.block->msg_size(), ...)
  -> OnMessage(channel_id, rb, msg_info)
  -> ListenerHandler<ReadableBlock>::Run(rb, msg_info)
  -> adapter: ParseFromArray(rb->buf, rb->block->msg_size(), msg.get())
  -> Receiver::OnNewMessage(msg, msg_info)
  -> 后续 Reader / 用户回调
```

---

## 13. SHM 完整时序图（arena block）

```text
[发送端]
ShmTransmitter::Transmit(msg, msg_info)
  -> Segment::AcquireArenaBlockToWrite(..., &arena_wb)
  -> SerializeToArenaMessageWrapper(...)
  -> memcpy(arena_wb.buf, wrapper_data, fixed_size)
  -> 写入 MessageInfo
  -> readable_info.set_arena_block_index(arena_wb.index)

  -> [如果存在 serialized receiver]
       Segment::AcquireBlockToWrite(...)
       SerializeToArray(...)
       写入 MessageInfo
       readable_info.set_block_index(wb.index)

  -> ReleaseArenaWrittenBlock(...)
  -> [可选] ReleaseWrittenBlock(...)
  -> notifier_->Notify(readable_info)

[接收端]
ShmDispatcher::ThreadFunc()
  -> notifier_->Listen(..., &readable_info)
  -> 如果 arena_block_index != -1 -> ReadArenaMessage(...)
  -> 如果 block_index != -1 -> ReadMessage(...)
  -> 各自进入 ListenerHandler
  -> arena adapter 用 ParseFromArenaMessageWrapper(...)
  -> serialized adapter 用 ParseFromArray(...)
  -> Receiver::OnNewMessage(...)
```

---

## 14. 这一套设计的关键思想

### 14.1 数据面和控制面分离

- 数据面：共享内存 block
- 控制面：notifier + `ReadableInfo`

优点：大对象不需要通过通知机制传输，只需要传一个很小的索引描述符。

---

### 14.2 消息体和 `MessageInfo` 共存于同一个 block

无论普通 shm 还是 arena shm，发送端都会把：

- 消息内容
- `MessageInfo`

连续写入同一块共享内存。

所以接收端只要知道：

- `buf`
- `msg_size`
- `msg_info_size`

就能拆出两部分。

---

### 14.3 arena / serialized 双轨兼容

arena 模式不是强制所有接收者都走 arena。

如果存在 RawMessage 或不支持 arena 的消费者，发送端还可以额外写一份普通序列化 block。

这使得系统兼容性更强。

---

### 14.4 block 锁保证并发安全

- 写端：`TryLockForWrite()` / `ReleaseWriteLock()`
- 读端：`TryLockForRead()` / `ReleaseReadLock()`

保证：

- 写入未完成时不会被读到
- 读取过程中 block 不会被写线程破坏

---

## 15. 最终总结

一句话总结这条 SHM 闭环：

> `ShmTransmitter` 负责“把消息 + MessageInfo 写入共享内存，并通过 notifier 发送一个 block 索引通知”；`ShmDispatcher` 负责“监听通知、按索引读回共享内存 block、还原出 MessageInfo 和消息体，并继续分发到 Receiver / Reader / 用户回调”。

如果你后续继续学习，建议按这个顺序看源码：

1. `cyber/transport/transmitter/shm_transmitter.h`
2. `cyber/transport/shm/segment.h`
3. `cyber/transport/shm/segment.cc`
4. `cyber/transport/shm/readable_info.h`
5. `cyber/transport/dispatcher/shm_dispatcher.h`
6. `cyber/transport/dispatcher/shm_dispatcher.cc`
7. `cyber/transport/message/listener_handler.h`
8. `cyber/transport/receiver/shm_receiver.h`
9. `cyber/node/reader.h`

这样从“发 -> 存 -> 通知 -> 收 -> 分发 -> 回调”会比较顺。