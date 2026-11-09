# 归档：flit 级 AT 模型（高精度对照，周期精确）
# 与 ../ 的 transaction 级主线模型相互独立，可单独编译运行。
# 需要时用它核对某个微观效应（VC/arbiter/HOL），或校准 transaction 级模型。
SYSTEMC ?= ../../_tmp/systemc-c14
CXX     ?= g++
CXXFLAGS = -std=c++14 -O1 -I$(SYSTEMC)/include -I.
LDFLAGS  = -L$(SYSTEMC)/lib -lsystemc -lm

# 见上层 Makefile：MSYS make 会把子进程 TMP 重置成不可写的 C:\WINDOWS
TMPDIR_SAFE := $(shell python -c 'import tempfile; print(tempfile.gettempdir().replace(chr(92),"/"))')
ifneq ($(TMPDIR_SAFE),)
export TMP    := $(TMPDIR_SAFE)
export TEMP   := $(TMPDIR_SAFE)
export TMPDIR := $(TMPDIR_SAFE)
endif

all: noc_demo_flit

noc_demo_flit: noc_top.cpp noc_common.h router_at.h router_lt.h
	$(CXX) $(CXXFLAGS) noc_top.cpp -o noc_demo_flit $(LDFLAGS)

run: noc_demo_flit
	./noc_demo_flit at 4 4 30 1

clean:
	rm -f noc_demo_flit noc_demo_flit.exe
