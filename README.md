# libaio-windows

**Linux-native asynchronous I/O access library（libaio 0.3.113）的 Windows 移植版。**
API 与上游 libaio 完全同名同义（`io_setup / io_destroy / io_submit / io_getevents / io_cancel`），
内部后端不是“让 MSVC 编译通过”，而是**完整重写为 Windows OVERLAPPED I/O + IOCP**。

- 上游基线：<https://pagure.io/libaio.git>（`libaio.spec` Version 0.3.113，
  HEAD `b8eadc9f89e8f7ab0338eacda9f98a6caea76883`，2022-06-02）
- 与 GitHub 镜像 <https://github.com/yugabyte/libaio> 的关系：**两者当前完全相同**
  （同一 HEAD 提交、194 条提交历史逐一相同、文件树 diff 为空），yugabyte 仓库是
  pagure 官方上游的镜像，任选其一作为上游均不影响本项目内容。
- 许可证：LGPL-2.1-or-later（与上游一致，见 `COPYING`）。

---

## 1. 这是什么、不是什么

```
能做（已实现，测试通过）：
  DeepSpeed async_io / DeepNVMe AIO 的全部依赖面
  io_submit/io_getevents 异步 I/O → Windows OVERLAPPED + IOCP
  NVMe/SSD ──async Read/Write──> pinned CPU buffer ──cudaMemcpyAsync──> GPU

不做（超出范围，需要 NVIDIA 闭源组件）：
  GPUDirect Storage（GDS）：需要 libcufile（闭源、Linux-only），Windows 无此组件
  NVMe ──DMA──> GPU VRAM 直通路径
```

即：Windows 上补齐 DeepSpeed 的 **AIO 路径**；GDS 依旧无解（这是 NVIDIA 的闭源
边界，不是移植工作量的问题）。

## 2. 语义映射

| Linux libaio（上游）        | Windows 本移植                                            |
| --------------------------- | --------------------------------------------------------- |
| `io_setup(2)`               | `CreateIoCompletionPort()`（每个 io_context 一个 IOCP）   |
| `io_submit(2)`（PREAD/PWRITE） | `ReadFile()/WriteFile()` + `OVERLAPPED`（见 §3 三级路径） |
| `io_submit`（PREADV/PWRITEV） | 单 iovec 走普通路径；多 iovec 由 worker 线程聚合后执行   |
| `io_submit`（FSYNC/FDSYNC） | worker 线程执行 `FlushFileBuffers()`                      |
| `io_getevents(2)`           | `GetQueuedCompletionStatusEx()`（min_nr/timeout 语义对齐） |
| `io_cancel(2)`              | `CancelIoEx()`；成功后事件以 `res == -ECANCELED` 浮出      |
| `io_destroy(2)`             | 关闭 IOCP；仍有在途请求时返回 `-EBUSY`（同 Linux 行为）   |
| `open(O_DIRECT)`            | **`aio_open(path, O_DIRECT …)`** → `FILE_FLAG_NO_BUFFERING \| FILE_FLAG_OVERLAPPED` |
| `close(fd)`                 | `aio_close(fd)`                                           |
| `pread 读到 EOF`            | `res == 0`（`ERROR_HANDLE_EOF`/`STATUS_END_OF_FILE` 归一） |
| 请求取消/错误               | `res` 为负 errno（`-ECANCELED/-EINVAL/-EIO/-EBADF…`），与 Linux 相同 |

每个 `io_context_t` 内：一次提交恰好产生**一个**完成包（内核包，或库自己
`PostQueuedCompletionStatus` 的包——同步报错/NOOP/worker 结果），因此
`io_getevents` 的收割计数与 Linux 严格一致。

## 3. io_submit 的三级路径（自动选择，无需配置）

1. **native（最快）**：fd 由 `aio_open()` 打开（创建时即带 `FILE_FLAG_OVERLAPPED`）
   且已绑定本 context 的 IOCP → 直接 `ReadFile/WriteFile`，每请求零额外系统调用。
2. **reopen**：fd 来自 CRT `open()/_open()` 等外部代码 → 每请求
   `ReOpenFile()` 出一个 `FILE_FLAG_OVERLAPPED` 句柄完成 I/O，完成后关闭。
   未改动的存量代码（先用 `open()` 拿 fd）无需改动即可工作。
3. **worker**：fsync/fdsync、多 iovec、以及前两级失败的罕见场景 → 由
   context 的 worker 线程阻塞执行，再投递完成包。语义不变，仅串行化。

fd 表为进程级；`aio_open()` 记录句柄属性（overlapped / NO_BUFFERING / 扇区大小），
供对齐校验与路径选择使用。

## 4. O_DIRECT 对齐要求（与 Linux 相同）

`O_DIRECT`（→ `FILE_FLAG_NO_BUFFERING`）要求**缓冲区地址、传输长度、文件偏移**
均为该卷逻辑扇区大小（512 或 4Kn 盘为 4096）的整数倍，否则 `io_submit` 直接返回
`-EINVAL`（与内核 aio 行为一致）。缓冲区请用 `_aligned_malloc(size, 4096)`。

## 5. DeepSpeed 接入

DeepSpeed `async_io` op（`csrc/aio/deepspeed_aio.cpp`）在 Windows 上的适配：

```cpp
#include <libaio.h>                 // D:\libaio-install\include
#pragma comment(lib, "aio.lib")     // 或链接 D:\libaio-install\lib\aio.lib；aio.dll 放 PATH

// 唯一必须改的一行：
- int fd = open(path.c_str(), O_DIRECT | O_RDWR | O_CREAT, 0644);
+ int fd = aio_open(path.c_str(), O_DIRECT | O_RDWR | O_CREAT, 0644);

// 其余原样：io_prep_pwrite/io_prep_pread/io_submit/io_getevents/io_destroy
// 收割循环保持 DeepSpeed 原写法（io_getevents(ctx, 1, MAX_EVENTS, ...) 循环收割）
```

注意：`aio_open`/`aio_close` 是本移植的扩展 API（Windows 下 O_DIRECT/overlapped
语义在 `CreateFile` 时决定，无法从 fd 事后补上）。若 fd 来自第三方库的
`open()`，不改也能跑（自动走 ReOpenFile 三级路径），但建议统一改用 `aio_open`
以获得最优路径。

头文件布局与上游一致，`PADDEDul` 在 Windows 上取 `unsigned long long` 以保持
与 Linux x86_64 相同的结构布局（`struct iocb` 64 字节、`struct io_event` 32 字节），
DeepSpeed 对 `iocb` 的用法无需改动。MSVC 下默认 `#pragma comment(lib, "aio.lib")`
自动链接（可用 `LIBAIO_NO_AUTOLINK` 关闭；静态链接请定义 `LIBAIO_STATIC`）。

**明确不支持**（与 Linux 内核行为对齐地报错）：`IO_CMD_POLL`、eventfd
（`io_set_eventfd`）、`RWF_*` flags、单次传输 > 4 GB（拆分即可）。

## 6. 构建与测试

```
:: 需要 VS2022 BuildTools（MSVC 14.44 + 自带 CMake/Ninja）
cd /d D:\src\libaio-windows
build.cmd          :: configure + build + 运行 test_basic、test_deepspeed

:: 安装
cmake --install build --prefix D:\libaio-install
```

产物：`aio.dll` + `aio.lib`（导入库）+ `aio_static.lib` + `libaio.h`。

测试覆盖（`tests/test_basic.c`）：

- native O_DIRECT 四路并发 write/read + 数据校验（真 NVMe/SSD 异步完成）
- 短读/EOF 语义（`res == 剩余字节` / `res == 0`），无重复完成包
- `IO_CMD_FSYNC`；O_DIRECT 对齐校验（`-EINVAL`）
- 坏 fd（`-EBADF`）、坏 opcode（`-EINVAL`）、cancel 未提交请求（`-ENOENT`）、
  超时返回 0
- CRT `open()` fd 的 ReOpenFile 路径；vectored 读写（聚合路径）
- 参数校验矩阵；`io_destroy` 语义

`tests/test_deepspeed.c` 复刻 DeepSpeed `op_builder/async_io.py` 的探针程序
（io_setup → O_DIRECT pwrite → io_getevents → pread 校验 → io_destroy），
是 DeepSpeed AIO 后端能否构建的判定标准。全套测试连续 5 轮运行稳定。

## 7. 文件结构

```
include/libaio.h        公共头（上游 0.3.113 头的 Windows 移植，API 同名）
src/aio_internal.h      内部结构：context/request/fd 表
src/aio_backend.c       fd 表、aio_open/aio_close、errno 映射、worker 线程
src/io_setup.c          io_setup  → CreateIoCompletionPort
src/io_submit.c         io_submit → OVERLAPPED ReadFile/WriteFile（三级路径）
src/io_getevents.c      io_getevents → GetQueuedCompletionStatusEx
src/io_cancel.c         io_cancel → CancelIoEx
src/io_destroy.c        io_destroy（-EBUSY 语义）
src/io_pgetevents.c     io_pgetevents（sigmask 忽略）
src/io_queue_*.c        上游的兼容别名
tests/                  test_basic.c / test_deepspeed.c
build.cmd               一键构建+测试脚本（MSVC x64 + Ninja）
COPYING                 LGPL-2.1（来自上游）
```

## 8. 已知限制

- 单次传输上限 4 GB（`ReadFile/WriteFile` 的 DWORD 限制），超限 `-EINVAL`。
- eventfd、`IO_CMD_POLL`、`RWF_NOWAIT` 等 Linux 专属特性不支持。
- 多 iovec 走聚合路径（正确性优先；DeepSpeed 不用 vectored I/O）。
- worker 仿真路径（仅在前两级失败时触发）会移动文件指针并串行执行。
- GDS/libcufile 属 NVIDIA 闭源组件，Windows 无对应物，本项目不含也不计划。
