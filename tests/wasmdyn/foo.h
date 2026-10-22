/* libfoo's interface (libfoo.c, libfoo_cpp.cpp). */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int foo_counter;
extern int (*foo_callback)(int);
int foo_add(int a, int b);
const char *foo_greeting(void);
int foo_call(int x);
int *foo_tls_addr(void);
int foo_tls_bump(void);
size_t (*foo_strlen_ptr(void))(const char *);
int foo_weak(void);
int foo_checked_area(int side);
int foo_catch(int (*fn)(int), int x);
const char *foo_static_string(void);
#ifdef __cplusplus
}
struct __attribute__((visibility("default"))) Shape {
	virtual ~Shape();
	virtual int area() const = 0;
	virtual const char *name() const;
};
struct __attribute__((visibility("default"))) Square : Shape {
	int side;
	explicit Square(int s) : side(s) {}
	int area() const override;
	const char *name() const override;
};
extern "C" Shape *foo_make_square(int side);
extern "C" int foo_area(const Shape *s);
extern "C" int foo_is_square(Shape *s);
#endif
