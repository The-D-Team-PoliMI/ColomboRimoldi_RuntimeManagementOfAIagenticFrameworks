.PHONY: all build run clean

all: run

build:
	@cmake -B build > /dev/null
	@cmake --build build -j$$(nproc) > /dev/null

run: build
	@./build/data_agent_cpp

clean:
	@rm -rf build