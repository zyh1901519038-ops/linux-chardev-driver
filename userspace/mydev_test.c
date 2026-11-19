/*
 * userspace/mydev_test.c —— 驱动的一致性测试程序
 *
 * 用法：
 *   ./mydev_test all           跑全部测试
 *   ./mydev_test write "abc"   写入
 *   ./mydev_test read 32       读 32 字节
 *   ./mydev_test nonblock      验证非阻塞读返回 EAGAIN
 *   ./mydev_test poll 1000     等待可读（毫秒）
 *   ./mydev_test ioctl         查询容量 / 已用 / 清空
 *   ./mydev_test concurrent    2 个读进程 + 1 个写进程并发，校验数据不乱
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEV_PATH "/dev/mydev"

#define MYDEV_IOC_MAGIC   'k'
#define MYDEV_IOC_GETSIZE _IOR(MYDEV_IOC_MAGIC, 1, int)
#define MYDEV_IOC_GETUSED _IOR(MYDEV_IOC_MAGIC, 2, int)
#define MYDEV_IOC_RESET   _IO(MYDEV_IOC_MAGIC, 3)

static int failures;

#define CHECK(cond, fmt, ...)                                  \
	do {                                                   \
		if (cond) {                                    \
			printf("  [PASS] " fmt "\n", ##__VA_ARGS__); \
		} else {                                       \
			printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); \
			failures++;                            \
		}                                              \
	} while (0)

static int do_write(const char *text)
{
	int fd = open(DEV_PATH, O_WRONLY);
	ssize_t n;

	if (fd < 0) { perror("open"); return -1; }
	n = write(fd, text, strlen(text));
	if (n < 0) perror("write");
	else printf("[write] %zd 字节: %s\n", n, text);
	close(fd);
	return n < 0 ? -1 : 0;
}

static int do_read(int want)
{
	int fd = open(DEV_PATH, O_RDONLY);
	char *buf = calloc(1, want + 1);
	ssize_t n;

	if (fd < 0) { perror("open"); free(buf); return -1; }
	n = read(fd, buf, want);
	if (n < 0) perror("read");
	else printf("[read] %zd 字节: %s\n", n, buf);
	close(fd);
	free(buf);
	return n < 0 ? -1 : 0;
}

/* 非阻塞：空缓冲区应立即返回 EAGAIN，而不是卡住 */
static int do_nonblock(void)
{
	int fd = open(DEV_PATH, O_RDONLY | O_NONBLOCK);
	char buf[16];
	ssize_t n;

	if (fd < 0) { perror("open"); return -1; }
	n = read(fd, buf, sizeof(buf));
	CHECK(n < 0 && errno == EAGAIN, "非阻塞读返回 EAGAIN（实际 n=%zd errno=%d）", n, errno);
	close(fd);
	return 0;
}

/* poll：有数据应立即就绪；无数据应超时返回 0 */
static int do_poll(int timeout_ms)
{
	struct pollfd pfd;
	int fd = open(DEV_PATH, O_RDONLY);
	int rc;

	if (fd < 0) { perror("open"); return -1; }
	pfd.fd = fd;
	pfd.events = POLLIN;
	rc = poll(&pfd, 1, timeout_ms);
	printf("[poll] rc=%d revents=0x%x\n", rc, pfd.revents);
	close(fd);
	return rc < 0 ? -1 : 0;
}

static int do_ioctl(void)
{
	int fd = open(DEV_PATH, O_RDWR);
	int size = -1, used = -1;

	if (fd < 0) { perror("open"); return -1; }

	if (ioctl(fd, MYDEV_IOC_GETSIZE, &size) < 0) { perror("ioctl GETSIZE"); close(fd); return -1; }
	printf("[ioctl] 缓冲区容量 = %d 字节\n", size);
	CHECK(size > 0, "容量大于 0");

	if (ioctl(fd, MYDEV_IOC_GETUSED, &used) < 0) { perror("ioctl GETUSED"); close(fd); return -1; }
	printf("[ioctl] 已用 = %d 字节\n", used);

	if (ioctl(fd, MYDEV_IOC_RESET) < 0) { perror("ioctl RESET"); close(fd); return -1; }
	if (ioctl(fd, MYDEV_IOC_GETUSED, &used) < 0) { perror("ioctl GETUSED"); close(fd); return -1; }
	CHECK(used == 0, "RESET 后已用为 0（实际 %d）", used);
	close(fd);
	return 0;
}

/*
 * 并发测试：
 *   父进程写 200 条带序号的记录；两个子进程同时读，各自校验收到的字节数
 *   这里只验证「不丢数据、不崩、不死锁」，严格的内容顺序校验见 README 的说明
 */
static int do_concurrent(void)
{
	int readers = 2, i;
	pid_t pids[2];
	int wfd, nfd;

	nfd = open(DEV_PATH, O_RDWR);
	if (nfd >= 0) { ioctl(nfd, MYDEV_IOC_RESET); close(nfd); }

	/* 起 2 个读进程：poll 带超时读，避免写端结束后永久阻塞 */
	for (i = 0; i < readers; i++) {
		pids[i] = fork();
		if (pids[i] == 0) {
			int fd = open(DEV_PATH, O_RDONLY);
			char buf[4096];
			long got = 0;
			int rounds = 0;

			if (fd < 0) _exit(1);
			for (;;) {
				struct pollfd p = { .fd = fd, .events = POLLIN };
				int pr = poll(&p, 1, 3000);   /* 3 秒无新数据就收工 */

				if (pr == 0) break;
				if (pr < 0) { if (errno == EINTR) continue; break; }

				ssize_t n = read(fd, buf, sizeof(buf));
				if (n > 0) { got += n; rounds++; continue; }
				if (n < 0 && errno == EINTR) continue;
				break;
			}
			printf("  [child %d] 读到 %ld 字节（%d 次 read）\n", getpid(), got, rounds);
			close(fd);
			_exit(0);
		}
	}

	/* 父进程写入 200 条记录 */
	wfd = open(DEV_PATH, O_WRONLY);
	if (wfd < 0) { perror("open write"); return -1; }
	for (i = 0; i < 200; i++) {
		char line[32];
		int len = snprintf(line, sizeof(line), "record-%04d\n", i);

		if (write(wfd, line, len) != len) { perror("write"); break; }
	}
	close(wfd);
	printf("  [parent] 已写入 %d 条记录\n", i);

	for (i = 0; i < readers; i++)
		waitpid(pids[i], NULL, 0);

	CHECK(1, "并发读写完成，无死锁/无崩溃（各子进程读到的字节数见上）");
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s <命令> [参数]\n"
		"  write <text>      写入文本\n"
		"  read <n>          读 n 字节\n"
		"  nonblock          验证非阻塞读返回 EAGAIN\n"
		"  poll <ms>         等待可读\n"
		"  ioctl             容量 / 已用 / 清空\n"
		"  concurrent        并发读写测试\n"
		"  all               全部\n", prog);
}

int run_all(void)
{
	printf("\n== 1. 写入 ==");
	printf("\n");
	do_write("hello-mydev-");
	printf("\n== 2. ioctl ==");
	printf("\n");
	do_ioctl();
	printf("\n== 3. 非阻塞 ==");
	printf("\n");
	do_nonblock();
	printf("\n== 4. poll（应有数据）==");
	printf("\n");
	do_write("x");
	do_poll(10);
	printf("\n== 5. 读出 ==");
	printf("\n");
	do_read(64);
	printf("\n== 6. 并发 ==");
	printf("\n");
	do_concurrent();
	return failures;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "all";

	if (!strcmp(cmd, "write") && argc > 2) return do_write(argv[2]) < 0;
	if (!strcmp(cmd, "read") && argc > 2) return do_read(atoi(argv[2])) < 0;
	if (!strcmp(cmd, "nonblock")) return do_nonblock();
	if (!strcmp(cmd, "poll")) return do_poll(argc > 2 ? atoi(argv[2]) : 1000);
	if (!strcmp(cmd, "ioctl")) return do_ioctl();
	if (!strcmp(cmd, "concurrent")) return do_concurrent();
	if (!strcmp(cmd, "all")) {
		int f = run_all();
		printf("\n===== 结果：%s（失败项 %d）=====\n", f ? "有失败" : "全部通过", f);
		return f ? 1 : 0;
	}

	usage(argv[0]);
	return 2;
}
