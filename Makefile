# Macintosh Homogeneous Substrate — Build System
# Ahmad Ali Parr — hand-rolled C, no AI generation

CC     = gcc
CFLAGS = -O2 -g -Wall -Wextra -std=c11
NVCC   = nvcc
NVFLAGS= -O2 -std=c++17 -arch=sm_80

C_SRCS = c/core.c c/runtime.c c/gc/gc.c c/gc/barriers.c c/gc/concurrent.c \
         c/jit/x86_64.c c/compiler/ssa.c

CUDA_SRCS = cuda/csr.cu cuda/ell.cu cuda/spgemm.cu

C_OBJS    = $(C_SRCS:.c=.o)
CUDA_OBJS = $(CUDA_SRCS:.cu=.o)

.PHONY: all c cuda clean count

all: c cuda

c: $(C_OBJS)
	@echo "C build complete."

cuda: $(CUDA_OBJS)
	@echo "CUDA build complete."

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cu
	$(NVCC) $(NVFLAGS) -c $< -o $@

count:
	@find . -name "*.c" -o -name "*.cu" -o -name "*.bas" -o -name "*.lisp" -o -name "*.dylan" | \
	  xargs wc -l 2>/dev/null | tail -1

clean:
	find . -name "*.o" -delete
	@echo "Clean."
