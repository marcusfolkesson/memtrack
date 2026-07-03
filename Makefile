CXX      := $(CROSS_TOOLCHAIN)-g++
HOSTCXX  ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra
LDFLAGS  := -ldl -lpthread

.PHONY: all test clean

all: memtrack.so memview

memtrack.so: memtrack.cpp
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ $< $(LDFLAGS)

memview: memview.cpp
	$(HOSTCXX) $(CXXFLAGS) -o $@ $< -lncurses

test_app: test_app.cpp
	$(CXX) -O0 -std=c++17 -Wall -Wextra -g -rdynamic -o $@ $< -lpthread

test: memtrack.so test_app
	@echo "Running test_app..."
	@LD_PRELOAD=./memtrack.so MEMTRACK_STACK_DEPTH=64 ./test_app 2>mt.log; \
	app_exit=$$?; \
	if [ $$app_exit -ne 0 ]; then \
		echo "FAIL: test_app exited with code $$app_exit"; \
		exit 1; \
	fi
	@echo "Verifying memtrack log..."
	@./verify.sh mt.log

clean:
	rm -f memtrack.so test_app memview mt.log
