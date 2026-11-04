########################################
# Compiler & Linker
########################################
CC      = gcc
LINK    = gcc
DPUCC   = dpu-upmem-dpurte-clang
DPULINK = dpu-upmem-dpurte-clang

########################################
# Directories
########################################
DPU_DIR   := dpu
HOST_DIR  := host
BUILD_DIR := bin
OBJ_DIR   := obj
INC_DIR   := include

########################################
# UPMEM SDK path & environment
########################################
ifndef UPMEM_HOME
$(error UPMEM_HOME is not set; export it to your UPMEM SDK path, e.g. export UPMEM_HOME=/path/to/upmem-2024.2.0-Linux-x86_64)
endif
export PKG_CONFIG_PATH := ${UPMEM_HOME}/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PATH := ${UPMEM_HOME}/bin:$(PATH)
# libofis.so's own dependency (libdpuverbose) is resolved through this at link time.
export LD_LIBRARY_PATH := ${UPMEM_HOME}/lib:$(LD_LIBRARY_PATH)
# libofis.so resolves this library by its 0.0 soname
$(shell cd ${UPMEM_HOME}/lib && [ -e libdpuverbose.so.0.0 ] || ln -s libdpuverbose.so.2024.2 libdpuverbose.so.0.0)

########################################
# Configs
########################################
NR_DPUS     ?= 1024
NR_TASKLETS ?= 16
GRAPH       ?= CJ
# P1=HOUSE5 (default), P2=HOUSE5_X, P3=K4_TAIL5, P4=P_P6, P5=HOUSE5_Y
PATTERN     ?= HOUSE5
# Compact prefetch pipeline producer count (used only when
# OFIS_COMPACT_PIPELINE is defined - see include/common.h).
# Override from the command line: make all PIPELINE_PRODUCERS=8
PIPELINE_PRODUCERS ?= 16

# Extra -D flags selecting the engine and its options (see include/common.h).
#   PimPam     : -DOFIS_NO_MODE
#   OFIS-PimPam: -DOFIS_NO_COMPACT_PIPELINE -DOFIS_NO_THREAD_INIT_CSR_XFER \
#                -DOFIS_PREBUILD_ALL -DOFIS_COLLECT_PER_BATCH
EXTRA_DEFS ?=

########################################
# Compiler & Linker Flags
########################################
COMMON_CCFLAGS := -c -w -g -O2 -I${INC_DIR} \
	-DNR_TASKLETS=${NR_TASKLETS} -DNR_DPUS=${NR_DPUS} -D${GRAPH} \
	-DDPU_BINARY=\"${BUILD_DIR}/dpu\" -DDPU_ALLOC_BINARY=\"${BUILD_DIR}/dpu_alloc\" \
	-D${PATTERN} -DDATA_PATH=\"data/bin/${GRAPH}.bin\" \
	-DOFIS_COMPACT_PIPELINE_PRODUCERS=${PIPELINE_PRODUCERS} \
	${EXTRA_DEFS}

HOST_CCFLAGS := ${COMMON_CCFLAGS} -std=c11 `dpu-pkg-config --cflags dpu` \
	-I${UPMEM_HOME}/include -I${UPMEM_HOME}/include/dpu \
	-pthread -D_GNU_SOURCE

DPU_CCFLAGS  := ${COMMON_CCFLAGS}

COMMON_LFLAGS := -DNR_TASKLETS=${NR_TASKLETS}
HOST_LFLAGS   := ${COMMON_LFLAGS} `dpu-pkg-config --libs dpu` -pthread
DPU_LFLAGS    := ${COMMON_LFLAGS}

########################################
# Includes
########################################
INC_FILE := ${INC_DIR}/common.h ${INC_DIR}/cyclecount.h ${INC_DIR}/timer.h ${INC_DIR}/dpu_mine.h

########################################
# Targets
########################################
.PHONY: all all_before host dpu clean test test_single test_all test_bitmap

all: all_before ${BUILD_DIR}/host ${BUILD_DIR}/dpu ${BUILD_DIR}/dpu_alloc

all_before:
	mkdir -p ${BUILD_DIR}
	mkdir -p ${OBJ_DIR}
	mkdir -p ${OBJ_DIR}/${HOST_DIR}
	mkdir -p ${OBJ_DIR}/${DPU_DIR}
	mkdir -p result

########################################
# Host Binary
########################################
${BUILD_DIR}/host: ${OBJ_DIR}/${HOST_DIR}/main.o \
	${OBJ_DIR}/${HOST_DIR}/partition.o \
	${OBJ_DIR}/${HOST_DIR}/mine.o \
	${OBJ_DIR}/${HOST_DIR}/set_op.o \
	${OBJ_DIR}/${HOST_DIR}/heap.o
	${LINK} $^ ${HOST_LFLAGS} -o $@

########################################
# DPU Binaries
########################################
${BUILD_DIR}/dpu: ${OBJ_DIR}/${DPU_DIR}/main.o \
	${OBJ_DIR}/${DPU_DIR}/set_op.o \
	${OBJ_DIR}/${DPU_DIR}/${PATTERN}.o
	${DPULINK} ${DPU_LFLAGS} $^ -o $@

${BUILD_DIR}/dpu_alloc: ${OBJ_DIR}/${DPU_DIR}/partition.o
	${DPULINK} ${DPU_LFLAGS} $^ -o $@

########################################
# Object Rules
########################################
${OBJ_DIR}/${HOST_DIR}/%.o: ${HOST_DIR}/%.c ${INC_FILE}
	${CC} ${HOST_CCFLAGS} $< -o $@

${OBJ_DIR}/${DPU_DIR}/%.o: ${DPU_DIR}/%.c ${INC_FILE}
	${DPUCC} ${DPU_CCFLAGS} $< -o $@

########################################
# Clean
########################################
clean:
	@rm -rf ${BUILD_DIR} ${OBJ_DIR}

########################################
# Test Commands
########################################
test:
	make clean --no-print-directory
	make all --no-print-directory
	sudo LD_LIBRARY_PATH=${UPMEM_HOME}/lib:$${LD_LIBRARY_PATH} ./${BUILD_DIR}/host

test_single:
	make clean --no-print-directory
	NR_DPUS=1 NR_TASKLETS=1 make all --no-print-directory
	sudo LD_LIBRARY_PATH=${LD_LIBRARY_PATH} ./${BUILD_DIR}/host

test_all:
	GRAPH=WV PATTERN=HOUSE5 make test --no-print-directory
	GRAPH=PP PATTERN=HOUSE5 make test --no-print-directory
	GRAPH=CA PATTERN=HOUSE5 make test --no-print-directory
	GRAPH=YT PATTERN=HOUSE5 make test --no-print-directory
	GRAPH=PT PATTERN=HOUSE5 make test --no-print-directory
	GRAPH=LJ PATTERN=HOUSE5 make test --no-print-directory

test_bitmap:

test_time:

.SILENT: