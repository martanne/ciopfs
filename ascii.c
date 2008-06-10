#include <ctype.h>

static inline bool str_contains_upper(const char *s)
{
	while (*s) {
		if (isupper(*s++))
			return true;
	}
	return false;
}

static inline char *str_fold(const char *src)
{
	char *t, *dest = malloc(strlen(src));
	if (!dest)
		return NULL;
	for (t = dest; *src; src++, t++)
		*t = tolower(*src);
	*t = '\0';
	return dest;
}
