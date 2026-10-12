/* Runs without making a system call for as long as it is let. Expect kill -9 (or Ctrl-C) to end it. */
int main(int argc, char **argv)
{
	volatile unsigned long n = 0;

	for (;;)
		n++;
	return 0;
}
