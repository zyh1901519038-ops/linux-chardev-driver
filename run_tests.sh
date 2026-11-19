#!/usr/bin/env bash
# 一键：构建 → 加载 → 测试 → 卸载
# 用法：sudo bash run_tests.sh [buf_size]
set -u

BUF=${1:-1024}
MOD=mydev
DEV=/dev/mydev
FAIL=0

say() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
ok()  { printf '  \033[32m[PASS]\033[0m %s\n' "$*"; }
err() { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }

cleanup() {
	say "清理：卸载模块"
	rmmod "$MOD" 2>/dev/null && ok "已卸载" || true
}
trap cleanup EXIT

say "0. 环境检查"
if [ "$(id -u)" -ne 0 ]; then echo "请用 sudo 运行"; exit 1; fi
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
	echo "找不到内核头文件：/lib/modules/$(uname -r)/build"
	echo "Ubuntu: sudo apt install linux-headers-\$(uname -r)"
	exit 1
fi
ok "内核头文件存在"

say "1. 构建内核模块"
make -s modules && ok "mydev.ko 构建成功" || { err "构建失败"; exit 1; }

say "2. 构建用户态测试程序"
make -s -C userspace && ok "mydev_test 构建成功" || { err "构建失败"; exit 1; }

say "3. 加载模块（buf_size=$BUF）"
insmod "./${MOD}.ko" buf_size="$BUF" || { err "insmod 失败，看 dmesg"; dmesg | tail -n 20; exit 1; }
sleep 0.3
ok "加载成功"
dmesg | grep "$MOD" | tail -n 3

say "4. 设备节点"
if [ -c "$DEV" ]; then
	ok "$DEV 存在"
	ls -l "$DEV"
else
	err "$DEV 不存在"
fi

say "5. 功能测试"
./userspace/mydev_test all
[ $? -eq 0 ] && ok "全部用例通过" || err "有失败用例"

say "6. 卸载后可再加载（验证资源正确释放）"
rmmod "$MOD" && ok "卸载成功" || err "卸载失败"
if insmod "./${MOD}.ko" buf_size="$BUF" 2>/dev/null; then
	ok "二次加载成功（说明 init/exit 的资源释放是对的）"
else
	err "二次加载失败（可能有资源没释放干净）"
fi

say "7. 内核日志"
dmesg | grep "$MOD" | tail -n 15

printf '\n================ 总结 ================\n'
if [ $FAIL -eq 0 ]; then printf '\033[32m全部通过\033[0m\n'; else printf '\033[31m失败 %d 项\033[0m\n' "$FAIL"; fi
exit $FAIL
