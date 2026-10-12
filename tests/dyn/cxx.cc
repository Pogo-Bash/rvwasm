// C++ through the dynamic loader: libstdc++ and libgcc_s, static constructors,
// exceptions (which unwind the stack using tables the loader has to expose),
// virtual calls, containers, iostreams with floating point.
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct Shape {
	virtual ~Shape() = default;
	virtual double area() const = 0;
};

struct Rect : Shape {
	double w, h;
	Rect(double w, double h) : w(w), h(h) {}
	double area() const override { return w * h; }
};

struct Circle : Shape {
	double r;
	explicit Circle(double r) : r(r) {}
	double area() const override { return 3.141592653589793 * r * r; }
};

static std::map<std::string, int> counts = {{"static", 1}};

static int checked(int depth)
{
	if (depth == 0)
		throw std::runtime_error("thrown from the bottom");
	std::string guard(static_cast<size_t>(depth), 'x');	// destroyed during unwinding
	return checked(depth - 1) + static_cast<int>(guard.size());
}

int main()
{
	std::vector<std::unique_ptr<Shape>> shapes;
	shapes.push_back(std::make_unique<Rect>(3, 4));
	shapes.push_back(std::make_unique<Circle>(1.5));
	shapes.push_back(std::make_unique<Rect>(0.5, 0.25));

	double total = 0;
	for (const auto &s : shapes)
		total += s->area();
	std::cout << "total area " << total << "\n";

	std::vector<int> v;
	for (int i = 0; i < 1000; i++)
		v.push_back((i * 7919) % 1000);
	std::sort(v.begin(), v.end());
	std::cout << "sorted " << v.front() << " " << v[500] << " " << v.back() << "\n";

	for (const char *w : {"pear", "apple", "pear", "fig", "pear"})
		counts[w]++;
	for (const auto &kv : counts)
		std::cout << kv.first << "=" << kv.second << " ";
	std::cout << "\n";

	try {
		checked(20);
	} catch (const std::exception &e) {
		std::cout << "caught: " << e.what() << "\n";
	}
	try {
		std::vector<int>(3).at(10);
	} catch (const std::out_of_range &) {
		std::cout << "caught out_of_range\n";
	}
	return 0;
}
