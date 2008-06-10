#include <unicode/ustring.h>
#include <unicode/uchar.h>

static inline UChar *utf8_to_utf16(const char *str, int32_t *length)
{
	UChar *ustr;
	UErrorCode status = U_ZERO_ERROR;

	u_strFromUTF8(NULL, 0, length, str, -1, &status);
	status = U_ZERO_ERROR;
	(*length)++; /* for the NUL char */
	ustr = malloc(sizeof(UChar) * (*length));
	if (!ustr)
		return NULL;
	u_strFromUTF8(ustr, *length, NULL, str, -1, &status);
	if (U_FAILURE(status)) {
		free(ustr);
		return NULL;
	}
	return ustr;
}

static inline char *utf16_to_utf8(UChar *ustr, int32_t *length)
{
	char *str;
	UErrorCode status = U_ZERO_ERROR;

	u_strToUTF8(NULL, 0, length, ustr, -1, &status);
	status = U_ZERO_ERROR;
	(*length)++; /* for the NUL char */
	str = malloc(*length);
	if (!str)
		return NULL;
	u_strToUTF8(str, *length, NULL, ustr, -1, &status);
	if (U_FAILURE(status)) {
		free(str);
		return NULL;
	}
	return str;
}

static inline bool str_contains_upper(const char *s)
{
	bool ret = false;
	int32_t length, i;
	UChar32 c;
	UChar *ustr = utf8_to_utf16(s, &length);
	if (!ustr)
		return true;
	for (i = 0; i < length; /* U16_NEXT post-increments */) {
		U16_NEXT(ustr, i, length, c);
		if (u_isupper(c)) {
			ret = true;
			goto out;
		}
	}
out:
	free(ustr);
	return ret;
}

static inline char *str_fold(const char *s)
{
	int32_t length;
	char *str;
	UChar *ustr;
	UErrorCode status = U_ZERO_ERROR;

	ustr = utf8_to_utf16(s, &length);
	if (!ustr)
		return NULL;
	u_strFoldCase(ustr, length, ustr, length, U_FOLD_CASE_EXCLUDE_SPECIAL_I, &status);
	if (U_FAILURE(status))
		return NULL;
	str = utf16_to_utf8(ustr, &length);
	free(ustr);
	return str;
}
