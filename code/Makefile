# SystemC 2.3.4（库与示例必须用同一 C++ 标准编译，见文章 §11 的 ABI 说明）
# 编译时用 make SYSTEMC=/your/systemc/prefix 覆盖；默认值是本仓库在 Windows 上
# 的本地构建目录，Linux/macOS 用户请显式传入自己的 SystemC 安装前缀。
SYSTEMC ?= ../_tmp/systemc-c14
CXX     ?= g++
CXXFLAGS = -std=c++14 -O1 -I$(SYSTEMC)/include -I.
LDFLAGS  = -L$(SYSTEMC)/lib -lsystemc -lm

# MSYS make 会把子进程 TMP 重置成 C:\WINDOWS（不可写），导致 g++ 报
# "Cannot create temporary file in C:\WINDOWS\"。用 Python 探测一个确实可写的
# 临时目录并导出；跨平台通用，没有 Python 时退回系统默认。
TMPDIR_SAFE := $(shell python -c 'import tempfile; print(tempfile.gettempdir().replace(chr(92),"/"))')
ifneq ($(TMPDIR_SAFE),)
export TMP    := $(TMPDIR_SAFE)
export TEMP   := $(TMPDIR_SAFE)
export TMPDIR := $(TMPDIR_SAFE)
endif

all: noc_txn_demo tour

noc_txn_demo: noc_txn_top.cpp noc_txn_common.h router_txn.h router_txn_lt.h
	$(CXX) $(CXXFLAGS) noc_txn_top.cpp -o noc_txn_demo $(LDFLAGS)

tour: tlm_feature_tour.cpp noc_txn_common.h
	$(CXX) $(CXXFLAGS) tlm_feature_tour.cpp -o tour $(LDFLAGS)

# 架构探索主线：默认轻载 / 重载对照
run-at: noc_txn_demo
	./noc_txn_demo at 4 4 30 $(or $(GAP),8) xy mesh uniform 64 16

run-lt: noc_txn_demo
	./noc_txn_demo lt 4 4 30 $(or $(GAP),8) xy mesh uniform 64 16

run-tour: tour
	./tour

# 参数扫描：跑实验 -> 出图 -> 同步文章数字 -> 重生成 HTML，一条龙
sweep: noc_txn_demo
	python ../tools/sweep.py
	python ../tools/make_figs_txn.py
	python ../tools/refresh_article.py
	python ../tools/md2html.py

# 归档的 flit 级高精度对照
flit: 
	$(MAKE) -C flit_level

clean:
	rm -f noc_txn_demo tour noc_txn_demo.exe tour.exe
