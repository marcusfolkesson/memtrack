CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra
LDFLAGS  := -ldl -lpthread

.PHONY: all test clean

all: memtrack.so memview

memtrack.so: memtrack.cpp
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ $< $(LDFLAGS)

memview: memview.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lncurses

test_app: test_app.cpp
	$(CXX) $(CXXFLAGS) -rdynamic -o $@ $< -lpthread

test: memtrack.so test_app
	LD_PRELOAD=./memtrack.so ./test_app

clean:
	rm -f memtrack.so test_app memview
