#include <glib.h>

static inline bool str_contains_upper(const char *s)
{
	while (*s) {
		if(g_unichar_isupper(g_utf8_get_char(s)))
			return true;
		s = g_utf8_next_char(s);
	}
	return false;
}

static inline char *str_fold(const char *s)
{
	return g_utf8_casefold(s, -1);
}
