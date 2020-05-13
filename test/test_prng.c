#include "sample.h"

#include <stdio.h>

/* SPDX-License-Identifier: MIT */

int
main()
{
	struct sample_state state = { 0 };
	bool dummy;

	for (;;)
		printf("%.18f\n", sample_uniform(&state, &dummy));

	return 0;
}
