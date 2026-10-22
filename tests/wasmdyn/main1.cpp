// Linked against libfoo.so (dylink.0 NEEDED): calls, data, function pointers both ways, virtual
// functions across the boundary, exceptions both ways, dynamic_cast on objects of either module,
// thread-local variables of the library, its constructors.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "foo.h"

static int fails;
#define CHECK(c) do { if (c) std::printf("ok   %s\n", #c); else { std::printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

struct Circle : Shape {	// a subclass of the library's class, made here
	int r;
	explicit Circle(int r) : r(r) {}
	int area() const override { return 3 * r * r; }
};

static int triple(int x) { return 3 * x; }
static int thrower(int x) { if (x > 5) throw std::runtime_error("too big"); return x; }

struct FooErrorView : std::runtime_error { int code; };

int main(int argc, char **argv)
{
	std::printf("main1: %s, argc %d\n", argv[0], argc);
	CHECK(foo_counter == 42);			// 40, and 2 from libfoo's constructor
	CHECK(foo_add(1, 2) == 45);
	foo_counter = 0;
	CHECK(foo_add(1, 2) == 3);			// the program wrote the library's data
	CHECK(std::strcmp(foo_greeting(), "hello from libfoo") == 0);	// a data relocation in libfoo
	foo_callback = triple;				// a program function, called by the library
	CHECK(foo_call(7) == 21);
	CHECK(foo_strlen_ptr()("four") == 4);		// libc's function, by pointer from the library
	CHECK(foo_strlen_ptr() == &std::strlen);	// the same pointer as the program's
	CHECK(foo_weak() == 1);
	CHECK(*foo_tls_addr() == 7);
	CHECK(foo_tls_bump() == 8);
	CHECK(std::strcmp(foo_static_string(), "static ctor ran") == 0);

	Shape *sq = foo_make_square(5);
	CHECK(sq->area() == 25);			// a virtual call into the library
	CHECK(std::strcmp(sq->name(), "square") == 0);
	CHECK(dynamic_cast<Square *>(sq) != nullptr);	// RTTI of the library's class, cast here
	Circle c(2);
	CHECK(foo_area(&c) == 12);			// the library calls the program's override
	CHECK(std::strcmp(c.name(), "shape") == 0);	// the library's base implementation
	CHECK(foo_is_square(sq) == 1);
	CHECK(foo_is_square(&c) == 0);			// dynamic_cast in the library on the program's class
	delete sq;					// a virtual destructor in the library

	int caught = 0;
	try {
		foo_checked_area(-3);
	} catch (const std::runtime_error &e) {		// thrown in the library, caught by base type here
		caught = std::strcmp(e.what(), "negative side") == 0;
	}
	CHECK(caught);
	CHECK(foo_catch(thrower, 3) == 3);
	CHECK(foo_catch(thrower, 9) == -1007);		// thrown here, caught in the library
	std::printf("main1: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
