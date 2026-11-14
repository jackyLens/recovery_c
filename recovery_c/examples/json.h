/*
 * json.h —— 示例和测试用的小型 JSON 读取器，不属于核心库。
 *
 * 一次把整个文档解析成树，只读。数字一律存成 double，字符串是 UTF-8
 * （\uXXXX 转义会展开，含代理对）。不处理超过 JSON_MAX_DEPTH 层的嵌套。
 */
#ifndef RC_EXAMPLE_JSON_H
#define RC_EXAMPLE_JSON_H

#include <stddef.h>

#define JSON_MAX_DEPTH 64

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;

struct json_value {
    int          type;     /* json_type */
    int          boolean;  /* JSON_BOOL：0 / 1 */
    double       number;   /* JSON_NUMBER */
    char        *string;   /* JSON_STRING：以 0 结尾 */
    size_t       count;    /* JSON_ARRAY / JSON_OBJECT：元素个数 */
    json_value **items;    /* 元素，对象按出现顺序存放 */
    char       **keys;     /* JSON_OBJECT：与 items 一一对应的键 */
};

/*
 * 解析一段 JSON 文本。
 *   text     [入] 文本，不要求以 0 结尾
 *   len      [入] 字节数
 *   err      [出] 可选。失败时写入错误说明（含字节偏移）
 *   err_cap  [入] err 的容量
 * 成功返回根节点，用 json_free 释放；失败返回 NULL。
 */
json_value *json_parse(const char *text, size_t len, char *err, size_t err_cap);

/* 读入整个文件再解析。文件打不开也返回 NULL 并写 err。 */
json_value *json_parse_file(const char *path, char *err, size_t err_cap);

/* 释放整棵树。传 NULL 无操作。 */
void json_free(json_value *v);

/* 按键取对象成员。v 不是对象或没有这个键返回 NULL。键重复时取第一个。 */
const json_value *json_get(const json_value *v, const char *key);

/* 取数组第 i 个元素。v 不是数组或越界返回 NULL。 */
const json_value *json_at(const json_value *v, size_t i);

/* 数组或对象的元素个数，其他类型返回 0。 */
size_t json_count(const json_value *v);

/* v 为 NULL 或 JSON null 时返回 1。 */
int json_is_null(const json_value *v);

/* 取数字；v 不是数字时返回 fallback。 */
double json_number(const json_value *v, double fallback);

/* 取字符串；v 不是字符串时返回 NULL。 */
const char *json_string(const json_value *v);

/* 取布尔；v 不是布尔时返回 fallback。 */
int json_bool(const json_value *v, int fallback);

#endif
