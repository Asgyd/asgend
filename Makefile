.PHONY: all build test clean run
all: build
build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build --parallel

test: build
	ctest --test-dir build --output-on-failure

run: build
	./build/agent-orchestrator -c config/config.json

clean:
	rm -rf build
