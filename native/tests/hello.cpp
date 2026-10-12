// Stage (a) of docs/native-wasm.md: C++ with exceptions, threads, iostreams, std::string, RTTI, statics.
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <memory>
#include <chrono>

struct Base { virtual ~Base() {} virtual std::string name() const { return "base"; } };
struct Derived : Base { std::string name() const override { return "derived"; } };
struct Guard { const char *s; ~Guard() { std::printf("unwound %s\n", s); } };

static int depth(int n) {
	Guard g{"frame"};
	if (n == 0)
		throw std::runtime_error("deep");
	return depth(n - 1) + 1;
}

thread_local int tls_counter = 0;

int main() {
	auto t0 = std::chrono::steady_clock::now();
	std::cout << "hello from native C++" << std::endl;
	try {
		depth(2);
	} catch (const std::exception &e) {
		std::cout << "caught: " << e.what() << std::endl;
	}
	try {
		throw 42;
	} catch (int v) {
		std::cout << "caught int " << v << std::endl;
	}
	std::unique_ptr<Base> b(new Derived);
	std::cout << "rtti: " << b->name() << " " << (dynamic_cast<Derived *>(b.get()) ? "ok" : "bad") << std::endl;

	std::atomic<long> sum{0};
	std::mutex m;
	std::vector<std::thread> ts;
	for (int i = 0; i < 4; i++)
		ts.emplace_back([i, &sum, &m] {
			for (int k = 0; k < 1000; k++)
				tls_counter++;
			try {
				if (i & 1)
					throw std::string("thread ") + std::to_string(i);
			} catch (const std::string &s) {
				std::lock_guard<std::mutex> l(m);
				std::cout << "caught in " << s << std::endl;
			}
			sum += tls_counter * (i + 1);
		});
	for (auto &t : ts)
		t.join();
	std::cout << "threads sum " << sum << " (expect 10000), main tls " << tls_counter << std::endl;
	auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
	std::cout << "took " << us << " us" << std::endl;
	return 0;
}
