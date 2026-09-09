# Requires ROCm to be installed (https://rocm.docs.amd.com).
# ROCM_PATH defaults to /opt/rocm — override if installed elsewhere:
#   make ROCM_PATH=/opt/rocm-6.4.0
ROCM_PATH ?= /opt/rocm

# Optional: use a newer libhsakmt built from source (e.g. rocm-systems/rocr-runtime).
#   make HSAKMT_PATH=/path/to/rocr-runtime
ifdef HSAKMT_PATH
HSAKMT_INC = -I$(HSAKMT_PATH)/libhsakmt/include
HSAKMT_LIB = $(HSAKMT_PATH)/build/libhsakmt/archive/libhsakmt.a
HSAKMT_DEF =
else
HSAKMT_INC =
HSAKMT_LIB = -L$(ROCM_PATH)/lib -lhsakmt
HSAKMT_DEF =
endif

CXX      = g++

# HsaMemMapFlags removed in ROCm ≥10.x (ROCM-1028)
HAS_MAP_FLAGS := $(shell grep -c HsaMemMapFlags $(ROCM_PATH)/include/hsakmt/hsakmttypes.h 2>/dev/null)
ifeq ($(HAS_MAP_FLAGS),0)
COMPAT_DEF = -DHSAKMT_NO_MAP_FLAGS
endif

CXXFLAGS = -std=c++17 -Wall -O2 $(HSAKMT_INC) -I$(ROCM_PATH)/include -Iinclude $(HSAKMT_DEF) $(COMPAT_DEF)

# libdrm_amdgpu, libdrm, and libnuma are dependencies of libhsakmt.
LDFLAGS  = $(HSAKMT_LIB) -ldrm_amdgpu -ldrm -lnuma

SRC = $(wildcard src/*.cpp)
OUT = sdma_collective

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

clean:
	rm -f $(OUT)
