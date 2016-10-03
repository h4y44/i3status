#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_version.h>

#include "i3status.h"

void 
print_brightness(yajl_gen json_gen, char *buffer, const char *format, const char *path,
		int brightness_max) {
	const char *walk;
	char *outwalk = buffer;

	FILE *f_cur = fopen(path, "rb");
	if (!f_cur) {
		goto error;
	} 

	int brightness_cur;
	fscanf(f_cur, "%d", &brightness_cur);
	fclose(f_cur);

	int brightness_per = (100*brightness_cur)/brightness_max;

	for (walk = format; *walk != '\0'; walk++) {
		if (*walk != '%') {
			*(outwalk++) = *walk;
			continue;
		}

		if (BEGINS_WITH(walk+1, "percent")) {
			outwalk += sprintf(outwalk, "%02d%s", brightness_per, pct_mark);
			walk += strlen("percent");
		}
	}

	OUTPUT_FULL_TEXT(buffer);
	return;
error:
    OUTPUT_FULL_TEXT("cant read screen brightness");
    (void)fputs("i3status: Cannot read screen brightness\n", stderr);
}
