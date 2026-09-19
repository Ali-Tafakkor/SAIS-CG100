#ifndef RJ_JSON_H
#define RJ_JSON_H
#include <stddef.h>
#define RJ_TOKENS 1024
typedef struct { int start, end, next, count; char type; } RjToken;
typedef struct { const char *text; RjToken t[RJ_TOKENS]; int count, pos, length; } RjJson;
int rj_parse(RjJson *j, const char *text);
int rj_get(const RjJson *j, int object, const char *key);
int rj_string(const RjJson *j, int token, char *out, size_t cap);
int rj_eq(const RjJson *j, int token, const char *value);
int rj_raw(const RjJson *j, int token, char *out, size_t cap);
double rj_number(const RjJson *j, int token);
int rj_ipv4(const char *s, unsigned long *value);
int rj_validate(const RjJson *j, int value, const char *schema);
int rj_quote(char *out, size_t cap, const char *s);
void rj_zero(void *p, size_t n);
#endif
