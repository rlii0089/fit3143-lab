MPICC ?= mpicc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
PROG = wsn_v2
# Code shared by every version lives in one place.
COMMON = ../lab-3-common

# Apple clang has no built-in OpenMP: use Homebrew's libomp on macOS.
# Linux/CAAS (gcc) supports -fopenmp directly.
ifeq ($(shell uname -s),Darwin)
LIBOMP ?= $(shell brew --prefix libomp 2>/dev/null)
OMPFLAGS = -Xpreprocessor -fopenmp -I$(LIBOMP)/include
OMPLIBS = -L$(LIBOMP)/lib -lomp
else
OMPFLAGS = -fopenmp
OMPLIBS =
endif

.PHONY: all clean
all: $(PROG)

$(PROG): $(PROG).c $(COMMON)/common.c $(COMMON)/common.h
	$(MPICC) $(CFLAGS) $(OMPFLAGS) -I$(COMMON) $(PROG).c $(COMMON)/common.c -o $@ $(OMPLIBS)

# Only removes the binary: base-station logs are part of the submission.
clean:
	rm -f $(PROG)
