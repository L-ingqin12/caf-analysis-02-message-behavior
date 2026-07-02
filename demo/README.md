# CAF Message & Behavior Demo

## 概述

本 Demo 全面演示 CAF 的消息传递和行为模式匹配子系统，包括：

1. **多种消息类型** — Atom、int32_t、std::string、自定义结构体的收发
2. **Behavior 模式匹配** — `or_else()` 组合多个 `message_handler`
3. **自定义类型序列化** — `inspect()` + `binary_serializer` / `binary_deserializer` 往返
4. **超时处理** — `after()` 行为级超时 + `set_idle_handler()` 空闲超时
5. **动态行为切换** — `become()` / `unbecome()` 状态机

## 架构说明

```
caf_main
  ├── Custom Type Serialization
  │     └── task_msg → binary_serializer → binary_deserializer → task_msg
  ├── worker (actor_from_state)
  │     ├── idle: 接收 submit_atom → become(processing)
  │     ├── processing: 1.5s 后完成任务 → unbecome() → idle
  │     └── set_idle_handler(3s, repeat): 空闲时定期打印
  ├── monitor (or_else 组合)
  │     ├── task_handler: submit_atom / result_msg
  │     └── fallback: string / int32 / hello_atom / catch-all
  ├── timer (after() timeout)
  │     └── 3s 无匹配消息 → 超时触发并 quit
  └── scoped_actor
        ├── 发送多种类型消息
        ├── request/response 查询状态
        └── 发送 exit 消息优雅关闭
```

## 演示消息类型

| 类型 | 说明 | 匹配行为 |
|------|------|----------|
| `hello_atom` | 问候标记 | fallback → 打印 |
| `greet_atom + string` | 带名字的问候 | idle_handler → 打印 |
| `submit_atom + task_msg` | 提交任务 | idle_handler → become(processing) |
| `query_atom` | 状态查询 | 返回状态字符串 |
| `cancel_atom + int32` | 取消任务 | processing → 取消并回到 idle |
| `std::string` | 字符串消息 | fallback → 打印 |
| `int32` | 整数消息 | fallback → 打印 |
| `result_msg` | 结果消息 | task_handler → 打印 |

## 编译与运行

### 前置条件

- CMake >= 3.20
- C++17 编译器
- Git（用于 FetchContent 拉取 CAF）

### 编译步骤

```bash
cd /root/caf-analysis-output/02-message-behavior/demo
mkdir -p build && cd build
cmake ..
cmake --build .
```

### 运行

```bash
./message-behavior-demo
```

### 预期输出

程序将输出六个部分的运行结果：

1. **Custom Type Serialization** — task_msg 的序列化往返测试
2. **Spawn Actors** — 创建三个 actor 的日志
3. **Multiple Message Types** — 不同消息类型发送和匹配的详细输出
4. **Behavior Timeout** — timer actor 的超时触发
5. **状态切换** — worker 在 idle / processing 之间的切换

## 关键代码

| 功能 | 文件行号 |
|------|----------|
| 自定义类型定义 | `main.cpp:65-108` |
| worker 状态机 | `main.cpp:124-219` |
| or_else 组合 | `main.cpp:232-264` |
| after() 超时 | `main.cpp:274-285` |
| 序列化往返 | `main.cpp:297-322` |
| request/response | `main.cpp:361-385` |
