SUBDIRS = src test bench
.PHONY: all clean $(SUBDIRS)
.NOTPARALLEL: $(SUBDIRS)

LOG ?= 1
USE_DOUBLE ?= 1
USE_ORDERING ?= 0
USE_BARRIER ?= 0

CXX ?= g++
MPI_CXX ?= mpicxx
CXXFLAGS := -std=c++14 -Wall -fPIC -fopenmp
CPPFLAGS :=
LDFLAGS :=
LIBS := -lgomp -ltbb

# Boost
CPPFLAGS += -I$(BOOST_DIR)/include
LDFLAGS += -L$(BOOST_DIR)/lib

ifeq ($(DEBUG), 1)
	CPPFLAGS += -D_DEBUG
	CXXFLAGS += -g -O0
else
	CPPFLAGS += -DNDEBUG
	CXXFLAGS += -O3 -funroll-loops -mtune=native -march=native
endif

ifeq ($(USE_DOUBLE), 1)
	CPPFLAGS += -D_USE_DOUBLE
endif

ifeq ($(USE_ORDERING), 1)
	CPPFLAGS += -D_USE_ORDERING
endif

ifeq ($(USE_BARRIER), 1)
	CPPFLAGS += -D_USE_BARRIER
endif

ifeq ($(LOG), 1)
	CPPFLAGS +=  -D_LOG_INFO
endif

export CXX
export MPI_CXX
export CPPFLAGS
export CXXFLAGS
export LDFLAGS
export LIBS

lib_SOURCES := $(wildcard src/*.cpp)
lib_OBJECTS := $(lib_SOURCES:%.cpp=%.o)
export lib_OBJECTS

$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

all: $(SUBDIRS)

clean:	$(SUBDIRS)
	$(RM) -f  *.so *.o *.s src/*.o src/*.s *~
