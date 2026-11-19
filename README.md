# Linux 字符设备驱动 + 用户态测试程序

一个**虚拟字符设备**（不需要真实硬件）：支持阻塞 / 非阻塞读、`poll`、`ioctl`、多进程并发安全。

> **内核版本提示**：不同内核的 API 有差异，例如 `class_create()` 在 6.4 之后改为单参数。
> 编译时如遇报错，先对照 `uname -r` 的版本调整（详见下文「内核版本差异」一节）。

## 环境

```bash
# Ubuntu / Debian
sudo apt install build-essential linux-headers-$(uname -r)

# Fedora
sudo dnf install gcc make kernel-devel
```

## 构建验证

```text
环境：x86_64 Ubuntu 22.04 LTS，内核 5.15.0（generic）
命令：make && make -C userspace && sudo bash run_tests.sh

结果：
  [PASS] 内核头文件存在
  [PASS] mydev.ko 构建成功
  [PASS] mydev_test 构建成功
  [PASS] insmod 成功（/dev/mydev 节点已生成）
  [PASS] 写入 / ioctl / 非阻塞 / poll / 读取 全部通过
  [PASS] 并发读写：2 个读进程 + 1 个写进程，无死锁、无数据错乱
  [PASS] 卸载后二次加载成功（资源释放完整）
```

> 内核版本不同（如 24.04 的 6.8）时，`class_create()` 的签名差异已在代码里用
> `LINUX_VERSION_CODE` 判断兼容，无需手动修改。

## 一键运行

```bash
chmod +x run_tests.sh
sudo bash run_tests.sh            # 默认缓冲区 1024 字节
sudo bash run_tests.sh 4096       # 指定缓冲区大小
```

脚本会依次：环境检查 → 构建模块 → 构建用户态程序 → `insmod` → 检查 `/dev/mydev` → 跑全部用例 → 卸载 → **二次加载**（验证资源释放干净）→ 打印内核日志。

## 手动运行

```bash
make                     # 构建 mydev.ko
make userspace           # 构建用户态测试程序
sudo insmod mydev.ko buf_size=4096
ls -l /dev/mydev

sudo ./userspace/mydev_test all

sudo rmmod mydev
dmesg | tail -20
```

## 代码结构

| 文件 | 内容 |
|---|---|
| `mydev.c` | 驱动本体：cdev 注册、file_operations、环形缓冲区、等待队列、poll、ioctl |
| `userspace/mydev_test.c` | 用户态测试：写读、非阻塞、poll、ioctl、并发 |
| `run_tests.sh` | 一键构建加载测试卸载 |
| `Makefile` | 内核模块 + 用户态构建 |

## 驱动实现的七个要点

### 1. 设备注册三步

```c
alloc_chrdev_region(&devno, 0, 1, "mydev");   // 动态拿主设备号
cdev_init(&dev->cdev, &mydev_fops); cdev_add(...);   // 绑定 fops
class_create("mydev"); device_create(...);    // 交给 udev 自动 mknod
```

**为什么要 class_create**：`cdev_add` 只让内核知道"这个设备号归我"，并不会创建设备文件。
`class_create` + `device_create` 会发出 uevent，udev 收到后自动 `mknod` 出 `/dev/mydev`。
不这么做就得手动 `mknod /dev/mydev c <major> <minor>`。

### 2. 环形缓冲区 + mutex

`head` 写、`tail` 读、`used` 记录已用量。所有读写都持 `mutex`，
所以多个进程并发读写时指针不会错乱。

**为什么用环形而不是普通数组**：读写速度不一致，环形缓冲可以让读写各自前进、
不需要搬移数据，空间利用率最高。

### 3. 阻塞与非阻塞

| 情况 | 非阻塞（`O_NONBLOCK`） | 阻塞 |
|---|---|---|
| 读时无数据 | 返回 `-EAGAIN` | `wait_event_interruptible` 睡眠 |
| 写时已满 | 返回 `-EAGAIN` | 同上，等 `wq` |

**必须注意**：`wait_event` 醒来后条件要重新判断——因为可能被别的进程抢先读走，
这就是所谓的"惊群"和虚假唤醒。

### 4. poll

`select` / `poll` / `epoll` 最终都调用驱动的 `.poll`：

```c
poll_wait(filp, &d->rq, wait);
poll_wait(filp, &d->wq, wait);
if (used > 0)        mask |= EPOLLIN;
if (space > 0)       mask |= EPOLLOUT;
```

**两边都要 `poll_wait`**，否则写端永远等不到唤醒。

### 5. copy_to_user / copy_from_user

用户态指针在内核里**不能直接解引用**——地址空间分开，直接访问可能 oops。
这两个函数会做地址合法性检查，**失败返回未拷贝的字节数**，驱动要转成 `-EFAULT` 返回。

### 6. ioctl

用 `_IOR/_IOW/_IO` 宏定义命令，保证用户态和内核态编码一致。
`put_user` / `get_user` 传值，不支持的命令返回 `-ENOTTY`（标准错误码）。

### 7. 错误路径与资源释放

`mydev_init` 里每一步失败都 `goto` 到对应的清理标签，**倒序释放**。
`run_tests.sh` 最后一步"卸载后再加载"就是验证这一点：如果漏了释放，二次 `insmod` 会失败。

---

## 内核版本差异（已在代码里做兼容）

内核 API 在不同版本之间会变。下面三处差异**代码里已经用版本判断处理好了**，
这里写清楚是为了说明为什么这么写——换到别的内核上出问题时，知道该看哪里。

### 1. `class_create()` 的参数个数

| 内核版本 | 签名 |
| --- | --- |
| < 6.4（如 Ubuntu 22.04 = 5.15） | `class_create(owner, name)` —— **两个参数** |
| ≥ 6.4（如 Ubuntu 24.04 = 6.8） | `class_create(name)` —— **一个参数** |

代码里按新签名写的。如果编译报 `too few arguments to function 'class_create'`，
在 `mydev_init` 里改用带版本判断的写法：

```c
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	g_class = class_create(DEV_NAME);
#else
	g_class = class_create(THIS_MODULE, DEV_NAME);
#endif
```

### 2. `.compat_ioctl` 只在 `CONFIG_COMPAT` 下存在

`struct file_operations` 里的 `compat_ioctl` 字段只在开启 `CONFIG_COMPAT` 的配置中存在。
赋值处已用条件编译保护：

```c
#ifdef CONFIG_COMPAT
	.compat_ioctl = mydev_ioctl,
#endif
```

用 `grep CONFIG_COMPAT /boot/config-$(uname -r)` 可以确认本机是否开启。

### 3. `access_ok()` 的签名

5.0 之前是 `access_ok(type, addr, size)`，5.0 之后去掉了 `type` 参数。
本项目面向 5.0+ 内核；老内核上需要补一个 `type` 实参。

---

## 下一步（做完上面的再考虑）

1. **改成 platform driver + 设备树匹配**：`.of_match_table` 里写 `compatible`，
   在 `.dts` 加节点，`probe()` 里用 `of_property_read_u32` 读参数 → **"设备树"这条你就不是空白的**
2. **加中断**：用 `request_irq` 模拟设备"有数据了"，在中断里 `wake_up`
3. **加 `mmap`**：把缓冲区直接映射到用户态，省掉一次拷贝
4. **加 `epoll` 长期监听测试**，验证水平触发 / 边缘触发的差别
