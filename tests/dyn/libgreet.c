/* A shared library for main.c to dlopen: its own data, TLS and constructor. */
#include <stdio.h>

int greet_calls;
static __thread int bonus = 1;

__attribute__((constructor)) static void loaded(void)
{
	puts("libgreet: constructor");
}

int greet(int x)
{
	greet_calls++;
	return x * 2 + bonus;
}
