#include "rj_json.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void rj_zero(void *p, size_t n) {
    volatile unsigned char *q = p;
    while (n--)
        *q++ = 0;
}
static int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static void space(RjJson *j) {
    while (j->pos < j->length && strchr(" \r\n\t", j->text[j->pos]))
        j->pos++;
}
static int utf8(const unsigned char *s) {
    while (*s) {
        unsigned c = *s++, n = 0, low = 0;
        if (c < 0x80)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            c &= 31;
            n = 1;
            low = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            c &= 15;
            n = 2;
            low = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            c &= 7;
            n = 3;
            low = 0x10000;
        } else
            return 0;
        while (n--) {
            if ((*s & 0xc0) != 0x80)
                return 0;
            c = (c << 6) | (*s++ & 63);
        }
        if (c < low || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return 0;
    }
    return 1;
}
int rj_string(const RjJson *j, int k, char *out, size_t cap) {
    if (k < 0 || k >= j->count || j->t[k].type != 's' || !cap)
        return -1;
    size_t n = 0;
    int p = j->t[k].start;
    while (p < j->t[k].end) {
        unsigned c = (unsigned char)j->text[p++];
        if (c == '\\') {
            c = (unsigned char)j->text[p++];
            if (c == 'u') {
                c = 0;
                for (int i = 0; i < 4; i++)
                    c = (c << 4) | (unsigned)hex(j->text[p++]);
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (p + 6 > j->t[k].end || j->text[p] != '\\' || j->text[p + 1] != 'u')
                        return -1;
                    p += 2;
                    unsigned lo = 0;
                    for (int i = 0; i < 4; i++)
                        lo = (lo << 4) | (unsigned)hex(j->text[p++]);
                    if (lo < 0xdc00 || lo > 0xdfff)
                        return -1;
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                } else if (c >= 0xdc00 && c <= 0xdfff)
                    return -1;
                if (c == 0)
                    return -1;
                unsigned char b[4];
                size_t len;
                if (c < 0x80) {
                    b[0] = c;
                    len = 1;
                } else if (c < 0x800) {
                    b[0] = 0xc0 | (c >> 6);
                    b[1] = 0x80 | (c & 63);
                    len = 2;
                } else if (c < 0x10000) {
                    b[0] = 0xe0 | (c >> 12);
                    b[1] = 0x80 | ((c >> 6) & 63);
                    b[2] = 0x80 | (c & 63);
                    len = 3;
                } else {
                    b[0] = 0xf0 | (c >> 18);
                    b[1] = 0x80 | ((c >> 12) & 63);
                    b[2] = 0x80 | ((c >> 6) & 63);
                    b[3] = 0x80 | (c & 63);
                    len = 4;
                }
                if (n + len >= cap)
                    return -1;
                memcpy(out + n, b, len);
                n += len;
                continue;
            }
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'b')
                c = '\b';
            else if (c == 'f')
                c = '\f';
        }
        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return (int)n;
}
static int parse_value(RjJson *j, int depth) {
    if (depth > 12 || j->count >= RJ_TOKENS)
        return -1;
    space(j);
    if (j->pos >= j->length)
        return -1;
    int k = j->count++;
    RjToken *t = &j->t[k];
    memset(t, 0, sizeof(*t));
    t->start = j->pos;
    char c = j->text[j->pos++];
    if (c == '{' || c == '[') {
        t->type = c;
        space(j);
        char close = c == '{' ? '}' : ']';
        if (j->text[j->pos] != close)
            for (;;) {
                if (c == '{') {
                    if (j->text[j->pos] != '"')
                        return -1;
                    int key = parse_value(j, depth + 1);
                    if (key < 0)
                        return -1;
                    char a[128], b[128];
                    if (rj_string(j, key, a, sizeof(a)) < 0)
                        return -1;
                    for (int x = k + 1; x < key; x = j->t[j->t[x].next].next) {
                        if (rj_string(j, x, b, sizeof(b)) < 0 || !strcmp(a, b))
                            return -1;
                    }
                    space(j);
                    if (j->text[j->pos++] != ':')
                        return -1;
                }
                if (parse_value(j, depth + 1) < 0)
                    return -1;
                t->count++;
                space(j);
                if (j->text[j->pos] == close)
                    break;
                if (j->text[j->pos++] != ',')
                    return -1;
                space(j);
            }
        if (j->text[j->pos++] != close)
            return -1;
    } else if (c == '"') {
        t->type = 's';
        t->start = j->pos;
        while (j->pos < j->length && j->text[j->pos] != '"') {
            unsigned char a = j->text[j->pos++];
            if (a < 32)
                return -1;
            if (a == '\\') {
                if (j->pos >= j->length)
                    return -1;
                char e = j->text[j->pos++];
                if (e == 'u') {
                    for (int i = 0; i < 4; i++)
                        if (j->pos >= j->length || hex(j->text[j->pos++]) < 0)
                            return -1;
                } else if (!strchr("\"\\/bfnrt", e))
                    return -1;
            }
        }
        if (j->pos >= j->length)
            return -1;
        t->end = j->pos++;
        t->next = j->count;
        /* Decode validation also rejects unpaired surrogates and embedded NUL. */
        static char decoded[16385];
        if (rj_string(j, k, decoded, sizeof(decoded)) < 0)
            return -1;
        rj_zero(decoded, sizeof(decoded));
        return k;
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *v = c == 't' ? "true" : c == 'f' ? "false" : "null";
        int n = (int)strlen(v);
        if (strncmp(j->text + t->start, v, n))
            return -1;
        j->pos = t->start + n;
        t->type = c;
    } else {
        t->type = 'd';
        int p = t->start;
        if (j->text[p] == '-')
            p++;
        if (j->text[p] == '0')
            p++;
        else {
            if (j->text[p] < '1' || j->text[p] > '9')
                return -1;
            while (j->text[p] >= '0' && j->text[p] <= '9')
                p++;
        }
        if (j->text[p] == '.') {
            p++;
            if (j->text[p] < '0' || j->text[p] > '9')
                return -1;
            while (j->text[p] >= '0' && j->text[p] <= '9')
                p++;
        }
        if (j->text[p] == 'e' || j->text[p] == 'E') {
            p++;
            if (j->text[p] == '+' || j->text[p] == '-')
                p++;
            if (j->text[p] < '0' || j->text[p] > '9')
                return -1;
            while (j->text[p] >= '0' && j->text[p] <= '9')
                p++;
        }
        j->pos = p;
    }
    t->end = j->pos;
    t->next = j->count;
    if (t->type == 'd' && (!isfinite(rj_number(j, k)) || t->end - t->start > 128))
        return -1;
    return k;
}
int rj_parse(RjJson *j, const char *text) {
    memset(j, 0, sizeof(*j));
    j->text = text;
    j->length = (int)strlen(text);
    if (!utf8((const unsigned char *)text))
        return -1;
    if (parse_value(j, 0) != 0)
        return -1;
    space(j);
    return j->pos == j->length ? 0 : -1;
}
int rj_eq(const RjJson *j, int k, const char *s) {
    char tmp[256];
    return rj_string(j, k, tmp, sizeof(tmp)) >= 0 && !strcmp(tmp, s);
}
int rj_get(const RjJson *j, int k, const char *s) {
    if (k < 0 || j->t[k].type != '{')
        return -1;
    for (int x = k + 1; x < j->t[k].next; x = j->t[j->t[x].next].next)
        if (rj_eq(j, x, s))
            return j->t[x].next;
    return -1;
}
int rj_raw(const RjJson *j, int k, char *out, size_t cap) {
    if (k < 0)
        return -1;
    int start = j->t[k].start, end = j->t[k].end;
    if (j->t[k].type == 's') {
        start--;
        end++;
    }
    int n = end - start;
    if (n < 0 || (size_t)n >= cap)
        return -1;
    memcpy(out, j->text + start, n);
    out[n] = 0;
    return n;
}
double rj_number(const RjJson *j, int k) {
    if (k < 0 || j->t[k].type != 'd')
        return -1;
    const char *p = j->text + j->t[k].start, *end = j->text + j->t[k].end;
    int sign = 1, fraction = 0, exp = 0, es = 1;
    double n = 0;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (p < end && *p != 'e' && *p != 'E') {
        if (*p == '.') {
            fraction = 1;
            p++;
            continue;
        }
        n = n * 10 + (*p++ - '0');
        if (fraction)
            exp--;
    }
    if (p < end) {
        p++;
        if (*p == '-' || *p == '+') {
            if (*p == '-')
                es = -1;
            p++;
        }
        int x = 0;
        while (p < end) {
            if (x > 1000)
                return n == 0 ? 0 : INFINITY;
            x = x * 10 + (*p++ - '0');
        }
        exp += es * x;
    }
    if (n == 0)
        return 0;
    if (exp > 400 || !isfinite(n))
        return INFINITY;
    if (exp < -400)
        return 0;
    while (exp > 0) {
        n *= 10;
        exp--;
    }
    while (exp < 0) {
        n /= 10;
        exp++;
    }
    return sign * n;
}
int rj_ipv4(const char *s, unsigned long *value) {
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned a = 0, n = 0;
        const char *begin = s;
        while (*s >= '0' && *s <= '9') {
            a = a * 10 + (*s++ - '0');
            if (++n > 3 || a > 255)
                return 0;
        }
        if (!n || (n > 1 && *begin == '0'))
            return 0;
        v = (v << 8) | a;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    if (value)
        *value = v;
    return 1;
}
static int validate(const RjJson *j, int k, const RjJson *s, int sk, int depth) {
    if (k < 0 || depth > 12)
        return 0;
    int u = rj_get(s, sk, "anyOf");
    if (u < 0)
        u = rj_get(s, sk, "oneOf");
    if (u >= 0) {
        for (int a = u + 1; a < s->t[u].next; a = s->t[a].next)
            if (validate(j, k, s, a, depth + 1))
                return 1;
        return 0;
    }
    int type = rj_get(s, sk, "type");
    char t = j->t[k].type;
    if (type >= 0 &&
        !((rj_eq(s, type, "object") && t == '{') || (rj_eq(s, type, "array") && t == '[') ||
          (rj_eq(s, type, "string") && t == 's') ||
          (rj_eq(s, type, "boolean") && (t == 't' || t == 'f')) ||
          (rj_eq(s, type, "null") && t == 'n') ||
          ((rj_eq(s, type, "number") || rj_eq(s, type, "integer")) && t == 'd')))
        return 0;
    if (t == 'd') {
        double n = rj_number(j, k);
        if (rj_eq(s, type, "integer") && floor(n) != n)
            return 0;
        int lo = rj_get(s, sk, "minimum"), hi = rj_get(s, sk, "maximum");
        if ((lo >= 0 && n < rj_number(s, lo)) || (hi >= 0 && n > rj_number(s, hi)))
            return 0;
    }
    if (t == 's') {
        char buf[4097];
        int len = rj_string(j, k, buf, sizeof(buf));
        if (len < 0)
            return 0;
        int lo = rj_get(s, sk, "minLength"), hi = rj_get(s, sk, "maxLength");
        if ((lo >= 0 && len < rj_number(s, lo)) || (hi >= 0 && len > rj_number(s, hi)))
            return 0;
        int f = rj_get(s, sk, "format");
        if (rj_eq(s, f, "ipv4") && !rj_ipv4(buf, NULL))
            return 0;
    }
    int e = rj_get(s, sk, "enum"), c = rj_get(s, sk, "const");
    if (e >= 0 || c >= 0) {
        int match = 0, start = e >= 0 ? e + 1 : c, end = e >= 0 ? s->t[e].next : c + 1;
        for (int a = start; a < end; a = s->t[a].next) {
            if (t == 's') {
                char b[256];
                if (rj_string(s, a, b, sizeof(b)) >= 0 && rj_eq(j, k, b))
                    match = 1;
            } else if (t == s->t[a].type && (t != 'd' || rj_number(j, k) == rj_number(s, a)))
                match = 1;
        }
        if (!match)
            return 0;
    }
    if (t == '{') {
        int props = rj_get(s, sk, "properties"), req = rj_get(s, sk, "required"),
            extra = rj_get(s, sk, "additionalProperties");
        char key[128];
        if (req >= 0)
            for (int a = req + 1; a < s->t[req].next; a = s->t[a].next) {
                if (rj_string(s, a, key, sizeof(key)) < 0 || rj_get(j, k, key) < 0)
                    return 0;
            }
        for (int a = k + 1; a < j->t[k].next; a = j->t[j->t[a].next].next) {
            if (rj_string(j, a, key, sizeof(key)) < 0)
                return 0;
            int child = rj_get(s, props, key);
            if (child < 0) {
                if (extra >= 0 && s->t[extra].type == 'f')
                    return 0;
            } else if (!validate(j, j->t[a].next, s, child, depth + 1))
                return 0;
        }
    }
    if (t == '[') {
        int lo = rj_get(s, sk, "minItems"), hi = rj_get(s, sk, "maxItems"),
            it = rj_get(s, sk, "items");
        if ((lo >= 0 && j->t[k].count < rj_number(s, lo)) ||
            (hi >= 0 && j->t[k].count > rj_number(s, hi)))
            return 0;
        if (it >= 0)
            for (int a = k + 1; a < j->t[k].next; a = j->t[a].next)
                if (!validate(j, a, s, it, depth + 1))
                    return 0;
    }
    return 1;
}
int rj_validate(const RjJson *j, int k, const char *schema) {
    static RjJson s;
    if (rj_parse(&s, schema))
        return 0;
    return validate(j, k, &s, 0, 0);
}
int rj_quote(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3)
        return -1;
    out[n++] = '"';
    for (; *s; s++) {
        unsigned char c = *s;
        if (c < 32) {
            if (n + 6 >= cap)
                return -1;
            snprintf(out + n, cap - n, "\\u%04x", c);
            n += 6;
        } else {
            if (n + 2 >= cap)
                return -1;
            if (c == '"' || c == '\\')
                out[n++] = '\\';
            out[n++] = c;
        }
    }
    if (n + 2 > cap)
        return -1;
    out[n++] = '"';
    out[n] = 0;
    return (int)n;
}
