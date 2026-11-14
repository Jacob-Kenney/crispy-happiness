CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
SRC = src/game.cpp src/scx_tables.cpp

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIB = libcanyon_env.dylib
  SHARED = -dynamiclib
else
  LIB = libcanyon_env.so
  SHARED = -shared
endif

.PHONY: all lib test clean

all: lib test

lib: $(LIB)

$(LIB): $(SRC) src/env_c_api.cpp include/canyon/game.hpp
	$(CXX) $(CXXFLAGS) -fPIC $(SHARED) -o $@ $(SRC) src/env_c_api.cpp

build/tests: $(SRC) tests/tests.cpp include/canyon/game.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) tests/tests.cpp

test: build/tests
	./build/tests

clean:
	rm -rf build $(LIB)