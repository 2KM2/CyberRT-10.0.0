# CyberRT 协程调度器学习笔记

本文聚焦 CyberRT 的 **协程调度器设计**，解释：

- 为什么消息到达后不直接执行用户回调
- `CRoutine / RoutineContext / Scheduler / Processor / DataVisitor` 的分工
- Reader 回调如何被包装成协程
- 消息到达后如何唤醒协程并在 Processor 线程中执行
- 这套设计相对“直接回调”的优势是什么

---

## 1. 先给一个总认识

CyberRT 的执行模型不是：

```text
消息来了 -> 直接执行用户回调
```

而是：

```text
消息来了
  -> DataDispatcher / DataVisitor 标记“有数据可取”
  -> 唤醒一个 CRoutine
  -> Scheduler 把这个协程交给某个 Processor
  -> Processor 在线程上 Resume 协程
  -> 协程执行用户逻辑
  -> 没数据了再 Yield 挂起
```

所以这套系统的本质是：

> **事件驱动 + 协程调度 + 多线程执行器**

---

## 2. 为什么 CyberRT 要做协程调度器

如果 Reader 收到消息后直接在 transport 线程里执行用户回调，会有几个问题：

1. **通信线程和业务线程耦合**
   - 用户回调一慢，就会拖慢 transport 接收线程

2. **多路消息融合困难**
   - 多输入组件需要等待多个 channel 的消息组合之后再执行

3. **缺少统一调度能力**
   - 难以统一做优先级控制、绑核、线程池分配

4. **挂起等待不优雅**
   - 没数据时只能空转或阻塞，不够灵活

所以 CyberRT 的思路是：

> **把“消息到达”和“业务执行”彻底解耦。**
>
> 消息到达只负责通知，真正的处理逻辑封装为协程，由调度器统一安排运行。

---

## 3. 这套设计里的核心角色

---

## 3.1 `CRoutine`

定义在：

- `cyber/croutine/croutine.h:40`

`CRoutine` 就是 CyberRT 的“协程对象”。

你可以把它理解成：

> 一个可被挂起 / 恢复执行的轻量任务。

它内部包含：

- 协程函数 `func_`
- 协程状态 `state_`
- 协程上下文 `RoutineContext`
- 协程 id / name / priority / processor_id 等调度属性

---

## 3.2 `RoutineContext`

定义在：

- `cyber/croutine/detail/routine_context.h:42`

```cpp
struct RoutineContext {
  char stack[STACK_SIZE];
  char* sp = nullptr;
};
```

它本质上就是：

- 协程私有栈
- 栈指针

并提供上下文切换接口：

- `SwapContext(...)`
  `cyber/croutine/detail/routine_context.h:53-55`

```cpp
ctx_swap(reinterpret_cast<void**>(src_sp), reinterpret_cast<void**>(dest_sp));
```

所以 CyberRT 的协程不是语言层 coroutine，而是：

> **用户态手工切换上下文的栈协程**

---

## 3.3 `Scheduler`

定义在：

- `cyber/scheduler/scheduler.h:58`

它是调度器抽象基类，负责：

- 创建任务
- 管理 `CRoutine`
- 分发任务到 Processor
- 响应通知并唤醒任务

关键接口：

- `CreateTask(...)`
- `NotifyTask(...)`
- `DispatchTask(...)`
- `NotifyProcessor(...)`
- `RemoveCRoutine(...)`

---

## 3.4 `Processor`

定义在：

- `cyber/scheduler/processor.h:45`

你可以把它理解成：

> **真正执行协程的工作线程。**

一个 `Processor` 对应一个线程。线程循环里不断从 `ProcessorContext` 取下一个可运行协程，并 `Resume()` 执行它。

---

## 3.5 `ProcessorContext`

定义在：

- `cyber/scheduler/processor_context.h:36`

核心接口：

```cpp
virtual std::shared_ptr<CRoutine> NextRoutine() = 0;
```

它的职责是：

> 提供“这个 Processor 接下来该跑哪个协程”。

因此：

- `Processor` 是执行器线程
- `ProcessorContext` 是任务来源 / 就绪队列接口

---

## 3.6 `DataVisitor`

定义在：

- `cyber/data/data_visitor.h`

`DataVisitor` 是 Reader / Component 与协程调度器之间的桥梁。

它负责：

- 从 DataDispatcher 管理的 channel buffer 中取消息
- 当有新数据到达时，通知 Scheduler 唤醒相应协程

---

## 4. `CRoutine` 是怎么工作的

---

## 4.1 协程创建时就分配独立栈

在：

- `cyber/croutine/croutine.cc:45-67`

```cpp
context_ = context_pool->GetObject();
...
MakeContext(CRoutineEntry, this, context_.get());
state_ = RoutineState::READY;
```

说明每个协程在创建时就有自己的运行栈，并且初始状态就是：

```text
READY
```

---

## 4.2 协程入口函数

在：

- `cyber/croutine/croutine.cc:38-42`

```cpp
void CRoutineEntry(void *arg) {
  CRoutine *r = static_cast<CRoutine *>(arg);
  r->Run();
  CRoutine::Yield(RoutineState::FINISHED);
}
```

含义是：

1. 跑真正的用户函数 `Run()`
2. 跑完后把状态置为 `FINISHED`
3. 切回调度线程

---

## 4.3 协程恢复执行

在：

- `cyber/croutine/croutine.cc:71-86`

```cpp
RoutineState CRoutine::Resume() {
  ...
  current_routine_ = this;
  SwapContext(GetMainStack(), GetStack());
  current_routine_ = nullptr;
  return state_;
}
```

意思是：

> Processor 线程把自己的主栈切换到这个协程的私有栈，协程开始或继续运行。

---

## 4.4 协程挂起

在：

- `cyber/croutine/croutine.h:125-133`

```cpp
inline void CRoutine::Yield(const RoutineState &state) {
  auto routine = GetCurrentRoutine();
  routine->set_state(state);
  SwapContext(GetCurrentRoutine()->GetStack(), GetMainStack());
}
```

也就是：

> 协程自己把状态改掉，然后主动切回主线程上下文。

---

## 4.5 协程状态

定义在：

- `cyber/croutine/croutine.h:38`

```cpp
enum class RoutineState { READY, FINISHED, SLEEP, IO_WAIT, DATA_WAIT };
```

含义：

- `READY`：可运行
- `FINISHED`：已结束
- `SLEEP`：睡眠等待
- `IO_WAIT`：等待 IO
- `DATA_WAIT`：等待数据

---

## 5. Reader 为什么是“协程化”的

这是理解 CyberRT 调度器最关键的点之一。

---

## 5.1 `Reader::Init()` 先创建 `DataVisitor`

在：

- `cyber/node/reader.h:297-298`

```cpp
auto dv = std::make_shared<data::DataVisitor<MessageT>>(
    role_attr_.channel_id(), pending_queue_size_);
```

说明 Reader 并不是直接从 transport 线程里消费消息，而是通过 `DataVisitor` 从数据缓存中取消息。

---

## 5.2 再基于 `DataVisitor` 生成 `RoutineFactory`

在：

- `cyber/node/reader.h:300-301`

```cpp
croutine::RoutineFactory factory =
    croutine::CreateRoutineFactory<MessageT>(std::move(func), dv);
```

这一步把“消息处理逻辑”包装成协程模板。

---

## 5.3 `CreateRoutineFactory` 里真正生成的是什么

最核心的代码在：

- `cyber/croutine/routine_factory.h:55-68`

```cpp
factory.create_routine = [=]() {
  return [=]() {
    std::shared_ptr<M0> msg;
    for (;;) {
      CRoutine::GetCurrentRoutine()->set_state(RoutineState::DATA_WAIT);
      if (dv->TryFetch(msg)) {
        f(msg);
        CRoutine::Yield(RoutineState::READY);
      } else {
        CRoutine::Yield();
      }
    }
  };
};
```

---

## 5.4 这段逻辑翻译成人话

一个 Reader 协程会一直循环：

1. 先把自己标记为 `DATA_WAIT`
2. 尝试从 `DataVisitor` 取一条消息
3. 如果取到了：
   - 执行用户逻辑 `f(msg)`
   - 执行完后 `Yield(READY)`
4. 如果没取到：
   - 直接 `Yield()` 挂起

所以 Reader 的回调不是“一次性函数”，而是一个：

> **长期驻留、循环消费消息的协程**

---

## 6. `DataVisitor` 在整个设计中的位置

---

## 6.1 `DataVisitorBase`

定义在：

- `cyber/data/data_visitor_base.h:33`

它内部持有：

- `Notifier`
- `DataNotifier*`

关键接口：

- `RegisterNotifyCallback(...)`
  `cyber/data/data_visitor_base.h:37-39`

```cpp
notifier_->callback = callback;
```

说明它支持注册一个“有新数据时我该通知谁”的回调。

---

## 6.2 单输入 `DataVisitor`

在：

- `cyber/data/data_visitor.h:168-193`

构造时做了两件关键事：

```cpp
DataDispatcher<M0>::Instance()->AddBuffer(buffer_);
data_notifier_->AddNotifier(buffer_.channel_id(), notifier_);
```

意思是：

1. 它把自己的消息 buffer 注册给 `DataDispatcher`
2. 又把 notifier 注册给 `DataNotifier`

于是消息链路变成：

```text
Receiver 收到消息
  -> DataDispatcher 把消息推进 buffer
  -> DataNotifier 触发 notifier 回调
  -> Scheduler 收到通知，唤醒对应协程
```

---

## 6.3 `TryFetch()` 的作用

在：

- `cyber/data/data_visitor.h:183-188`

```cpp
if (buffer_.Fetch(&next_msg_index_, m0)) {
  next_msg_index_++;
  return true;
}
return false;
```

本质上就是：

> **按顺序从 channel buffer 中取出新消息。**

---

## 7. Scheduler 如何把协程和数据通知接起来

---

## 7.1 创建任务

在：

- `cyber/scheduler/scheduler.cc:42-69`

```cpp
auto task_id = GlobalData::RegisterTaskName(name);

auto cr = std::make_shared<CRoutine>(func);
cr->set_id(task_id);
cr->set_name(name);

if (!DispatchTask(cr)) {
  return false;
}

if (visitor != nullptr) {
  visitor->RegisterNotifyCallback([this, task_id]() {
    ...
    this->NotifyProcessor(task_id);
  });
}
```

---

## 7.2 它实际上做了两件事

### 第一件：创建协程并交给调度器

```cpp
std::make_shared<CRoutine>(func)
DispatchTask(cr)
```

### 第二件：把 `DataVisitor` 的“有数据”通知绑定到这个协程

```cpp
visitor->RegisterNotifyCallback([this, task_id]() {
  this->NotifyProcessor(task_id);
});
```

所以某个 Reader 的数据一旦到达，不会直接执行用户回调，而是：

> **先通知 Scheduler 去唤醒这个 Reader 对应的 CRoutine。**

---

## 7.3 `NotifyTask()`

在：

- `cyber/scheduler/scheduler.cc:72-77`

```cpp
bool Scheduler::NotifyTask(uint64_t crid) {
  if (stop_.load()) {
    return true;
  }
  return NotifyProcessor(crid);
}
```

即：

- 某个 task 收到事件
- Scheduler 根据 task id 通知对应 processor

---

## 8. `Processor` 如何执行协程

`Processor` 是真正跑协程的线程。

---

## 8.1 主循环

在：

- `cyber/scheduler/processor.cc:40-62`

```cpp
while (running_.load()) {
  if (context_ != nullptr) {
    auto croutine = context_->NextRoutine();
    if (croutine) {
      ...
      croutine->Resume();
      croutine->Release();
    } else {
      context_->Wait();
    }
  } else {
    cv_ctx_.wait_for(...);
  }
}
```

这段代码非常关键。

Processor 线程的逻辑就是：

1. 从 `ProcessorContext` 取一个可运行协程
2. 如果取到了，就 `Resume()` 执行
3. 如果没有，就等待

---

## 8.2 这说明什么

说明 `Processor` 本身不负责调度策略，它只是：

> **不断地问 context：下一个要执行哪个协程？**

真正决定“该跑谁”的，是：

- `ProcessorContext`
- `SchedulerClassic`
- `SchedulerChoreography`

---

## 9. 调度策略层：Classic / Choreography

从目录结构可以看到：

- `cyber/scheduler/policy/scheduler_classic.h`
- `cyber/scheduler/policy/scheduler_choreography.h`

这说明 CyberRT 的调度器不是单一实现，而是把“调度策略”做成了可替换层。

基类抽象接口在：

- `cyber/scheduler/scheduler.h:76-77`

```cpp
virtual bool DispatchTask(const std::shared_ptr<CRoutine>&) = 0;
virtual bool NotifyProcessor(uint64_t crid) = 0;
```

也就是说，不同策略可以决定：

1. 新任务如何分配到 Processor
2. 收到通知后如何唤醒正确的 Processor

---

## 10. Reader 协程调度链路总图

现在把所有角色串起来：

### 10.1 初始化阶段

```text
Reader::Init()
  -> 创建 DataVisitor
  -> 用 DataVisitor 构造 RoutineFactory
  -> Scheduler::CreateTask(factory, name)
  -> 生成 CRoutine
  -> DispatchTask(cr)
  -> 给 DataVisitor 注册 notifier callback
```

### 10.2 消息到达阶段

```text
Receiver 收到消息
  -> DataDispatcher::Dispatch(channel_id, msg)
  -> 写入对应 ChannelBuffer
  -> DataNotifier 触发 DataVisitor.notifier_->callback
  -> Scheduler::NotifyProcessor(task_id)
```

### 10.3 执行阶段

```text
Processor 线程
  -> context_->NextRoutine()
  -> 取到 Reader 对应的 CRoutine
  -> croutine->Resume()
  -> 协程中 dv->TryFetch(msg)
  -> 执行 Reader 的 func(msg)
  -> func(msg) 内部 Enqueue + 用户 reader_func_(msg)
  -> 协程 Yield(...)
```

---

## 11. 这套设计的核心优势

---

## 11.1 解耦“消息到达”和“业务执行”

transport 层只负责：

- 接收消息
- 投递进缓存
- 发出通知

业务逻辑则由调度器统一安排执行。

---

## 11.2 支持多输入同步消费

`CreateRoutineFactory` 和 `DataVisitor` 都支持多模板版本：

- `DataVisitor<M0>`
- `DataVisitor<M0, M1>`
- `DataVisitor<M0, M1, M2>`
- `DataVisitor<M0, M1, M2, M3>`

这使得多输入组件可以在调度层自然完成数据融合后再执行。

---

## 11.3 用户态协程切换比线程切换更轻

因为用的是 `SwapContext()` 做用户态上下文切换，所以成本通常比内核态线程调度低。

---

## 11.4 调度策略可插拔

通过 `SchedulerClassic` / `SchedulerChoreography`，系统可以支持不同的调度模式，而不影响上层 Reader / Component 逻辑。

---

## 11.5 便于 CPU 绑定和资源控制

Scheduler 支持：

- process 级 cpuset
- inner thread 绑核
- 调度策略 / 优先级设置

相关代码在：

- `cyber/scheduler/scheduler.cc:79-100`

---

## 12. 一句话总结

> CyberRT 的协程调度器，本质上是一个 **“DataVisitor 驱动的事件唤醒 + CRoutine 用户态协程 + Processor 工作线程执行 + Scheduler 策略分发”** 的系统。Reader / Component 的业务回调并不是在消息到达时直接执行，而是被包装成一个长期驻留、可挂起/恢复的协程，由调度器在有数据时唤醒执行。

---

## 13. 建议后续阅读顺序

如果你后面想继续深挖，建议按这个顺序看：

1. `cyber/croutine/croutine.h`
2. `cyber/croutine/croutine.cc`
3. `cyber/croutine/detail/routine_context.h`
4. `cyber/croutine/routine_factory.h`
5. `cyber/data/data_visitor_base.h`
6. `cyber/data/data_visitor.h`
7. `cyber/scheduler/scheduler.h`
8. `cyber/scheduler/scheduler.cc`
9. `cyber/scheduler/processor.h`
10. `cyber/scheduler/processor.cc`
11. 再继续看 `scheduler_classic.cc / scheduler_choreography.cc`

这样会比较顺。