CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra
LDFLAGS  := -ldl -lpthread

.PHONY: all test clean

all: memtrack.so

memtrack.so: memtrack.cpp
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ $< $(LDFLAGS)

test_app: test_app.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

test: memtrack.so test_app
	LD_PRELOAD=./memtrack.so ./test_app

clean:
	rm -f memtrack.so test_app
