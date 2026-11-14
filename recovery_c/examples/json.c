/* json.c —— 递归下降的 JSON 读取器，见 json.h。 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;      /* 文本 */
    size_t      n;      /* 长度 */
    size_t      i;      /* 当前位置 */
    const char *err;    /* 第一个错误的说明，NULL 表示还没出错 */
    size_t      err_at;
} parser;

static json_value *parse_value(parser *p, int depth);

static void fail(parser *p, const char *msg)
{
    if (!p->err) {
        p->err = msg;
        p->err_at = p->i;
    }
}

static void skip_ws(parser *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->i++;
    }
}

static json_value *new_value(parser *p, int type)
{
    json_value *v = (json_value *)calloc(1, sizeof *v);
    if (!v)
        fail(p, "内存不足");
    else
        v->type = type;
    return v;
}

/* 匹配字面量 true / false / null */
static int eat(parser *p, const char *lit)
{
    size_t k = strlen(lit);
    if (p->n - p->i >= k && memcmp(p->s + p->i, lit, k) == 0) {
        p->i += k;
        return 1;
    }
    return 0;
}

static int hex4(parser *p, unsigned *out)
{
    unsigned v = 0;
    int k;

    if (p->n - p->i < 4)
        return 0;
    for (k = 0; k < 4; k++) {
        char c = p->s[p->i + k];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    p->i += 4;
    *out = v;
    return 1;
}

/* 码点写成 UTF-8，返回字节数 */
static size_t put_utf8(char *dst, unsigned cp)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * 解析字符串，p->i 指向开头的引号。返回 malloc 的以 0 结尾的串。
 * 展开后的长度不会超过原文长度，所以按原文长度一次分配。
 */
static char *parse_string_raw(parser *p)
{
    size_t start = ++p->i, end = start, o = 0;
    char *out;

    while (end < p->n && p->s[end] != '"') {
        if (p->s[end] == '\\')
            end++;
        end++;
    }
    if (end >= p->n) {
        fail(p, "字符串没有结束");
        return NULL;
    }
    out = (char *)malloc(end - start + 1);
    if (!out) {
        fail(p, "内存不足");
        return NULL;
    }
    while (p->i < end) {
        char c = p->s[p->i++];
        unsigned cp, lo;

        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        c = p->s[p->i++];
        switch (c) {
        case '"':  out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case '/':  out[o++] = '/';  break;
        case 'b':  out[o++] = '\b'; break;
        case 'f':  out[o++] = '\f'; break;
        case 'n':  out[o++] = '\n'; break;
        case 'r':  out[o++] = '\r'; break;
        case 't':  out[o++] = '\t'; break;
        case 'u':
            if (!hex4(p, &cp))
                goto bad;
            /* 高代理后面必须跟一个低代理 */
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (p->n - p->i < 6 || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u')
                    goto bad;
                p->i += 2;
                if (!hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                    goto bad;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            o += put_utf8(out + o, cp);
            break;
        default:
            goto bad;
        }
    }
    out[o] = '\0';
    p->i = end + 1;
    return out;

bad:
    fail(p, "非法的转义序列");
    free(out);
    return NULL;
}

static json_value *parse_number(parser *p)
{
    char buf[64];
    size_t k = 0;
    char *endp;
    json_value *v;

    while (p->i + k < p->n && k < sizeof buf - 1) {
        char c = p->s[p->i + k];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'))
            break;
        buf[k++] = c;
    }
    buf[k] = '\0';
    v = new_value(p, JSON_NUMBER);
    if (!v)
        return NULL;
    v->number = strtod(buf, &endp);
    if (k == 0 || *endp != '\0') {
        fail(p, "非法的数字");
        free(v);
        return NULL;
    }
    p->i += k;
    return v;
}

/* 往数组或对象里追加一个成员，容量按 2 倍增长。失败时调用方负责释放 item 和 key。 */
static int push(parser *p, json_value *v, size_t *cap, char *key, json_value *item)
{
    if (v->count == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        json_value **ni = (json_value **)realloc(v->items, nc * sizeof *ni);
        if (!ni) {
            fail(p, "内存不足");
            return 0;
        }
        v->items = ni;
        if (v->type == JSON_OBJECT) {
            char **nk = (char **)realloc(v->keys, nc * sizeof *nk);
            if (!nk) {
                fail(p, "内存不足");
                return 0;
            }
            v->keys = nk;
        }
        *cap = nc;
    }
    if (v->type == JSON_OBJECT)
        v->keys[v->count] = key;
    v->items[v->count++] = item;
    return 1;
}

static json_value *parse_container(parser *p, int depth, int type)
{
    char close = type == JSON_ARRAY ? ']' : '}';
    json_value *v = new_value(p, type);
    size_t cap = 0;

    if (!v)
        return NULL;
    p->i++;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        return v;
    }
    for (;;) {
        char *key = NULL;
        json_value *item;

        skip_ws(p);
        if (type == JSON_OBJECT) {
            if (p->i >= p->n || p->s[p->i] != '"') {
                fail(p, "对象的键必须是字符串");
                break;
            }
            key = parse_string_raw(p);
            if (!key)
                break;
            skip_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') {
                fail(p, "键后面缺少冒号");
                free(key);
                break;
            }
            p->i++;
        }
        item = parse_value(p, depth + 1);
        if (!item) {
            free(key);
            break;
        }
        if (!push(p, v, &cap, key, item)) {
            free(key);
            json_free(item);
            break;
        }
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == close) {
            p->i++;
            return v;
        }
        fail(p, "缺少逗号或右括号");
        break;
    }
    json_free(v);
    return NULL;
}

static json_value *parse_value(parser *p, int depth)
{
    json_value *v;
    char c;

    skip_ws(p);
    if (p->i >= p->n) {
        fail(p, "文本提前结束");
        return NULL;
    }
    if (depth > JSON_MAX_DEPTH) {
        fail(p, "嵌套太深");
        return NULL;
    }
    c = p->s[p->i];
    if (c == '{')
        return parse_container(p, depth, JSON_OBJECT);
    if (c == '[')
        return parse_container(p, depth, JSON_ARRAY);
    if (c == '"') {
        char *s = parse_string_raw(p);
        if (!s)
            return NULL;
        v = new_value(p, JSON_STRING);
        if (!v) {
            free(s);
            return NULL;
        }
        v->string = s;
        return v;
    }
    if (eat(p, "true") || eat(p, "false")) {
        v = new_value(p, JSON_BOOL);
        if (v)
            v->boolean = c == 't';
        return v;
    }
    if (eat(p, "null"))
        return new_value(p, JSON_NULL);
    return parse_number(p);
}

json_value *json_parse(const char *text, size_t len, char *err, size_t err_cap)
{
    parser p;
    json_value *v;

    p.s = text;
    p.n = len;
    p.i = 0;
    p.err = NULL;
    p.err_at = 0;

    v = parse_value(&p, 0);
    if (v) {
        skip_ws(&p);
        if (p.i != p.n) {
            fail(&p, "文档结束后还有多余内容");
            json_free(v);
            v = NULL;
        }
    }
    if (!v && err && err_cap)
        snprintf(err, err_cap, "%s（偏移 %lu）", p.err ? p.err : "解析失败",
                 (unsigned long)p.err_at);
    return v;
}

json_value *json_parse_file(const char *path, char *err, size_t err_cap)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t len = 0, cap = 0;
    json_value *v;

    if (!f) {
        if (err && err_cap)
            snprintf(err, err_cap, "打不开文件 %s", path);
        return NULL;
    }
    for (;;) {
        size_t got;
        if (len == cap) {
            size_t nc = cap ? cap * 2 : 65536;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                fclose(f);
                if (err && err_cap)
                    snprintf(err, err_cap, "内存不足");
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        got = fread(buf + len, 1, cap - len, f);
        if (got == 0)
            break;
        len += got;
    }
    fclose(f);
    v = json_parse(buf ? buf : "", len, err, err_cap);
    free(buf);
    return v;
}

void json_free(json_value *v)
{
    size_t i;

    if (!v)
        return;
    for (i = 0; i < v->count; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->string);
    free(v);
}

const json_value *json_get(const json_value *v, const char *key)
{
    size_t i;

    if (!v || v->type != JSON_OBJECT)
        return NULL;
    for (i = 0; i < v->count; i++)
        if (strcmp(v->keys[i], key) == 0)
            return v->items[i];
    return NULL;
}

const json_value *json_at(const json_value *v, size_t i)
{
    if (!v || v->type != JSON_ARRAY || i >= v->count)
        return NULL;
    return v->items[i];
}

size_t json_count(const json_value *v)
{
    if (!v || (v->type != JSON_ARRAY && v->type != JSON_OBJECT))
        return 0;
    return v->count;
}

int json_is_null(const json_value *v)
{
    return !v || v->type == JSON_NULL;
}

double json_number(const json_value *v, double fallback)
{
    return v && v->type == JSON_NUMBER ? v->number : fallback;
}

const char *json_string(const json_value *v)
{
    return v && v->type == JSON_STRING ? v->string : NULL;
}

int json_bool(const json_value *v, int fallback)
{
    return v && v->type == JSON_BOOL ? v->boolean : fallback;
}
