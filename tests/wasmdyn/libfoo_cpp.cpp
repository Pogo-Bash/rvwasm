// libfoo: the C++ half. A class with a virtual function defined in the library, an exception
// thrown in the library and caught in the program (and the other way round), typeinfo used
// across modules (dynamic_cast).
#include <stdexcept>
#include <string>
#include "foo.h"

struct __attribute__((visibility("default"))) FooError : std::runtime_error {
	int code;
	FooError(const std::string &what, int c) : std::runtime_error(what), code(c) {}
};

Shape::~Shape() {}
const char *Shape::name() const { return "shape"; }
int Square::area() const { return side * side; }
const char *Square::name() const { return "square"; }

extern "C" Shape *foo_make_square(int side) { return new Square(side); }

extern "C" int foo_area(const Shape *s) { return s->area(); }

// Throws a FooError when side < 0: the program catches it by type.
extern "C" int foo_checked_area(int side)
{
	if (side < 0)
		throw FooError("negative side", side);
	Square s(side);
	return s.area();
}

// Calls back into the program, which may throw: caught here by type.
extern "C" int foo_catch(int (*fn)(int), int x)
{
	try {
		return fn(x);
	} catch (const std::exception &e) {
		return -1000 - (int)std::string(e.what()).size();
	}
}

// dynamic_cast on an object made in the program (its vtable is the program's for its own types).
extern "C" int foo_is_square(Shape *s) { return dynamic_cast<Square *>(s) != nullptr; }

static std::string *made_at_load = new std::string("static ctor ran");
extern "C" const char *foo_static_string() { return made_at_load->c_str(); }
