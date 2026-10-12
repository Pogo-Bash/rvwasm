// Guest threads through the C++ standard library: std::thread, futures,
// condition variables, atomics, thread_local objects with destructors.
// Output does not depend on scheduling; only the main thread prints.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

static std::atomic<long> destroyed{0};

struct Tracker {
	long value = 0;
	~Tracker() { destroyed += value; }
};
static thread_local Tracker tracker;

static long fib(int n)
{
	return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

int main()
{
	// Parallel sum over slices of a vector.
	std::vector<long> data(100000);
	std::iota(data.begin(), data.end(), 1);
	std::vector<long> partial(8);
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; i++)
		threads.emplace_back([&, i] {
			auto first = data.begin() + i * data.size() / 8;
			auto last = data.begin() + (i + 1) * data.size() / 8;
			partial[i] = std::accumulate(first, last, 0L);
			tracker.value = i + 1;
		});
	for (auto &t : threads)
		t.join();
	std::printf("parallel sum: %ld, thread_local destructors: %ld\n",
		    std::accumulate(partial.begin(), partial.end(), 0L), destroyed.load());

	// Futures: values and an exception crossing threads.
	std::vector<std::future<long>> futures;
	for (int n = 15; n < 23; n++)
		futures.push_back(std::async(std::launch::async, fib, n));
	long total = 0;
	for (auto &f : futures)
		total += f.get();
	auto failing = std::async(std::launch::async, []() -> int { throw std::runtime_error("from a thread"); });
	try {
		failing.get();
	} catch (const std::exception &e) {
		std::printf("futures: %ld, exception \"%s\"\n", total, e.what());
	}

	// A promise fulfilled later, and wait_for timing out first.
	std::promise<int> promise;
	auto future = promise.get_future();
	bool timed_out = future.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
	std::thread setter([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		promise.set_value(7);
	});
	int got = future.get();
	setter.join();
	std::printf("promise: timed out first %s, then %d\n", timed_out ? "yes" : "no", got);

	// call_once, shared_mutex and a condition variable with a predicate.
	std::once_flag flag;
	std::shared_mutex shared;
	std::mutex m;
	std::condition_variable cv;
	int once = 0, turn = 0;
	long readers = 0;
	threads.clear();
	for (int i = 0; i < 6; i++)
		threads.emplace_back([&, i] {
			std::call_once(flag, [&] { once++; });
			{
				std::shared_lock lock(shared);
				std::lock_guard count(m);
				readers++;
			}
			// Each thread waits for its own turn: a strict order out of a condition variable.
			std::unique_lock lock(m);
			cv.wait(lock, [&] { return turn == i; });
			turn++;
			cv.notify_all();
		});
	for (auto &t : threads)
		t.join();
	std::printf("call_once: %d, readers: %ld, turns taken in order: %d\n", once, readers, turn);
	std::printf("hardware_concurrency: %s\n", std::thread::hardware_concurrency() >= 1 ? "at least 1" : "unknown");
	return 0;
}
