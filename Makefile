obj-m += mydev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean userspace load unload info test help

all: modules

modules:
	@echo "==> 构建内核模块（KDIR=$(KDIR)）"
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C userspace clean 2>/dev/null || true

userspace:
	@echo "==> 构建用户态测试程序"
	$(MAKE) -C userspace

load: modules
	@echo "==> 加载模块"
	sudo insmod mydev.ko $(if $(BUF),buf_size=$(BUF),)
	@ls -l /dev/mydev

unload:
	@echo "==> 卸载模块"
	sudo rmmod mydev || true

info:
	@lsmod | grep mydev || echo "(未加载)"
	@dmesg | grep mydev | tail -n 8

test: test-all

test-all:
	@bash ./run_tests.sh

help:
	@echo "make            - 构建内核模块"
	@echo "make userspace  - 构建用户态测试程序"
	@echo "make load BUF=4096 - 加载模块（可指定缓冲区大小）"
	@echo "make unload     - 卸载"
	@echo "make test       - 一键：构建 + 加载 + 全部测试 + 卸载"
	@echo "make info       - 查看加载状态与内核日志"
