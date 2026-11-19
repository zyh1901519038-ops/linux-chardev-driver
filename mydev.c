// SPDX-License-Identifier: GPL-2.0
/*
 * mydev.c —— 一个完整的字符设备驱动示例（虚拟设备，不需要真实硬件）
 *
 * 实现内容：
 *   1. 设备号申请 / cdev 注册 / class_create 自动生成 /dev/mydev（含内核版本兼容）
 *   2. file_operations: open / release / read / write / poll / unlocked_ioctl
 *   3. 内核环形缓冲区 + mutex 保护（多进程并发安全）
 *   4. 阻塞读（wait_queue）+ 非阻塞（O_NONBLOCK）分支 + poll 唤醒
 *   5. copy_to_user / copy_from_user 的错误路径（-EFAULT），失败时不消费数据
 *   6. 模块参数 buf_size 可配
 *
 * 编译：make
 * 加载：sudo insmod mydev.ko [buf_size=2048]
 * 卸载：sudo rmmod mydev
 * 验证：bash run_tests.sh
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/string.h>      /* memcpy（显式包含，不靠其他头文件间接引入） */
#include <linux/version.h>     /* LINUX_VERSION_CODE / KERNEL_VERSION */

#define DEV_NAME  "mydev"
#define DEF_BUFSZ 1024
#define MAX_BUFSZ (1u << 20)

/* 模块参数：insmod mydev.ko buf_size=4096 */
static unsigned int buf_size = DEF_BUFSZ;
module_param(buf_size, uint, 0644);
MODULE_PARM_DESC(buf_size, "环形缓冲区大小（字节），默认 1024");

/* ioctl 命令定义 */
#define MYDEV_IOC_MAGIC   'k'
#define MYDEV_IOC_GETSIZE _IOR(MYDEV_IOC_MAGIC, 1, int)   /* 读缓冲区大小 */
#define MYDEV_IOC_GETUSED _IOR(MYDEV_IOC_MAGIC, 2, int)   /* 读当前已用字节数 */
#define MYDEV_IOC_RESET   _IO(MYDEV_IOC_MAGIC, 3)         /* 清空缓冲区 */

struct mydev {
	struct cdev        cdev;
	struct mutex       lock;      /* 保护环形缓冲区的读写指针 */
	wait_queue_head_t  rq;        /* 可读等待队列 */
	wait_queue_head_t  wq;        /* 可写等待队列 */
	char              *buf;
	size_t             size;      /* 缓冲区容量 */
	size_t             head;      /* 写位置 */
	size_t             tail;      /* 读位置 */
	size_t             used;      /* 已用字节数 */
	int                open_cnt;
};

static dev_t          g_devno;
static struct class  *g_class;
static struct device *g_device;
static struct mydev  *g_dev;

/* ------------------------- 环形缓冲区 ------------------------- */

static inline size_t ring_space(const struct mydev *d)
{
	return d->size - d->used;
}

/* 往环形缓冲区写入，调用者必须持有 lock；返回实际写入字节数 */
static size_t ring_put(struct mydev *d, const char *src, size_t len)
{
	size_t n = min(len, ring_space(d));
	size_t first = min(n, d->size - d->head);

	memcpy(d->buf + d->head, src, first);
	if (n > first)
		memcpy(d->buf, src + first, n - first);

	d->head = (d->head + n) % d->size;
	d->used += n;
	return n;
}

/* 只读不消费：拷到 dst 但不移动 tail，便于「先拷给用户态，成功了再消费」 */
static size_t ring_peek(const struct mydev *d, char *dst, size_t len)
{
	size_t n = min(len, d->used);
	size_t first = min(n, d->size - d->tail);

	memcpy(dst, d->buf + d->tail, first);
	if (n > first)
		memcpy(dst, d->buf + first, n - first);
	return n;
}

/* 消费 n 字节，调用者必须持有 lock */
static void ring_commit(struct mydev *d, size_t n)
{
	n = min(n, d->used);
	d->tail = (d->tail + n) % d->size;
	d->used -= n;
}

static void ring_reset(struct mydev *d)
{
	d->head = d->tail = d->used = 0;
}

/* ------------------------- file_operations ------------------------- */

static int mydev_open(struct inode *inode, struct file *filp)
{
	struct mydev *d = container_of(inode->i_cdev, struct mydev, cdev);

	filp->private_data = d;
	mutex_lock(&d->lock);
	d->open_cnt++;
	mutex_unlock(&d->lock);

	pr_info("mydev: opened (open_cnt=%d, pid=%d)\n", d->open_cnt, current->pid);
	return 0;
}

static int mydev_release(struct inode *inode, struct file *filp)
{
	struct mydev *d = filp->private_data;

	if (!d)
		return 0;

	mutex_lock(&d->lock);
	d->open_cnt--;
	mutex_unlock(&d->lock);

	pr_info("mydev: released (open_cnt=%d)\n", d->open_cnt);
	return 0;
}

static ssize_t mydev_read(struct file *filp, char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct mydev *d = filp->private_data;
	const bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;
	char *kbuf;
	size_t n;
	int ret;

	if (count == 0)
		return 0;

	kbuf = kmalloc(min(count, d->size), GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/*
	 * 等待循环：醒来之后必须重新检查条件。
	 * 原因有两个：① 数据可能被其他读进程抢先取走；② 可能是被信号唤醒。
	 * 等待条件里用 READ_ONCE 读 used（不取锁），真正的判断在锁内完成，
	 * 这样 wait_event 反复求值时不会反复进出 mutex。
	 */
	for (;;) {
		mutex_lock(&d->lock);
		if (d->used > 0) {
			/* 持锁完成 peek + copy + commit：避免与并发读交错丢数据。
			 * 代价是拷贝期间写端会等锁，生产驱动通常用 per-file 缓冲规避。
			 */
			n = ring_peek(d, kbuf, min(count, d->size));
			if (copy_to_user(ubuf, kbuf, n)) {
				mutex_unlock(&d->lock);
				ret = -EFAULT;      /* 不消费数据，用户态可以重试 */
				goto out;
			}
			ring_commit(d, n);
			mutex_unlock(&d->lock);
			wake_up_interruptible(&d->wq);   /* 腾出空间，叫醒写端 */
			ret = (ssize_t)n;
			goto out;
		}
		mutex_unlock(&d->lock);

		if (nonblock) {
			ret = -EAGAIN;              /* 非阻塞：立刻告诉用户态没数据 */
			goto out;
		}

		if (wait_event_interruptible(d->rq, READ_ONCE(d->used) > 0)) {
			ret = -ERESTARTSYS;         /* 被信号打断 */
			goto out;
		}
	}

out:
	kfree(kbuf);
	return ret;
}

static ssize_t mydev_write(struct file *filp, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct mydev *d = filp->private_data;
	const bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;
	char *kbuf;
	size_t n;
	int ret;

	if (count == 0)
		return 0;

	n = min(count, d->size);        /* 一次最多写一整圈 */
	kbuf = kmalloc(n, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/* 先从用户态拷出来（此时不持锁），再进临界区写入 */
	if (copy_from_user(kbuf, ubuf, n)) {
		ret = -EFAULT;
		goto out;
	}

	for (;;) {
		mutex_lock(&d->lock);
		if (ring_space(d) > 0) {
			n = ring_put(d, kbuf, n);
			mutex_unlock(&d->lock);
			wake_up_interruptible(&d->rq);   /* 有数据了，叫醒读端 */
			ret = (ssize_t)n;
			goto out;
		}
		mutex_unlock(&d->lock);

		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(d->wq, READ_ONCE(d->size) - READ_ONCE(d->used) > 0)) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

out:
	kfree(kbuf);
	return ret;
}

/*
 * poll：select / poll / epoll 最终都会走到这里判断设备是否就绪。
 * 读队列和写队列都要 poll_wait，否则某一端永远等不到唤醒。
 */
static __poll_t mydev_poll(struct file *filp, poll_table *wait)
{
	struct mydev *d = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &d->rq, wait);
	poll_wait(filp, &d->wq, wait);

	mutex_lock(&d->lock);
	if (d->used > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ring_space(d) > 0)
		mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&d->lock);

	return mask;
}

static long mydev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mydev *d = filp->private_data;
	int val;

	switch (cmd) {
	case MYDEV_IOC_GETSIZE:
		val = (int)d->size;
		if (put_user(val, (int __user *)arg))
			return -EFAULT;
		return 0;

	case MYDEV_IOC_GETUSED:
		mutex_lock(&d->lock);
		val = (int)d->used;
		mutex_unlock(&d->lock);
		if (put_user(val, (int __user *)arg))
			return -EFAULT;
		return 0;

	case MYDEV_IOC_RESET:
		mutex_lock(&d->lock);
		ring_reset(d);
		mutex_unlock(&d->lock);
		wake_up_interruptible(&d->wq);   /* 清空后空间变大，叫醒写端 */
		pr_info("mydev: buffer reset by pid %d\n", current->pid);
		return 0;

	default:
		return -ENOTTY;                  /* 不支持的命令，标准返回值 */
	}
}

static const struct file_operations mydev_fops = {
	.owner          = THIS_MODULE,
	.open           = mydev_open,
	.release        = mydev_release,
	.read           = mydev_read,
	.write          = mydev_write,
	.poll           = mydev_poll,
	.unlocked_ioctl = mydev_ioctl,
#ifdef CONFIG_COMPAT
	/* 32 位用户态跑在 64 位内核上时才需要，该字段仅在 CONFIG_COMPAT 下存在 */
	.compat_ioctl   = mydev_ioctl,
#endif
};

/* ------------------------- 模块装卸 ------------------------- */

static int __init mydev_init(void)
{
	int ret;

	if (buf_size == 0 || buf_size > MAX_BUFSZ) {
		pr_err("mydev: buf_size 不合法（1..%u）\n", MAX_BUFSZ);
		return -EINVAL;
	}

	g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
	if (!g_dev)
		return -ENOMEM;

	g_dev->size = buf_size;
	g_dev->buf = kmalloc(g_dev->size, GFP_KERNEL);
	if (!g_dev->buf) {
		ret = -ENOMEM;
		goto err_free_dev;
	}

	mutex_init(&g_dev->lock);
	init_waitqueue_head(&g_dev->rq);
	init_waitqueue_head(&g_dev->wq);

	/* 1) 申请设备号（动态分配主设备号） */
	ret = alloc_chrdev_region(&g_devno, 0, 1, DEV_NAME);
	if (ret) {
		pr_err("mydev: alloc_chrdev_region 失败 ret=%d\n", ret);
		goto err_free_buf;
	}

	/* 2) 注册 cdev，绑定 file_operations */
	cdev_init(&g_dev->cdev, &mydev_fops);
	g_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&g_dev->cdev, g_devno, 1);
	if (ret) {
		pr_err("mydev: cdev_add 失败 ret=%d\n", ret);
		goto err_unregister;
	}

	/*
	 * 3) 创建类和设备节点：udev 会自动 mknod 出 /dev/mydev
	 * class_create 的签名在内核 6.4 变了：之前是 (owner, name)，之后是 (name)。
	 * 用版本判断兼容两种内核。
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	g_class = class_create(DEV_NAME);
#else
	g_class = class_create(THIS_MODULE, DEV_NAME);
#endif
	if (IS_ERR(g_class)) {
		ret = PTR_ERR(g_class);
		g_class = NULL;
		pr_err("mydev: class_create 失败 ret=%d\n", ret);
		goto err_cdev_del;
	}

	g_device = device_create(g_class, NULL, g_devno, NULL, DEV_NAME);
	if (IS_ERR(g_device)) {
		ret = PTR_ERR(g_device);
		g_device = NULL;
		pr_err("mydev: device_create 失败 ret=%d\n", ret);
		goto err_class_destroy;
	}

	pr_info("mydev: loaded (major=%d minor=%d, buf_size=%u, kernel=%d.%d)\n",
		MAJOR(g_devno), MINOR(g_devno), buf_size,
		(LINUX_VERSION_CODE >> 16) & 0xff, (LINUX_VERSION_CODE >> 8) & 0xff);
	return 0;

err_class_destroy:
	class_destroy(g_class);
err_cdev_del:
	cdev_del(&g_dev->cdev);
err_unregister:
	unregister_chrdev_region(g_devno, 1);
err_free_buf:
	kfree(g_dev->buf);
err_free_dev:
	kfree(g_dev);
	g_dev = NULL;
	return ret;
}

static void __exit mydev_exit(void)
{
	if (g_device)
		device_destroy(g_class, g_devno);
	if (g_class)
		class_destroy(g_class);

	cdev_del(&g_dev->cdev);
	unregister_chrdev_region(g_devno, 1);
	kfree(g_dev->buf);
	kfree(g_dev);
	g_dev = NULL;

	pr_info("mydev: unloaded\n");
}

module_init(mydev_init);
module_exit(mydev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zhou Yihong");
MODULE_DESCRIPTION("A virtual character device with blocking I/O, poll and ioctl");
MODULE_VERSION("1.1");
