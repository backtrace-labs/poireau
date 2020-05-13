#include <stdlib.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
	size_t alloc_size = 1024 * 1024;
	size_t num_repeat = 100 * 1000 * 1000;

	if (argc > 1)
		alloc_size = atoi(argv[1]);

	if (argc > 2)
		num_repeat = atoi(argv[2]);

	for (size_t i = 0; i < num_repeat; i++) {
		void *buf;

		buf = malloc(alloc_size);
		/* Some compilers optimize away malloc/free. */
		asm volatile("" : "+r"(buf) :: "memory");
		free(buf);
	}

	return 0;
}
