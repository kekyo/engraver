#define _POSIX_C_SOURCE 200809L

#include "engraver.h"
#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EG_RT_ICON 3u
#define EG_RT_STRING 6u
#define EG_RT_GROUP_ICON 14u
#define EG_RT_VERSION 16u
#define EG_DEFAULT_CODE_PAGE 1200u
#define EG_JSON_MAGIC 0x45474a53u

typedef struct eg_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} eg_buffer;

typedef struct eg_internal_id {
    int is_string;
    uint32_t id;
    uint16_t *name;
    size_t name_length;
} eg_internal_id;

typedef struct eg_resource_item {
    eg_internal_id type;
    eg_internal_id name;
    eg_internal_id language;
    uint32_t code_page;
    unsigned char *data;
    size_t size;
} eg_resource_item;

typedef struct eg_resource_list {
    eg_resource_item *items;
    size_t count;
    size_t capacity;
} eg_resource_list;

typedef enum eg_update_kind {
    EG_UPDATE_DATA,
    EG_UPDATE_STRING,
    EG_UPDATE_VERSION_STRING,
    EG_UPDATE_VERSION_FIXED,
    EG_UPDATE_ICON
} eg_update_kind;

typedef struct eg_update_op {
    eg_update_kind kind;
    eg_resource_key key;
    unsigned char *data;
    size_t size;
    uint32_t id;
    uint16_t language;
    uint16_t code_page;
    char *text_a;
    char *text_b;
    eg_version_fixed_update fixed;
} eg_update_op;

struct eg_resource_update {
    eg_update_op *ops;
    size_t count;
    size_t capacity;
};

typedef struct eg_section {
    char name[9];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t characteristics;
    size_t header_offset;
} eg_section;

typedef struct eg_pe_info {
    size_t nt_offset;
    size_t file_header_offset;
    size_t optional_offset;
    size_t section_table_offset;
    size_t number_sections_field;
    size_t size_image_field;
    size_t resource_rva_field;
    size_t resource_size_field;
    uint16_t section_count;
    uint16_t optional_size;
    int is_pe64;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint32_t size_of_headers;
    uint32_t size_of_image;
    uint32_t resource_rva;
    uint32_t resource_size;
    eg_section *sections;
} eg_pe_info;

struct eg_pe_file {
    char *path;
    unsigned char *data;
    size_t size;
    eg_pe_info info;
};

typedef struct eg_owned_json_document {
    eg_json_document document;
    uint32_t magic;
    eg_json_string_update *strings;
    eg_json_icon_update *icons;
    eg_json_raw_update *raw;
    eg_json_version_update version;
    eg_json_version_string_update *version_strings;
    int has_version;
} eg_owned_json_document;

typedef struct eg_string16 {
    uint16_t *data;
    size_t length;
} eg_string16;

typedef struct eg_version_string {
    char *key;
    char *value;
} eg_version_string;

typedef struct eg_version_table {
    uint16_t language;
    uint16_t code_page;
    eg_version_string *strings;
    size_t string_count;
    size_t string_capacity;
} eg_version_table;

typedef struct eg_version_model {
    uint32_t fixed[13];
    eg_version_table *tables;
    size_t table_count;
    size_t table_capacity;
} eg_version_model;

typedef struct eg_icon_image {
    unsigned char directory[16];
    const unsigned char *data;
    size_t size;
} eg_icon_image;

typedef struct eg_icon_file {
    eg_icon_image *images;
    size_t count;
} eg_icon_file;

typedef struct eg_res_node {
    eg_internal_id id;
    struct eg_res_node *children;
    size_t child_count;
    size_t child_capacity;
    eg_resource_item *item;
    uint32_t dir_offset;
    uint32_t name_offset;
    uint32_t data_entry_offset;
    uint32_t data_offset;
} eg_res_node;

static uint16_t eg_read_u16(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 2u > size) return 0u;
    return (uint16_t)data[offset] | (uint16_t)((uint16_t)data[offset + 1u] << 8u);
}

static uint32_t eg_read_u32(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 4u > size) return 0u;
    return (uint32_t)data[offset] |
        ((uint32_t)data[offset + 1u] << 8u) |
        ((uint32_t)data[offset + 2u] << 16u) |
        ((uint32_t)data[offset + 3u] << 24u);
}

static void eg_write_u16(unsigned char *data, size_t offset, uint16_t value) {
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1u] = (unsigned char)((value >> 8u) & 0xffu);
}

static void eg_write_u32(unsigned char *data, size_t offset, uint32_t value) {
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1u] = (unsigned char)((value >> 8u) & 0xffu);
    data[offset + 2u] = (unsigned char)((value >> 16u) & 0xffu);
    data[offset + 3u] = (unsigned char)((value >> 24u) & 0xffu);
}

static uint32_t eg_align_u32(uint32_t value, uint32_t alignment) {
    if (alignment == 0u) return value;
    return (uint32_t)(((uint64_t)value + alignment - 1u) / alignment * alignment);
}

static size_t eg_align_size(size_t value, size_t alignment) {
    if (alignment == 0u) return value;
    return (value + alignment - 1u) / alignment * alignment;
}

static void *eg_alloc(size_t size) {
    void *ptr = malloc(size == 0u ? 1u : size);
    return ptr;
}

static void *eg_calloc(size_t count, size_t size) {
    if (size != 0u && count > SIZE_MAX / size) return NULL;
    return calloc(count == 0u ? 1u : count, size == 0u ? 1u : size);
}

static void *eg_realloc_array(void *ptr, size_t count, size_t size) {
    if (size != 0u && count > SIZE_MAX / size) return NULL;
    return realloc(ptr, count == 0u ? 1u : count * size);
}

static char *eg_strdup_len(const char *text, size_t length) {
    char *copy = (char *)eg_alloc(length + 1u);
    if (copy == NULL) return NULL;
    if (length != 0u) memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *eg_strdup_text(const char *text) {
    if (text == NULL) return NULL;
    return eg_strdup_len(text, strlen(text));
}

static eg_result eg_buffer_reserve(eg_buffer *buffer, size_t needed) {
    unsigned char *next;
    size_t next_capacity;
    if (needed <= buffer->capacity) return EG_OK;
    next_capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2u) return EG_ERROR_OUT_OF_MEMORY;
        next_capacity *= 2u;
    }
    next = (unsigned char *)realloc(buffer->data, next_capacity);
    if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
    buffer->data = next;
    buffer->capacity = next_capacity;
    return EG_OK;
}

static eg_result eg_buffer_resize(eg_buffer *buffer, size_t size) {
    eg_result result = eg_buffer_reserve(buffer, size);
    if (result != EG_OK) return result;
    if (size > buffer->size) memset(buffer->data + buffer->size, 0, size - buffer->size);
    buffer->size = size;
    return EG_OK;
}

static eg_result eg_buffer_append(eg_buffer *buffer, const void *data, size_t size) {
    eg_result result;
    if (size == 0u) return EG_OK;
    result = eg_buffer_reserve(buffer, buffer->size + size);
    if (result != EG_OK) return result;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return EG_OK;
}

static eg_result eg_buffer_append_u16(eg_buffer *buffer, uint16_t value) {
    unsigned char bytes[2];
    eg_write_u16(bytes, 0u, value);
    return eg_buffer_append(buffer, bytes, sizeof(bytes));
}

static eg_result eg_buffer_append_u32(eg_buffer *buffer, uint32_t value) {
    unsigned char bytes[4];
    eg_write_u32(bytes, 0u, value);
    return eg_buffer_append(buffer, bytes, sizeof(bytes));
}

static eg_result eg_buffer_align(eg_buffer *buffer, size_t alignment) {
    return eg_buffer_resize(buffer, eg_align_size(buffer->size, alignment));
}

static void eg_buffer_free(eg_buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

static eg_internal_id eg_make_id(uint32_t id) {
    eg_internal_id value;
    value.is_string = 0;
    value.id = id;
    value.name = NULL;
    value.name_length = 0u;
    return value;
}

static void eg_id_free(eg_internal_id *id) {
    if (id == NULL) return;
    free(id->name);
    id->name = NULL;
    id->name_length = 0u;
    id->id = 0u;
    id->is_string = 0;
}

static eg_result eg_id_copy(eg_internal_id *dst, const eg_internal_id *src) {
    memset(dst, 0, sizeof(*dst));
    dst->is_string = src->is_string;
    dst->id = src->id;
    dst->name_length = src->name_length;
    if (src->is_string) {
        if (src->name_length != 0u) {
            dst->name = (uint16_t *)eg_alloc(sizeof(uint16_t) * src->name_length);
            if (dst->name == NULL) return EG_ERROR_OUT_OF_MEMORY;
            memcpy(dst->name, src->name, sizeof(uint16_t) * src->name_length);
        }
    }
    return EG_OK;
}

static eg_result eg_id_from_public(eg_internal_id *dst, const eg_resource_id *src) {
    memset(dst, 0, sizeof(*dst));
    dst->is_string = src->is_string;
    dst->id = src->id;
    dst->name_length = src->name_length;
    if (src->is_string) {
        if (src->name_utf16 == NULL && src->name_length != 0u) return EG_ERROR_INVALID_ARGUMENT;
        if (src->name_length != 0u) {
            dst->name = (uint16_t *)eg_alloc(sizeof(uint16_t) * src->name_length);
            if (dst->name == NULL) return EG_ERROR_OUT_OF_MEMORY;
            memcpy(dst->name, src->name_utf16, sizeof(uint16_t) * src->name_length);
        }
    }
    return EG_OK;
}

static int eg_id_equal(const eg_internal_id *a, const eg_internal_id *b) {
    if (a->is_string != b->is_string) return 0;
    if (!a->is_string) return a->id == b->id;
    if (a->name_length != b->name_length) return 0;
    if (a->name_length == 0u) return 1;
    return memcmp(a->name, b->name, sizeof(uint16_t) * a->name_length) == 0;
}

static int eg_id_compare(const eg_internal_id *a, const eg_internal_id *b) {
    size_t i;
    if (a->is_string != b->is_string) return a->is_string ? -1 : 1;
    if (!a->is_string) {
        if (a->id < b->id) return -1;
        if (a->id > b->id) return 1;
        return 0;
    }
    for (i = 0u; i < a->name_length && i < b->name_length; i++) {
        if (a->name[i] < b->name[i]) return -1;
        if (a->name[i] > b->name[i]) return 1;
    }
    if (a->name_length < b->name_length) return -1;
    if (a->name_length > b->name_length) return 1;
    return 0;
}

static eg_result eg_public_key_copy(eg_resource_key *dst, const eg_resource_key *src) {
    eg_internal_id type;
    eg_internal_id name;
    eg_result result;
    memset(dst, 0, sizeof(*dst));
    result = eg_id_from_public(&type, &src->type);
    if (result != EG_OK) return result;
    result = eg_id_from_public(&name, &src->name);
    if (result != EG_OK) {
        eg_id_free(&type);
        return result;
    }
    dst->type.is_string = type.is_string;
    dst->type.id = type.id;
    dst->type.name_utf16 = type.name;
    dst->type.name_length = type.name_length;
    dst->name.is_string = name.is_string;
    dst->name.id = name.id;
    dst->name.name_utf16 = name.name;
    dst->name.name_length = name.name_length;
    dst->language = src->language;
    return EG_OK;
}

static void eg_public_key_free(eg_resource_key *key) {
    if (key == NULL) return;
    free((void *)key->type.name_utf16);
    free((void *)key->name.name_utf16);
    memset(key, 0, sizeof(*key));
}

const char *eg_result_string(eg_result result) {
    switch (result) {
    case EG_OK: return "ok";
    case EG_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case EG_ERROR_IO: return "i/o error";
    case EG_ERROR_INVALID_JSON: return "invalid json";
    case EG_ERROR_INVALID_PE: return "invalid pe";
    case EG_ERROR_UNSUPPORTED_PE: return "unsupported pe";
    case EG_ERROR_INVALID_RESOURCE: return "invalid resource";
    case EG_ERROR_DUPLICATE_RESOURCE: return "duplicate resource";
    case EG_ERROR_OUT_OF_MEMORY: return "out of memory";
    default: return "unknown error";
    }
}

typedef struct eg_posix_handle {
    int fd;
} eg_posix_handle;

static eg_result eg_posix_open_read(void *context, const char *path, void **out_handle) {
    eg_posix_handle *handle;
    (void)context;
    handle = (eg_posix_handle *)eg_alloc(sizeof(*handle));
    if (handle == NULL) return EG_ERROR_OUT_OF_MEMORY;
    handle->fd = open(path, O_RDONLY);
    if (handle->fd < 0) {
        free(handle);
        return EG_ERROR_IO;
    }
    *out_handle = handle;
    return EG_OK;
}

static eg_result eg_posix_open_write(void *context, const char *path, void **out_handle) {
    eg_posix_handle *handle;
    (void)context;
    handle = (eg_posix_handle *)eg_alloc(sizeof(*handle));
    if (handle == NULL) return EG_ERROR_OUT_OF_MEMORY;
    handle->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (handle->fd < 0) {
        free(handle);
        return EG_ERROR_IO;
    }
    *out_handle = handle;
    return EG_OK;
}

static eg_result eg_posix_read(void *context, void *raw_handle, void *buffer, size_t size, size_t *out_size) {
    eg_posix_handle *handle = (eg_posix_handle *)raw_handle;
    ssize_t amount;
    (void)context;
    amount = read(handle->fd, buffer, size);
    if (amount < 0) return EG_ERROR_IO;
    *out_size = (size_t)amount;
    return EG_OK;
}

static eg_result eg_posix_write(void *context, void *raw_handle, const void *buffer, size_t size) {
    eg_posix_handle *handle = (eg_posix_handle *)raw_handle;
    const unsigned char *cursor = (const unsigned char *)buffer;
    (void)context;
    while (size != 0u) {
        ssize_t amount = write(handle->fd, cursor, size);
        if (amount <= 0) return EG_ERROR_IO;
        cursor += (size_t)amount;
        size -= (size_t)amount;
    }
    return EG_OK;
}

static eg_result eg_posix_close(void *context, void *raw_handle) {
    eg_posix_handle *handle = (eg_posix_handle *)raw_handle;
    int status;
    (void)context;
    if (handle == NULL) return EG_OK;
    status = close(handle->fd);
    free(handle);
    return status == 0 ? EG_OK : EG_ERROR_IO;
}

static const eg_io_ops *eg_default_io(void) {
    static const eg_io_ops ops = {
        NULL,
        eg_posix_open_read,
        eg_posix_open_write,
        eg_posix_read,
        eg_posix_write,
        eg_posix_close
    };
    return &ops;
}

static eg_result eg_validate_read_ops(const eg_io_ops *ops) {
    if (ops == NULL || ops->open_read == NULL || ops->read == NULL || ops->close == NULL) {
        return EG_ERROR_INVALID_ARGUMENT;
    }
    return EG_OK;
}

static eg_result eg_validate_write_ops(const eg_io_ops *ops) {
    if (ops == NULL || ops->open_write == NULL || ops->write == NULL || ops->close == NULL) {
        return EG_ERROR_INVALID_ARGUMENT;
    }
    return EG_OK;
}

static eg_result eg_io_read_all(const eg_io_ops *ops, const char *path, unsigned char **out_data, size_t *out_size) {
    void *handle = NULL;
    eg_buffer buffer;
    eg_result result;
    unsigned char chunk[8192];
    result = eg_validate_read_ops(ops);
    if (result != EG_OK) return result;
    if (path == NULL || out_data == NULL || out_size == NULL) return EG_ERROR_INVALID_ARGUMENT;
    memset(&buffer, 0, sizeof(buffer));
    *out_data = NULL;
    *out_size = 0u;
    result = ops->open_read(ops->context, path, &handle);
    if (result != EG_OK) return result;
    for (;;) {
        size_t amount = 0u;
        result = ops->read(ops->context, handle, chunk, sizeof(chunk), &amount);
        if (result != EG_OK) break;
        if (amount == 0u) break;
        result = eg_buffer_append(&buffer, chunk, amount);
        if (result != EG_OK) break;
    }
    {
        eg_result close_result = ops->close(ops->context, handle);
        if (result == EG_OK) result = close_result;
    }
    if (result != EG_OK) {
        eg_buffer_free(&buffer);
        return result;
    }
    result = eg_buffer_append(&buffer, "", 1u);
    if (result != EG_OK) {
        eg_buffer_free(&buffer);
        return result;
    }
    buffer.size -= 1u;
    *out_data = buffer.data;
    *out_size = buffer.size;
    return EG_OK;
}

static eg_result eg_io_write_all(const eg_io_ops *ops, const char *path, const unsigned char *data, size_t size) {
    void *handle = NULL;
    eg_result result = eg_validate_write_ops(ops);
    if (result != EG_OK) return result;
    if (path == NULL || (data == NULL && size != 0u)) return EG_ERROR_INVALID_ARGUMENT;
    result = ops->open_write(ops->context, path, &handle);
    if (result != EG_OK) return result;
    result = ops->write(ops->context, handle, data, size);
    {
        eg_result close_result = ops->close(ops->context, handle);
        if (result == EG_OK) result = close_result;
    }
    return result;
}

static char *eg_path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return eg_strdup_text(".");
    if (slash == path) return eg_strdup_text("/");
    return eg_strdup_len(path, (size_t)(slash - path));
}

static char *eg_path_join(const char *base_dir, const char *path) {
    size_t base_len;
    size_t path_len;
    char *joined;
    if (path == NULL) return NULL;
    if (path[0] == '/' || base_dir == NULL || base_dir[0] == '\0' || strcmp(base_dir, ".") == 0) {
        return eg_strdup_text(path);
    }
    base_len = strlen(base_dir);
    path_len = strlen(path);
    joined = (char *)eg_alloc(base_len + 1u + path_len + 1u);
    if (joined == NULL) return NULL;
    memcpy(joined, base_dir, base_len);
    joined[base_len] = '/';
    memcpy(joined + base_len + 1u, path, path_len + 1u);
    return joined;
}

static eg_result eg_utf8_to_utf16(const char *text, uint16_t **out_data, size_t *out_length) {
    const unsigned char *cursor = (const unsigned char *)text;
    uint16_t *values = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    *out_data = NULL;
    *out_length = 0u;
    while (*cursor != '\0') {
        uint32_t cp;
        size_t advance;
        if (*cursor < 0x80u) {
            cp = *cursor;
            advance = 1u;
        } else if ((*cursor & 0xe0u) == 0xc0u && (cursor[1] & 0xc0u) == 0x80u) {
            cp = ((uint32_t)(*cursor & 0x1fu) << 6u) | (uint32_t)(cursor[1] & 0x3fu);
            advance = 2u;
            if (cp < 0x80u) return EG_ERROR_INVALID_JSON;
        } else if ((*cursor & 0xf0u) == 0xe0u && (cursor[1] & 0xc0u) == 0x80u && (cursor[2] & 0xc0u) == 0x80u) {
            cp = ((uint32_t)(*cursor & 0x0fu) << 12u) |
                ((uint32_t)(cursor[1] & 0x3fu) << 6u) |
                (uint32_t)(cursor[2] & 0x3fu);
            advance = 3u;
            if (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu)) return EG_ERROR_INVALID_JSON;
        } else if ((*cursor & 0xf8u) == 0xf0u &&
            (cursor[1] & 0xc0u) == 0x80u &&
            (cursor[2] & 0xc0u) == 0x80u &&
            (cursor[3] & 0xc0u) == 0x80u) {
            cp = ((uint32_t)(*cursor & 0x07u) << 18u) |
                ((uint32_t)(cursor[1] & 0x3fu) << 12u) |
                ((uint32_t)(cursor[2] & 0x3fu) << 6u) |
                (uint32_t)(cursor[3] & 0x3fu);
            advance = 4u;
            if (cp < 0x10000u || cp > 0x10ffffu) return EG_ERROR_INVALID_JSON;
        } else {
            return EG_ERROR_INVALID_JSON;
        }
        if (cp <= 0xffffu) {
            if (count == capacity) {
                size_t next_capacity = capacity == 0u ? 16u : capacity * 2u;
                uint16_t *next = (uint16_t *)eg_realloc_array(values, next_capacity, sizeof(uint16_t));
                if (next == NULL) {
                    free(values);
                    return EG_ERROR_OUT_OF_MEMORY;
                }
                values = next;
                capacity = next_capacity;
            }
            values[count++] = (uint16_t)cp;
        } else {
            uint32_t adjusted = cp - 0x10000u;
            if (count + 2u > capacity) {
                size_t next_capacity = capacity == 0u ? 16u : capacity * 2u;
                while (next_capacity < count + 2u) next_capacity *= 2u;
                {
                    uint16_t *next = (uint16_t *)eg_realloc_array(values, next_capacity, sizeof(uint16_t));
                    if (next == NULL) {
                        free(values);
                        return EG_ERROR_OUT_OF_MEMORY;
                    }
                    values = next;
                    capacity = next_capacity;
                }
            }
            values[count++] = (uint16_t)(0xd800u + (adjusted >> 10u));
            values[count++] = (uint16_t)(0xdc00u + (adjusted & 0x3ffu));
        }
        cursor += advance;
    }
    *out_data = values;
    *out_length = count;
    return EG_OK;
}

static eg_result eg_utf16_to_utf8(const uint16_t *data, size_t length, char **out_text) {
    eg_buffer buffer;
    size_t i;
    memset(&buffer, 0, sizeof(buffer));
    *out_text = NULL;
    for (i = 0u; i < length; i++) {
        uint32_t cp = data[i];
        unsigned char bytes[4];
        size_t byte_count;
        if (cp >= 0xd800u && cp <= 0xdbffu && i + 1u < length && data[i + 1u] >= 0xdc00u && data[i + 1u] <= 0xdfffu) {
            cp = 0x10000u + (((uint32_t)data[i] - 0xd800u) << 10u) + ((uint32_t)data[i + 1u] - 0xdc00u);
            i++;
        }
        if (cp < 0x80u) {
            bytes[0] = (unsigned char)cp;
            byte_count = 1u;
        } else if (cp < 0x800u) {
            bytes[0] = (unsigned char)(0xc0u | (cp >> 6u));
            bytes[1] = (unsigned char)(0x80u | (cp & 0x3fu));
            byte_count = 2u;
        } else if (cp < 0x10000u) {
            bytes[0] = (unsigned char)(0xe0u | (cp >> 12u));
            bytes[1] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3fu));
            bytes[2] = (unsigned char)(0x80u | (cp & 0x3fu));
            byte_count = 3u;
        } else {
            bytes[0] = (unsigned char)(0xf0u | (cp >> 18u));
            bytes[1] = (unsigned char)(0x80u | ((cp >> 12u) & 0x3fu));
            bytes[2] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3fu));
            bytes[3] = (unsigned char)(0x80u | (cp & 0x3fu));
            byte_count = 4u;
        }
        {
            eg_result result = eg_buffer_append(&buffer, bytes, byte_count);
            if (result != EG_OK) {
                eg_buffer_free(&buffer);
                return result;
            }
        }
    }
    {
        eg_result result = eg_buffer_append(&buffer, "", 1u);
        if (result != EG_OK) {
            eg_buffer_free(&buffer);
            return result;
        }
    }
    *out_text = (char *)buffer.data;
    return EG_OK;
}

static eg_result eg_append_utf16le(eg_buffer *buffer, const uint16_t *data, size_t length, int append_nul) {
    size_t i;
    for (i = 0u; i < length; i++) {
        eg_result result = eg_buffer_append_u16(buffer, data[i]);
        if (result != EG_OK) return result;
    }
    if (append_nul) return eg_buffer_append_u16(buffer, 0u);
    return EG_OK;
}

static eg_result eg_append_utf8_as_utf16le(eg_buffer *buffer, const char *text, int append_nul, size_t *out_units) {
    uint16_t *wide = NULL;
    size_t length = 0u;
    eg_result result = eg_utf8_to_utf16(text, &wide, &length);
    if (result != EG_OK) return result;
    result = eg_append_utf16le(buffer, wide, length, append_nul);
    free(wide);
    if (result == EG_OK && out_units != NULL) *out_units = length;
    return result;
}

static void eg_resource_item_free(eg_resource_item *item) {
    eg_id_free(&item->type);
    eg_id_free(&item->name);
    eg_id_free(&item->language);
    free(item->data);
    memset(item, 0, sizeof(*item));
}

static void eg_resource_list_free(eg_resource_list *list) {
    size_t i;
    for (i = 0u; i < list->count; i++) eg_resource_item_free(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static eg_result eg_resource_list_reserve(eg_resource_list *list, size_t needed) {
    eg_resource_item *next;
    size_t next_capacity;
    if (needed <= list->capacity) return EG_OK;
    next_capacity = list->capacity == 0u ? 16u : list->capacity;
    while (next_capacity < needed) next_capacity *= 2u;
    next = (eg_resource_item *)eg_realloc_array(list->items, next_capacity, sizeof(*list->items));
    if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
    list->items = next;
    list->capacity = next_capacity;
    return EG_OK;
}

static int eg_resource_key_equal_ids(
    const eg_resource_item *item,
    const eg_internal_id *type,
    const eg_internal_id *name,
    const eg_internal_id *language
) {
    return eg_id_equal(&item->type, type) && eg_id_equal(&item->name, name) && eg_id_equal(&item->language, language);
}

static eg_resource_item *eg_resource_list_find(
    eg_resource_list *list,
    const eg_internal_id *type,
    const eg_internal_id *name,
    const eg_internal_id *language
) {
    size_t i;
    for (i = 0u; i < list->count; i++) {
        if (eg_resource_key_equal_ids(&list->items[i], type, name, language)) return &list->items[i];
    }
    return NULL;
}

static eg_result eg_resource_list_set(
    eg_resource_list *list,
    const eg_internal_id *type,
    const eg_internal_id *name,
    const eg_internal_id *language,
    uint32_t code_page,
    const unsigned char *data,
    size_t size
) {
    eg_resource_item *item = eg_resource_list_find(list, type, name, language);
    unsigned char *copy = NULL;
    if (size != 0u) {
        copy = (unsigned char *)eg_alloc(size);
        if (copy == NULL) return EG_ERROR_OUT_OF_MEMORY;
        memcpy(copy, data, size);
    }
    if (item != NULL) {
        free(item->data);
        item->data = copy;
        item->size = size;
        item->code_page = code_page;
        return EG_OK;
    }
    {
        eg_result result = eg_resource_list_reserve(list, list->count + 1u);
        if (result != EG_OK) {
            free(copy);
            return result;
        }
    }
    item = &list->items[list->count++];
    memset(item, 0, sizeof(*item));
    if (eg_id_copy(&item->type, type) != EG_OK ||
        eg_id_copy(&item->name, name) != EG_OK ||
        eg_id_copy(&item->language, language) != EG_OK) {
        eg_resource_item_free(item);
        list->count--;
        free(copy);
        return EG_ERROR_OUT_OF_MEMORY;
    }
    item->data = copy;
    item->size = size;
    item->code_page = code_page;
    return EG_OK;
}

static void eg_resource_list_remove_at(eg_resource_list *list, size_t index) {
    eg_resource_item_free(&list->items[index]);
    if (index + 1u < list->count) {
        memmove(&list->items[index], &list->items[index + 1u], sizeof(*list->items) * (list->count - index - 1u));
    }
    list->count--;
}

static eg_result eg_update_reserve(eg_resource_update *update, size_t needed) {
    eg_update_op *next;
    size_t next_capacity;
    if (needed <= update->capacity) return EG_OK;
    next_capacity = update->capacity == 0u ? 16u : update->capacity;
    while (next_capacity < needed) next_capacity *= 2u;
    next = (eg_update_op *)eg_realloc_array(update->ops, next_capacity, sizeof(*update->ops));
    if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
    update->ops = next;
    update->capacity = next_capacity;
    return EG_OK;
}

eg_result eg_resource_update_create(eg_resource_update **out_update) {
    eg_resource_update *update;
    if (out_update == NULL) return EG_ERROR_INVALID_ARGUMENT;
    update = (eg_resource_update *)eg_calloc(1u, sizeof(*update));
    if (update == NULL) return EG_ERROR_OUT_OF_MEMORY;
    *out_update = update;
    return EG_OK;
}

void eg_resource_update_destroy(eg_resource_update *update) {
    size_t i;
    if (update == NULL) return;
    for (i = 0u; i < update->count; i++) {
        free(update->ops[i].data);
        free(update->ops[i].text_a);
        free(update->ops[i].text_b);
        eg_public_key_free(&update->ops[i].key);
    }
    free(update->ops);
    free(update);
}

static int eg_public_key_is_numeric(const eg_resource_key *key, uint32_t type, uint32_t name, uint16_t language) {
    return !key->type.is_string && !key->name.is_string && key->type.id == type && key->name.id == name && key->language == language;
}

static int eg_key_conflicts_string(const eg_resource_key *key, uint32_t string_id, uint16_t language) {
    uint32_t block_id = string_id / 16u + 1u;
    return eg_public_key_is_numeric(key, EG_RT_STRING, block_id, language);
}

static int eg_key_conflicts_numeric(const eg_resource_key *key, uint32_t type, uint32_t name, uint16_t language) {
    return eg_public_key_is_numeric(key, type, name, language);
}

static eg_result eg_check_duplicate_data_key(const eg_resource_update *update, const eg_resource_key *key) {
    size_t i;
    for (i = 0u; i < update->count; i++) {
        const eg_update_op *op = &update->ops[i];
        if (op->kind == EG_UPDATE_DATA && op->key.language == key->language &&
            op->key.type.is_string == key->type.is_string &&
            op->key.name.is_string == key->name.is_string &&
            op->key.type.id == key->type.id &&
            op->key.name.id == key->name.id) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (op->kind == EG_UPDATE_STRING && eg_key_conflicts_string(key, op->id, op->language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if ((op->kind == EG_UPDATE_VERSION_STRING || op->kind == EG_UPDATE_VERSION_FIXED) &&
            eg_key_conflicts_numeric(key, EG_RT_VERSION, 1u, op->language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (op->kind == EG_UPDATE_ICON && eg_key_conflicts_numeric(key, EG_RT_GROUP_ICON, op->id, op->language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
    }
    return EG_OK;
}

eg_result eg_resource_update_set_data(
    eg_resource_update *update,
    const eg_resource_key *key,
    const void *data,
    size_t size
) {
    eg_update_op *op;
    eg_result result;
    if (update == NULL || key == NULL || (data == NULL && size != 0u)) return EG_ERROR_INVALID_ARGUMENT;
    result = eg_check_duplicate_data_key(update, key);
    if (result != EG_OK) return result;
    result = eg_update_reserve(update, update->count + 1u);
    if (result != EG_OK) return result;
    op = &update->ops[update->count++];
    memset(op, 0, sizeof(*op));
    op->kind = EG_UPDATE_DATA;
    result = eg_public_key_copy(&op->key, key);
    if (result != EG_OK) {
        update->count--;
        return result;
    }
    if (size != 0u) {
        op->data = (unsigned char *)eg_alloc(size);
        if (op->data == NULL) {
            eg_public_key_free(&op->key);
            update->count--;
            return EG_ERROR_OUT_OF_MEMORY;
        }
        memcpy(op->data, data, size);
    }
    op->size = size;
    return EG_OK;
}

eg_result eg_resource_update_set_string_utf8(
    eg_resource_update *update,
    uint32_t id,
    uint16_t language,
    const char *value
) {
    eg_update_op *op;
    size_t i;
    eg_result result;
    if (update == NULL || value == NULL) return EG_ERROR_INVALID_ARGUMENT;
    for (i = 0u; i < update->count; i++) {
        if (update->ops[i].kind == EG_UPDATE_STRING && update->ops[i].id == id && update->ops[i].language == language) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (update->ops[i].kind == EG_UPDATE_DATA && eg_key_conflicts_string(&update->ops[i].key, id, language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
    }
    result = eg_update_reserve(update, update->count + 1u);
    if (result != EG_OK) return result;
    op = &update->ops[update->count++];
    memset(op, 0, sizeof(*op));
    op->kind = EG_UPDATE_STRING;
    op->id = id;
    op->language = language;
    op->text_a = eg_strdup_text(value);
    if (op->text_a == NULL) {
        update->count--;
        return EG_ERROR_OUT_OF_MEMORY;
    }
    return EG_OK;
}

eg_result eg_resource_update_set_version_string_utf8(
    eg_resource_update *update,
    uint16_t language,
    uint16_t code_page,
    const char *key,
    const char *value
) {
    eg_update_op *op;
    size_t i;
    eg_result result;
    if (update == NULL || key == NULL || value == NULL || key[0] == '\0') return EG_ERROR_INVALID_ARGUMENT;
    for (i = 0u; i < update->count; i++) {
        if (update->ops[i].kind == EG_UPDATE_VERSION_STRING &&
            update->ops[i].language == language &&
            update->ops[i].code_page == code_page &&
            strcmp(update->ops[i].text_a, key) == 0) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (update->ops[i].kind == EG_UPDATE_DATA && eg_key_conflicts_numeric(&update->ops[i].key, EG_RT_VERSION, 1u, language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
    }
    result = eg_update_reserve(update, update->count + 1u);
    if (result != EG_OK) return result;
    op = &update->ops[update->count++];
    memset(op, 0, sizeof(*op));
    op->kind = EG_UPDATE_VERSION_STRING;
    op->language = language;
    op->code_page = code_page;
    op->text_a = eg_strdup_text(key);
    op->text_b = eg_strdup_text(value);
    if (op->text_a == NULL || op->text_b == NULL) {
        free(op->text_a);
        free(op->text_b);
        update->count--;
        return EG_ERROR_OUT_OF_MEMORY;
    }
    return EG_OK;
}

eg_result eg_resource_update_set_version_fixed(
    eg_resource_update *update,
    uint16_t language,
    const eg_version_fixed_update *fixed
) {
    eg_update_op *op;
    size_t i;
    eg_result result;
    if (update == NULL || fixed == NULL) return EG_ERROR_INVALID_ARGUMENT;
    for (i = 0u; i < update->count; i++) {
        if (update->ops[i].kind == EG_UPDATE_VERSION_FIXED && update->ops[i].language == language) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (update->ops[i].kind == EG_UPDATE_DATA && eg_key_conflicts_numeric(&update->ops[i].key, EG_RT_VERSION, 1u, language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
    }
    result = eg_update_reserve(update, update->count + 1u);
    if (result != EG_OK) return result;
    op = &update->ops[update->count++];
    memset(op, 0, sizeof(*op));
    op->kind = EG_UPDATE_VERSION_FIXED;
    op->language = language;
    op->fixed = *fixed;
    return EG_OK;
}

static eg_result eg_parse_icon_file(const unsigned char *data, size_t size, eg_icon_file *icon) {
    uint16_t reserved;
    uint16_t type;
    uint16_t count;
    size_t i;
    memset(icon, 0, sizeof(*icon));
    if (size < 6u) return EG_ERROR_INVALID_RESOURCE;
    reserved = eg_read_u16(data, size, 0u);
    type = eg_read_u16(data, size, 2u);
    count = eg_read_u16(data, size, 4u);
    if (reserved != 0u || type != 1u || count == 0u) return EG_ERROR_INVALID_RESOURCE;
    if (6u + (size_t)count * 16u > size) return EG_ERROR_INVALID_RESOURCE;
    icon->images = (eg_icon_image *)eg_calloc(count, sizeof(*icon->images));
    if (icon->images == NULL) return EG_ERROR_OUT_OF_MEMORY;
    icon->count = count;
    for (i = 0u; i < count; i++) {
        size_t entry = 6u + i * 16u;
        uint32_t bytes = eg_read_u32(data, size, entry + 8u);
        uint32_t offset = eg_read_u32(data, size, entry + 12u);
        if ((uint64_t)offset + bytes > size || bytes == 0u) {
            free(icon->images);
            memset(icon, 0, sizeof(*icon));
            return EG_ERROR_INVALID_RESOURCE;
        }
        memcpy(icon->images[i].directory, data + entry, 16u);
        icon->images[i].data = data + offset;
        icon->images[i].size = bytes;
    }
    return EG_OK;
}

static void eg_icon_file_free(eg_icon_file *icon) {
    free(icon->images);
    memset(icon, 0, sizeof(*icon));
}

eg_result eg_resource_update_set_icon_from_ico_data(
    eg_resource_update *update,
    uint32_t group_id,
    uint16_t language,
    const void *data,
    size_t size
) {
    eg_update_op *op;
    eg_icon_file icon;
    size_t i;
    eg_result result;
    if (update == NULL || data == NULL || size == 0u) return EG_ERROR_INVALID_ARGUMENT;
    result = eg_parse_icon_file((const unsigned char *)data, size, &icon);
    if (result != EG_OK) return result;
    eg_icon_file_free(&icon);
    for (i = 0u; i < update->count; i++) {
        if (update->ops[i].kind == EG_UPDATE_ICON && update->ops[i].id == group_id && update->ops[i].language == language) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
        if (update->ops[i].kind == EG_UPDATE_DATA && eg_key_conflicts_numeric(&update->ops[i].key, EG_RT_GROUP_ICON, group_id, language)) {
            return EG_ERROR_DUPLICATE_RESOURCE;
        }
    }
    result = eg_update_reserve(update, update->count + 1u);
    if (result != EG_OK) return result;
    op = &update->ops[update->count++];
    memset(op, 0, sizeof(*op));
    op->kind = EG_UPDATE_ICON;
    op->id = group_id;
    op->language = language;
    op->data = (unsigned char *)eg_alloc(size);
    if (op->data == NULL) {
        update->count--;
        return EG_ERROR_OUT_OF_MEMORY;
    }
    memcpy(op->data, data, size);
    op->size = size;
    return EG_OK;
}

static eg_result eg_parse_pe_info(const unsigned char *data, size_t size, eg_pe_info *info) {
    uint32_t e_lfanew;
    uint16_t magic;
    uint32_t data_dir_count;
    uint32_t data_dir_offset;
    size_t section_table_end;
    size_t i;
    memset(info, 0, sizeof(*info));
    if (size < 0x40u || eg_read_u16(data, size, 0u) != 0x5a4du) return EG_ERROR_INVALID_PE;
    e_lfanew = eg_read_u32(data, size, 0x3cu);
    if (e_lfanew > size || (size_t)e_lfanew + 24u > size) return EG_ERROR_INVALID_PE;
    if (memcmp(data + e_lfanew, "PE\0\0", 4u) != 0) return EG_ERROR_INVALID_PE;
    info->nt_offset = e_lfanew;
    info->file_header_offset = info->nt_offset + 4u;
    info->number_sections_field = info->file_header_offset + 2u;
    info->section_count = eg_read_u16(data, size, info->number_sections_field);
    info->optional_size = eg_read_u16(data, size, info->file_header_offset + 16u);
    info->optional_offset = info->file_header_offset + 20u;
    if (info->optional_offset + info->optional_size > size) return EG_ERROR_INVALID_PE;
    magic = eg_read_u16(data, size, info->optional_offset);
    if (magic == 0x10bu) {
        info->is_pe64 = 0;
        data_dir_offset = 96u;
    } else if (magic == 0x20bu) {
        info->is_pe64 = 1;
        data_dir_offset = 112u;
    } else {
        return EG_ERROR_UNSUPPORTED_PE;
    }
    if (info->optional_size < data_dir_offset + 8u * 3u) return EG_ERROR_INVALID_PE;
    info->section_alignment = eg_read_u32(data, size, info->optional_offset + 32u);
    info->file_alignment = eg_read_u32(data, size, info->optional_offset + 36u);
    info->size_image_field = info->optional_offset + 56u;
    info->size_of_image = eg_read_u32(data, size, info->size_image_field);
    info->size_of_headers = eg_read_u32(data, size, info->optional_offset + 60u);
    data_dir_count = eg_read_u32(data, size, info->optional_offset + data_dir_offset - 4u);
    if (data_dir_count < 3u) return EG_ERROR_INVALID_PE;
    info->resource_rva_field = info->optional_offset + data_dir_offset + 16u;
    info->resource_size_field = info->resource_rva_field + 4u;
    info->resource_rva = eg_read_u32(data, size, info->resource_rva_field);
    info->resource_size = eg_read_u32(data, size, info->resource_size_field);
    info->section_table_offset = info->optional_offset + info->optional_size;
    section_table_end = info->section_table_offset + (size_t)info->section_count * 40u;
    if (section_table_end > size) return EG_ERROR_INVALID_PE;
    info->sections = (eg_section *)eg_calloc(info->section_count, sizeof(*info->sections));
    if (info->sections == NULL && info->section_count != 0u) return EG_ERROR_OUT_OF_MEMORY;
    for (i = 0u; i < info->section_count; i++) {
        size_t off = info->section_table_offset + i * 40u;
        size_t n;
        info->sections[i].header_offset = off;
        for (n = 0u; n < 8u && data[off + n] != '\0'; n++) info->sections[i].name[n] = (char)data[off + n];
        info->sections[i].name[n] = '\0';
        info->sections[i].virtual_size = eg_read_u32(data, size, off + 8u);
        info->sections[i].virtual_address = eg_read_u32(data, size, off + 12u);
        info->sections[i].raw_size = eg_read_u32(data, size, off + 16u);
        info->sections[i].raw_ptr = eg_read_u32(data, size, off + 20u);
        info->sections[i].characteristics = eg_read_u32(data, size, off + 36u);
    }
    return EG_OK;
}

static void eg_pe_info_free(eg_pe_info *info) {
    free(info->sections);
    memset(info, 0, sizeof(*info));
}

static eg_result eg_rva_to_offset(const eg_pe_info *info, uint32_t rva, size_t *out_offset) {
    size_t i;
    for (i = 0u; i < info->section_count; i++) {
        const eg_section *section = &info->sections[i];
        uint32_t span = section->virtual_size > section->raw_size ? section->virtual_size : section->raw_size;
        if (span == 0u) continue;
        if (rva >= section->virtual_address && rva < section->virtual_address + span) {
            *out_offset = (size_t)section->raw_ptr + (size_t)(rva - section->virtual_address);
            return EG_OK;
        }
    }
    return EG_ERROR_INVALID_PE;
}

eg_result eg_pe_open_file(const char *path, eg_pe_file **out_file) {
    return eg_pe_open_file_with_io(eg_default_io(), path, out_file);
}

eg_result eg_pe_open_file_with_io(const eg_io_ops *ops, const char *path, eg_pe_file **out_file) {
    eg_pe_file *file;
    eg_result result;
    if (path == NULL || out_file == NULL) return EG_ERROR_INVALID_ARGUMENT;
    *out_file = NULL;
    file = (eg_pe_file *)eg_calloc(1u, sizeof(*file));
    if (file == NULL) return EG_ERROR_OUT_OF_MEMORY;
    file->path = eg_strdup_text(path);
    if (file->path == NULL) {
        free(file);
        return EG_ERROR_OUT_OF_MEMORY;
    }
    result = eg_io_read_all(ops, path, &file->data, &file->size);
    if (result != EG_OK) {
        eg_pe_close(file);
        return result;
    }
    result = eg_parse_pe_info(file->data, file->size, &file->info);
    if (result != EG_OK) {
        eg_pe_close(file);
        return result;
    }
    *out_file = file;
    return EG_OK;
}

void eg_pe_close(eg_pe_file *file) {
    if (file == NULL) return;
    eg_pe_info_free(&file->info);
    free(file->data);
    free(file->path);
    free(file);
}

static eg_result eg_parse_resource_id(
    const unsigned char *data,
    size_t size,
    size_t base,
    size_t resource_size,
    uint32_t raw,
    eg_internal_id *out_id
) {
    memset(out_id, 0, sizeof(*out_id));
    if ((raw & 0x80000000u) == 0u) {
        *out_id = eg_make_id(raw & 0xffffu);
        return EG_OK;
    }
    {
        uint32_t rel = raw & 0x7fffffffu;
        uint16_t length;
        if (rel + 2u > resource_size || base + rel + 2u > size) return EG_ERROR_INVALID_RESOURCE;
        length = eg_read_u16(data, size, base + rel);
        if (rel + 2u + (uint32_t)length * 2u > resource_size || base + rel + 2u + (size_t)length * 2u > size) {
            return EG_ERROR_INVALID_RESOURCE;
        }
        out_id->is_string = 1;
        out_id->name_length = length;
        if (length != 0u) {
            out_id->name = (uint16_t *)eg_alloc(sizeof(uint16_t) * length);
            size_t i;
            if (out_id->name == NULL) return EG_ERROR_OUT_OF_MEMORY;
            for (i = 0u; i < length; i++) out_id->name[i] = eg_read_u16(data, size, base + rel + 2u + i * 2u);
        }
    }
    return EG_OK;
}

static eg_result eg_parse_resource_directory(
    const eg_pe_file *file,
    eg_resource_list *list,
    size_t base_offset,
    uint32_t base_rva,
    uint32_t resource_size,
    uint32_t dir_rel,
    int level,
    const eg_internal_id *type_id,
    const eg_internal_id *name_id
) {
    size_t dir_offset = base_offset + dir_rel;
    uint16_t named_count;
    uint16_t id_count;
    uint32_t entry_count;
    uint32_t i;
    if (level > 2) return EG_ERROR_UNSUPPORTED_PE;
    if (dir_rel + 16u > resource_size || dir_offset + 16u > file->size) return EG_ERROR_INVALID_RESOURCE;
    named_count = eg_read_u16(file->data, file->size, dir_offset + 12u);
    id_count = eg_read_u16(file->data, file->size, dir_offset + 14u);
    entry_count = (uint32_t)named_count + (uint32_t)id_count;
    if (dir_rel + 16u + entry_count * 8u > resource_size || dir_offset + 16u + (size_t)entry_count * 8u > file->size) {
        return EG_ERROR_INVALID_RESOURCE;
    }
    for (i = 0u; i < entry_count; i++) {
        size_t entry_off = dir_offset + 16u + (size_t)i * 8u;
        uint32_t name_raw = eg_read_u32(file->data, file->size, entry_off);
        uint32_t child_raw = eg_read_u32(file->data, file->size, entry_off + 4u);
        eg_internal_id id;
        eg_result result = eg_parse_resource_id(file->data, file->size, base_offset, resource_size, name_raw, &id);
        if (result != EG_OK) return result;
        if (level < 2) {
            if ((child_raw & 0x80000000u) == 0u) {
                eg_id_free(&id);
                return EG_ERROR_INVALID_RESOURCE;
            }
            result = eg_parse_resource_directory(
                file,
                list,
                base_offset,
                base_rva,
                resource_size,
                child_raw & 0x7fffffffu,
                level + 1,
                level == 0 ? &id : type_id,
                level == 1 ? &id : name_id
            );
            eg_id_free(&id);
            if (result != EG_OK) return result;
        } else {
            uint32_t data_rel = child_raw & 0x7fffffffu;
            uint32_t data_rva;
            uint32_t data_size;
            uint32_t code_page;
            size_t data_entry_offset = base_offset + data_rel;
            size_t data_file_offset;
            unsigned char *copy = NULL;
            if ((child_raw & 0x80000000u) != 0u) {
                eg_id_free(&id);
                return EG_ERROR_UNSUPPORTED_PE;
            }
            if (data_rel + 16u > resource_size || data_entry_offset + 16u > file->size) {
                eg_id_free(&id);
                return EG_ERROR_INVALID_RESOURCE;
            }
            data_rva = eg_read_u32(file->data, file->size, data_entry_offset);
            data_size = eg_read_u32(file->data, file->size, data_entry_offset + 4u);
            code_page = eg_read_u32(file->data, file->size, data_entry_offset + 8u);
            result = eg_rva_to_offset(&file->info, data_rva, &data_file_offset);
            if (result != EG_OK || data_file_offset + data_size > file->size) {
                eg_id_free(&id);
                return EG_ERROR_INVALID_RESOURCE;
            }
            if (data_size != 0u) {
                copy = (unsigned char *)eg_alloc(data_size);
                if (copy == NULL) {
                    eg_id_free(&id);
                    return EG_ERROR_OUT_OF_MEMORY;
                }
                memcpy(copy, file->data + data_file_offset, data_size);
            }
            result = eg_resource_list_set(list, type_id, name_id, &id, code_page, copy, data_size);
            free(copy);
            eg_id_free(&id);
            if (result != EG_OK) return result;
            (void)base_rva;
        }
    }
    return EG_OK;
}

static eg_result eg_load_resources(const eg_pe_file *file, eg_resource_list *list) {
    size_t base_offset;
    eg_result result;
    memset(list, 0, sizeof(*list));
    if (file->info.resource_rva == 0u || file->info.resource_size == 0u) return EG_OK;
    result = eg_rva_to_offset(&file->info, file->info.resource_rva, &base_offset);
    if (result != EG_OK) return result;
    if (base_offset + file->info.resource_size > file->size) return EG_ERROR_INVALID_RESOURCE;
    return eg_parse_resource_directory(
        file,
        list,
        base_offset,
        file->info.resource_rva,
        file->info.resource_size,
        0u,
        0,
        NULL,
        NULL
    );
}

static eg_result eg_parse_string_block(const unsigned char *data, size_t size, eg_string16 strings[16]) {
    size_t offset = 0u;
    size_t i;
    for (i = 0u; i < 16u; i++) {
        uint16_t length;
        strings[i].data = NULL;
        strings[i].length = 0u;
        if (offset + 2u > size) return EG_ERROR_INVALID_RESOURCE;
        length = eg_read_u16(data, size, offset);
        offset += 2u;
        if (offset + (size_t)length * 2u > size) return EG_ERROR_INVALID_RESOURCE;
        if (length != 0u) {
            size_t j;
            strings[i].data = (uint16_t *)eg_alloc(sizeof(uint16_t) * length);
            if (strings[i].data == NULL) return EG_ERROR_OUT_OF_MEMORY;
            strings[i].length = length;
            for (j = 0u; j < length; j++) strings[i].data[j] = eg_read_u16(data, size, offset + j * 2u);
        }
        offset += (size_t)length * 2u;
    }
    return EG_OK;
}

static void eg_free_string_block(eg_string16 strings[16]) {
    size_t i;
    for (i = 0u; i < 16u; i++) free(strings[i].data);
}

static eg_result eg_build_string_block(const eg_string16 strings[16], eg_buffer *buffer) {
    size_t i;
    for (i = 0u; i < 16u; i++) {
        eg_result result;
        if (strings[i].length > UINT16_MAX) return EG_ERROR_INVALID_RESOURCE;
        result = eg_buffer_append_u16(buffer, (uint16_t)strings[i].length);
        if (result != EG_OK) return result;
        result = eg_append_utf16le(buffer, strings[i].data, strings[i].length, 0);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static eg_result eg_apply_string_update(eg_resource_list *list, const eg_update_op *op) {
    eg_internal_id type = eg_make_id(EG_RT_STRING);
    eg_internal_id name = eg_make_id(op->id / 16u + 1u);
    eg_internal_id language = eg_make_id(op->language);
    eg_resource_item *item = eg_resource_list_find(list, &type, &name, &language);
    eg_string16 strings[16];
    uint16_t *wide = NULL;
    size_t wide_length = 0u;
    eg_buffer buffer;
    eg_result result;
    size_t index = op->id % 16u;
    memset(strings, 0, sizeof(strings));
    memset(&buffer, 0, sizeof(buffer));
    if (item != NULL) {
        result = eg_parse_string_block(item->data, item->size, strings);
        if (result != EG_OK) return result;
    }
    result = eg_utf8_to_utf16(op->text_a, &wide, &wide_length);
    if (result != EG_OK) {
        eg_free_string_block(strings);
        return result;
    }
    free(strings[index].data);
    strings[index].data = wide;
    strings[index].length = wide_length;
    result = eg_build_string_block(strings, &buffer);
    eg_free_string_block(strings);
    if (result != EG_OK) {
        eg_buffer_free(&buffer);
        return result;
    }
    result = eg_resource_list_set(list, &type, &name, &language, 0u, buffer.data, buffer.size);
    eg_buffer_free(&buffer);
    return result;
}

static void eg_version_model_init_default(eg_version_model *model) {
    memset(model, 0, sizeof(*model));
    model->fixed[0] = 0xfeef04bdu;
    model->fixed[1] = 0x00010000u;
    model->fixed[2] = 0u;
    model->fixed[3] = 0u;
    model->fixed[4] = 0u;
    model->fixed[5] = 0u;
    model->fixed[6] = 0x0000003fu;
    model->fixed[7] = 0u;
    model->fixed[8] = 0x00040004u;
    model->fixed[9] = 0x00000001u;
    model->fixed[10] = 0u;
    model->fixed[11] = 0u;
    model->fixed[12] = 0u;
}

static void eg_version_model_free(eg_version_model *model) {
    size_t i;
    size_t j;
    for (i = 0u; i < model->table_count; i++) {
        for (j = 0u; j < model->tables[i].string_count; j++) {
            free(model->tables[i].strings[j].key);
            free(model->tables[i].strings[j].value);
        }
        free(model->tables[i].strings);
    }
    free(model->tables);
    memset(model, 0, sizeof(*model));
}

static eg_version_table *eg_version_find_table(eg_version_model *model, uint16_t language, uint16_t code_page) {
    size_t i;
    for (i = 0u; i < model->table_count; i++) {
        if (model->tables[i].language == language && model->tables[i].code_page == code_page) return &model->tables[i];
    }
    return NULL;
}

static eg_result eg_version_add_table(eg_version_model *model, uint16_t language, uint16_t code_page, eg_version_table **out_table) {
    eg_version_table *table;
    if (model->table_count == model->table_capacity) {
        size_t next_capacity = model->table_capacity == 0u ? 4u : model->table_capacity * 2u;
        eg_version_table *next = (eg_version_table *)eg_realloc_array(model->tables, next_capacity, sizeof(*model->tables));
        if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
        model->tables = next;
        model->table_capacity = next_capacity;
    }
    table = &model->tables[model->table_count++];
    memset(table, 0, sizeof(*table));
    table->language = language;
    table->code_page = code_page;
    *out_table = table;
    return EG_OK;
}

static eg_result eg_version_set_string(eg_version_model *model, uint16_t language, uint16_t code_page, const char *key, const char *value) {
    eg_version_table *table = eg_version_find_table(model, language, code_page);
    size_t i;
    if (table == NULL) {
        eg_result result = eg_version_add_table(model, language, code_page, &table);
        if (result != EG_OK) return result;
    }
    for (i = 0u; i < table->string_count; i++) {
        if (strcmp(table->strings[i].key, key) == 0) {
            char *copy = eg_strdup_text(value);
            if (copy == NULL) return EG_ERROR_OUT_OF_MEMORY;
            free(table->strings[i].value);
            table->strings[i].value = copy;
            return EG_OK;
        }
    }
    if (table->string_count == table->string_capacity) {
        size_t next_capacity = table->string_capacity == 0u ? 8u : table->string_capacity * 2u;
        eg_version_string *next = (eg_version_string *)eg_realloc_array(table->strings, next_capacity, sizeof(*table->strings));
        if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
        table->strings = next;
        table->string_capacity = next_capacity;
    }
    table->strings[table->string_count].key = eg_strdup_text(key);
    table->strings[table->string_count].value = eg_strdup_text(value);
    if (table->strings[table->string_count].key == NULL || table->strings[table->string_count].value == NULL) {
        free(table->strings[table->string_count].key);
        free(table->strings[table->string_count].value);
        return EG_ERROR_OUT_OF_MEMORY;
    }
    table->string_count++;
    return EG_OK;
}

static eg_result eg_version_read_key(const unsigned char *data, size_t size, size_t offset, size_t block_end, char **out_key, size_t *out_value_offset) {
    size_t cursor = offset + 6u;
    size_t key_start = cursor;
    size_t key_units = 0u;
    uint16_t *wide;
    eg_result result;
    *out_key = NULL;
    *out_value_offset = 0u;
    while (cursor + 2u <= block_end && cursor + 2u <= size) {
        uint16_t ch = eg_read_u16(data, size, cursor);
        if (ch == 0u) break;
        cursor += 2u;
        key_units++;
    }
    if (cursor + 2u > block_end || cursor + 2u > size) return EG_ERROR_INVALID_RESOURCE;
    wide = (uint16_t *)eg_alloc(sizeof(uint16_t) * key_units);
    if (wide == NULL && key_units != 0u) return EG_ERROR_OUT_OF_MEMORY;
    {
        size_t i;
        for (i = 0u; i < key_units; i++) wide[i] = eg_read_u16(data, size, key_start + i * 2u);
    }
    result = eg_utf16_to_utf8(wide, key_units, out_key);
    free(wide);
    if (result != EG_OK) return result;
    cursor += 2u;
    *out_value_offset = eg_align_size(cursor, 4u);
    if (*out_value_offset > block_end) {
        free(*out_key);
        *out_key = NULL;
        return EG_ERROR_INVALID_RESOURCE;
    }
    return EG_OK;
}

static int eg_parse_table_key(const char *key, uint16_t *language, uint16_t *code_page) {
    unsigned int lang;
    unsigned int cp;
    if (strlen(key) != 8u) return 0;
    if (sscanf(key, "%4x%4x", &lang, &cp) != 2) return 0;
    if (lang > UINT16_MAX || cp > UINT16_MAX) return 0;
    *language = (uint16_t)lang;
    *code_page = (uint16_t)cp;
    return 1;
}

static eg_result eg_parse_version_string_table(
    eg_version_model *model,
    const unsigned char *data,
    size_t size,
    size_t offset,
    size_t end
) {
    uint16_t length = eg_read_u16(data, size, offset);
    char *table_key = NULL;
    size_t value_offset;
    size_t cursor;
    uint16_t language;
    uint16_t code_page;
    eg_result result;
    if (length == 0u || offset + length > end || offset + length > size) return EG_ERROR_INVALID_RESOURCE;
    result = eg_version_read_key(data, size, offset, offset + length, &table_key, &value_offset);
    if (result != EG_OK) return result;
    if (!eg_parse_table_key(table_key, &language, &code_page)) {
        free(table_key);
        return EG_OK;
    }
    free(table_key);
    cursor = value_offset;
    while (cursor + 6u <= offset + length) {
        uint16_t child_len = eg_read_u16(data, size, cursor);
        uint16_t value_len = eg_read_u16(data, size, cursor + 2u);
        char *key = NULL;
        char *value = NULL;
        size_t child_value;
        uint16_t *wide;
        size_t units;
        if (child_len == 0u) break;
        if (cursor + child_len > offset + length) return EG_ERROR_INVALID_RESOURCE;
        result = eg_version_read_key(data, size, cursor, cursor + child_len, &key, &child_value);
        if (result != EG_OK) return result;
        units = value_len;
        if (child_value + units * 2u > cursor + child_len || child_value + units * 2u > size) {
            free(key);
            return EG_ERROR_INVALID_RESOURCE;
        }
        while (units != 0u && eg_read_u16(data, size, child_value + (units - 1u) * 2u) == 0u) units--;
        wide = (uint16_t *)eg_alloc(sizeof(uint16_t) * units);
        if (wide == NULL && units != 0u) {
            free(key);
            return EG_ERROR_OUT_OF_MEMORY;
        }
        {
            size_t i;
            for (i = 0u; i < units; i++) wide[i] = eg_read_u16(data, size, child_value + i * 2u);
        }
        result = eg_utf16_to_utf8(wide, units, &value);
        free(wide);
        if (result == EG_OK) result = eg_version_set_string(model, language, code_page, key, value);
        free(key);
        free(value);
        if (result != EG_OK) return result;
        cursor = eg_align_size(cursor + child_len, 4u);
    }
    return EG_OK;
}

static eg_result eg_parse_version_model(const unsigned char *data, size_t size, eg_version_model *model) {
    uint16_t root_len;
    uint16_t root_value_len;
    char *root_key = NULL;
    size_t value_offset;
    size_t cursor;
    eg_result result;
    eg_version_model_init_default(model);
    if (size < 6u) return EG_OK;
    root_len = eg_read_u16(data, size, 0u);
    root_value_len = eg_read_u16(data, size, 2u);
    if (root_len == 0u || root_len > size) return EG_ERROR_INVALID_RESOURCE;
    result = eg_version_read_key(data, size, 0u, root_len, &root_key, &value_offset);
    if (result != EG_OK) return result;
    if (strcmp(root_key, "VS_VERSION_INFO") != 0) {
        free(root_key);
        return EG_ERROR_INVALID_RESOURCE;
    }
    free(root_key);
    if (root_value_len >= 52u && value_offset + 52u <= root_len) {
        size_t i;
        for (i = 0u; i < 13u; i++) model->fixed[i] = eg_read_u32(data, size, value_offset + i * 4u);
        cursor = eg_align_size(value_offset + 52u, 4u);
    } else {
        cursor = value_offset;
    }
    while (cursor + 6u <= root_len) {
        uint16_t child_len = eg_read_u16(data, size, cursor);
        char *key = NULL;
        size_t child_value;
        if (child_len == 0u) break;
        if (cursor + child_len > root_len) return EG_ERROR_INVALID_RESOURCE;
        result = eg_version_read_key(data, size, cursor, cursor + child_len, &key, &child_value);
        if (result != EG_OK) return result;
        if (strcmp(key, "StringFileInfo") == 0) {
            size_t table_cursor = child_value;
            while (table_cursor + 6u <= cursor + child_len) {
                uint16_t table_len = eg_read_u16(data, size, table_cursor);
                if (table_len == 0u) break;
                result = eg_parse_version_string_table(model, data, size, table_cursor, cursor + child_len);
                if (result != EG_OK) {
                    free(key);
                    return result;
                }
                table_cursor = eg_align_size(table_cursor + table_len, 4u);
            }
        }
        free(key);
        cursor = eg_align_size(cursor + child_len, 4u);
    }
    return EG_OK;
}

static eg_result eg_version_begin_block(eg_buffer *buffer, const char *key, uint16_t value_length, uint16_t type, size_t *out_start) {
    eg_result result;
    *out_start = buffer->size;
    result = eg_buffer_append_u16(buffer, 0u);
    if (result != EG_OK) return result;
    result = eg_buffer_append_u16(buffer, value_length);
    if (result != EG_OK) return result;
    result = eg_buffer_append_u16(buffer, type);
    if (result != EG_OK) return result;
    result = eg_append_utf8_as_utf16le(buffer, key, 1, NULL);
    if (result != EG_OK) return result;
    return eg_buffer_align(buffer, 4u);
}

static eg_result eg_version_end_block(eg_buffer *buffer, size_t start) {
    size_t length = buffer->size - start;
    if (length > UINT16_MAX) return EG_ERROR_INVALID_RESOURCE;
    eg_write_u16(buffer->data, start, (uint16_t)length);
    return EG_OK;
}

static eg_result eg_build_version_model(const eg_version_model *model, eg_buffer *buffer) {
    size_t root;
    size_t sfi;
    size_t vfi;
    size_t trans;
    size_t i;
    eg_result result = eg_version_begin_block(buffer, "VS_VERSION_INFO", 52u, 0u, &root);
    if (result != EG_OK) return result;
    for (i = 0u; i < 13u; i++) {
        result = eg_buffer_append_u32(buffer, model->fixed[i]);
        if (result != EG_OK) return result;
    }
    result = eg_buffer_align(buffer, 4u);
    if (result != EG_OK) return result;
    result = eg_version_begin_block(buffer, "StringFileInfo", 0u, 1u, &sfi);
    if (result != EG_OK) return result;
    for (i = 0u; i < model->table_count; i++) {
        char table_key[16];
        size_t table_start;
        size_t j;
        snprintf(table_key, sizeof(table_key), "%04x%04x", model->tables[i].language, model->tables[i].code_page);
        result = eg_version_begin_block(buffer, table_key, 0u, 1u, &table_start);
        if (result != EG_OK) return result;
        for (j = 0u; j < model->tables[i].string_count; j++) {
            size_t string_start;
            size_t units = 0u;
            size_t value_len_offset;
            result = eg_version_begin_block(buffer, model->tables[i].strings[j].key, 0u, 1u, &string_start);
            if (result != EG_OK) return result;
            value_len_offset = string_start + 2u;
            result = eg_append_utf8_as_utf16le(buffer, model->tables[i].strings[j].value, 1, &units);
            if (result != EG_OK) return result;
            if (units + 1u > UINT16_MAX) return EG_ERROR_INVALID_RESOURCE;
            eg_write_u16(buffer->data, value_len_offset, (uint16_t)(units + 1u));
            result = eg_buffer_align(buffer, 4u);
            if (result != EG_OK) return result;
            result = eg_version_end_block(buffer, string_start);
            if (result != EG_OK) return result;
        }
        result = eg_version_end_block(buffer, table_start);
        if (result != EG_OK) return result;
        result = eg_buffer_align(buffer, 4u);
        if (result != EG_OK) return result;
    }
    result = eg_version_end_block(buffer, sfi);
    if (result != EG_OK) return result;
    result = eg_buffer_align(buffer, 4u);
    if (result != EG_OK) return result;
    if (model->table_count != 0u) {
        result = eg_version_begin_block(buffer, "VarFileInfo", 0u, 1u, &vfi);
        if (result != EG_OK) return result;
        result = eg_version_begin_block(buffer, "Translation", 4u, 0u, &trans);
        if (result != EG_OK) return result;
        result = eg_buffer_append_u16(buffer, model->tables[0].language);
        if (result != EG_OK) return result;
        result = eg_buffer_append_u16(buffer, model->tables[0].code_page);
        if (result != EG_OK) return result;
        result = eg_buffer_align(buffer, 4u);
        if (result != EG_OK) return result;
        result = eg_version_end_block(buffer, trans);
        if (result != EG_OK) return result;
        result = eg_buffer_align(buffer, 4u);
        if (result != EG_OK) return result;
        result = eg_version_end_block(buffer, vfi);
        if (result != EG_OK) return result;
        result = eg_buffer_align(buffer, 4u);
        if (result != EG_OK) return result;
    }
    return eg_version_end_block(buffer, root);
}

static void eg_apply_fixed_fields(eg_version_model *model, const eg_version_fixed_update *fixed) {
    if (fixed->has_file_version) {
        model->fixed[2] = ((uint32_t)fixed->file_version.major << 16u) | fixed->file_version.minor;
        model->fixed[3] = ((uint32_t)fixed->file_version.patch << 16u) | fixed->file_version.build;
    }
    if (fixed->has_product_version) {
        model->fixed[4] = ((uint32_t)fixed->product_version.major << 16u) | fixed->product_version.minor;
        model->fixed[5] = ((uint32_t)fixed->product_version.patch << 16u) | fixed->product_version.build;
    }
    if (fixed->has_file_flags) model->fixed[7] = fixed->file_flags;
    if (fixed->has_file_os) model->fixed[8] = fixed->file_os;
    if (fixed->has_file_type) model->fixed[9] = fixed->file_type;
}

static eg_result eg_apply_version_update(eg_resource_list *list, const eg_update_op *op) {
    eg_internal_id type = eg_make_id(EG_RT_VERSION);
    eg_internal_id name = eg_make_id(1u);
    eg_internal_id language = eg_make_id(op->language);
    eg_resource_item *item = eg_resource_list_find(list, &type, &name, &language);
    eg_version_model model;
    eg_buffer buffer;
    eg_result result;
    memset(&buffer, 0, sizeof(buffer));
    if (item != NULL) result = eg_parse_version_model(item->data, item->size, &model);
    else {
        eg_version_model_init_default(&model);
        result = EG_OK;
    }
    if (result != EG_OK) return result;
    if (op->kind == EG_UPDATE_VERSION_FIXED) {
        eg_apply_fixed_fields(&model, &op->fixed);
    } else {
        result = eg_version_set_string(&model, op->language, op->code_page, op->text_a, op->text_b);
        if (result != EG_OK) {
            eg_version_model_free(&model);
            return result;
        }
    }
    result = eg_build_version_model(&model, &buffer);
    eg_version_model_free(&model);
    if (result != EG_OK) {
        eg_buffer_free(&buffer);
        return result;
    }
    result = eg_resource_list_set(list, &type, &name, &language, 0u, buffer.data, buffer.size);
    eg_buffer_free(&buffer);
    return result;
}

static size_t eg_collect_group_icon_refs(const eg_resource_item *item, uint16_t *refs, size_t max_refs) {
    uint16_t count;
    size_t i;
    if (item->size < 6u) return 0u;
    count = eg_read_u16(item->data, item->size, 4u);
    if (6u + (size_t)count * 14u > item->size) return 0u;
    for (i = 0u; i < count && i < max_refs; i++) refs[i] = eg_read_u16(item->data, item->size, 6u + i * 14u + 12u);
    return count;
}

static int eg_group_refs_icon_except(const eg_resource_list *list, uint32_t icon_id, uint32_t except_group, uint16_t except_language) {
    size_t i;
    for (i = 0u; i < list->count; i++) {
        const eg_resource_item *item = &list->items[i];
        uint16_t count;
        size_t j;
        if (item->type.is_string || item->name.is_string || item->language.is_string) continue;
        if (item->type.id != EG_RT_GROUP_ICON) continue;
        if (item->name.id == except_group && item->language.id == except_language) continue;
        if (item->size < 6u) continue;
        count = eg_read_u16(item->data, item->size, 4u);
        if (6u + (size_t)count * 14u > item->size) continue;
        for (j = 0u; j < count; j++) {
            if (eg_read_u16(item->data, item->size, 6u + j * 14u + 12u) == icon_id) return 1;
        }
    }
    return 0;
}

static int eg_icon_id_is_used(const eg_resource_list *list, uint32_t id) {
    size_t i;
    for (i = 0u; i < list->count; i++) {
        if (!list->items[i].type.is_string && !list->items[i].name.is_string &&
            list->items[i].type.id == EG_RT_ICON && list->items[i].name.id == id) {
            return 1;
        }
    }
    return 0;
}

static uint32_t eg_max_icon_id(const eg_resource_list *list) {
    size_t i;
    uint32_t max_id = 0u;
    for (i = 0u; i < list->count; i++) {
        if (!list->items[i].type.is_string && !list->items[i].name.is_string &&
            list->items[i].type.id == EG_RT_ICON && list->items[i].name.id > max_id) {
            max_id = list->items[i].name.id;
        }
    }
    return max_id;
}

static eg_result eg_apply_icon_update(eg_resource_list *list, const eg_update_op *op) {
    eg_icon_file icon;
    eg_internal_id group_type = eg_make_id(EG_RT_GROUP_ICON);
    eg_internal_id icon_type = eg_make_id(EG_RT_ICON);
    eg_internal_id group_name = eg_make_id(op->id);
    eg_internal_id language = eg_make_id(op->language);
    eg_resource_item *old_group = eg_resource_list_find(list, &group_type, &group_name, &language);
    uint16_t old_refs[256];
    uint16_t *new_refs = NULL;
    size_t old_ref_count = 0u;
    size_t i;
    uint32_t next_id;
    eg_buffer group_buffer;
    eg_result result;
    memset(&group_buffer, 0, sizeof(group_buffer));
    result = eg_parse_icon_file(op->data, op->size, &icon);
    if (result != EG_OK) return result;
    if (icon.count > 65535u) {
        eg_icon_file_free(&icon);
        return EG_ERROR_UNSUPPORTED_PE;
    }
    if (old_group != NULL) {
        old_ref_count = eg_collect_group_icon_refs(old_group, old_refs, sizeof(old_refs) / sizeof(old_refs[0]));
        if (old_ref_count > sizeof(old_refs) / sizeof(old_refs[0])) old_ref_count = 0u;
    }
    new_refs = (uint16_t *)eg_calloc(icon.count, sizeof(uint16_t));
    if (new_refs == NULL) {
        eg_icon_file_free(&icon);
        return EG_ERROR_OUT_OF_MEMORY;
    }
    next_id = eg_max_icon_id(list) + 1u;
    if (next_id == 0u) next_id = 1u;
    for (i = 0u; i < icon.count; i++) {
        if (i < old_ref_count && old_refs[i] != 0u) {
            new_refs[i] = old_refs[i];
        } else {
            while (next_id <= UINT16_MAX && eg_icon_id_is_used(list, next_id)) next_id++;
            if (next_id > UINT16_MAX) {
                free(new_refs);
                eg_icon_file_free(&icon);
                return EG_ERROR_UNSUPPORTED_PE;
            }
            new_refs[i] = (uint16_t)next_id++;
        }
    }
    result = eg_buffer_append_u16(&group_buffer, 0u);
    if (result == EG_OK) result = eg_buffer_append_u16(&group_buffer, 1u);
    if (result == EG_OK) result = eg_buffer_append_u16(&group_buffer, (uint16_t)icon.count);
    for (i = 0u; result == EG_OK && i < icon.count; i++) {
        result = eg_buffer_append(&group_buffer, icon.images[i].directory, 12u);
        if (result == EG_OK) result = eg_buffer_append_u16(&group_buffer, new_refs[i]);
    }
    for (i = 0u; result == EG_OK && i < icon.count; i++) {
        eg_internal_id icon_name = eg_make_id(new_refs[i]);
        result = eg_resource_list_set(list, &icon_type, &icon_name, &language, 0u, icon.images[i].data, icon.images[i].size);
    }
    if (result == EG_OK) {
        result = eg_resource_list_set(list, &group_type, &group_name, &language, 0u, group_buffer.data, group_buffer.size);
    }
    if (result == EG_OK) {
        for (i = 0u; i < old_ref_count; i++) {
            size_t j;
            int still_new = 0;
            for (j = 0u; j < icon.count; j++) {
                if (new_refs[j] == old_refs[i]) still_new = 1;
            }
            if (!still_new && !eg_group_refs_icon_except(list, old_refs[i], op->id, op->language)) {
                size_t k = 0u;
                while (k < list->count) {
                    if (!list->items[k].type.is_string && !list->items[k].name.is_string && !list->items[k].language.is_string &&
                        list->items[k].type.id == EG_RT_ICON &&
                        list->items[k].name.id == old_refs[i] &&
                        list->items[k].language.id == op->language) {
                        eg_resource_list_remove_at(list, k);
                    } else {
                        k++;
                    }
                }
            }
        }
    }
    eg_buffer_free(&group_buffer);
    free(new_refs);
    eg_icon_file_free(&icon);
    return result;
}

static eg_result eg_apply_updates(eg_resource_list *list, const eg_resource_update *update) {
    size_t i;
    for (i = 0u; update != NULL && i < update->count; i++) {
        const eg_update_op *op = &update->ops[i];
        eg_result result = EG_OK;
        if (op->kind == EG_UPDATE_DATA) {
            eg_internal_id type;
            eg_internal_id name;
            eg_internal_id language = eg_make_id(op->key.language);
            result = eg_id_from_public(&type, &op->key.type);
            if (result != EG_OK) return result;
            result = eg_id_from_public(&name, &op->key.name);
            if (result == EG_OK) {
                result = eg_resource_list_set(list, &type, &name, &language, 0u, op->data, op->size);
            }
            eg_id_free(&type);
            eg_id_free(&name);
        } else if (op->kind == EG_UPDATE_STRING) {
            result = eg_apply_string_update(list, op);
        } else if (op->kind == EG_UPDATE_VERSION_STRING || op->kind == EG_UPDATE_VERSION_FIXED) {
            result = eg_apply_version_update(list, op);
        } else if (op->kind == EG_UPDATE_ICON) {
            result = eg_apply_icon_update(list, op);
        }
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static void eg_node_free(eg_res_node *node) {
    size_t i;
    eg_id_free(&node->id);
    for (i = 0u; i < node->child_count; i++) eg_node_free(&node->children[i]);
    free(node->children);
    memset(node, 0, sizeof(*node));
}

static eg_result eg_node_add_child(eg_res_node *node, const eg_internal_id *id, eg_res_node **out_child) {
    eg_res_node *child;
    size_t i;
    for (i = 0u; i < node->child_count; i++) {
        if (eg_id_equal(&node->children[i].id, id)) {
            *out_child = &node->children[i];
            return EG_OK;
        }
    }
    if (node->child_count == node->child_capacity) {
        size_t next_capacity = node->child_capacity == 0u ? 8u : node->child_capacity * 2u;
        eg_res_node *next = (eg_res_node *)eg_realloc_array(node->children, next_capacity, sizeof(*node->children));
        if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
        node->children = next;
        node->child_capacity = next_capacity;
    }
    child = &node->children[node->child_count++];
    memset(child, 0, sizeof(*child));
    {
        eg_result result = eg_id_copy(&child->id, id);
        if (result != EG_OK) {
            node->child_count--;
            return result;
        }
    }
    *out_child = child;
    return EG_OK;
}

static int eg_node_compare_qsort(const void *a, const void *b) {
    const eg_res_node *na = (const eg_res_node *)a;
    const eg_res_node *nb = (const eg_res_node *)b;
    return eg_id_compare(&na->id, &nb->id);
}

static void eg_node_sort(eg_res_node *node) {
    size_t i;
    qsort(node->children, node->child_count, sizeof(*node->children), eg_node_compare_qsort);
    for (i = 0u; i < node->child_count; i++) eg_node_sort(&node->children[i]);
}

static eg_result eg_build_resource_tree(eg_resource_list *list, eg_res_node *root) {
    size_t i;
    memset(root, 0, sizeof(*root));
    for (i = 0u; i < list->count; i++) {
        eg_res_node *type_node;
        eg_res_node *name_node;
        eg_res_node *language_node;
        eg_result result = eg_node_add_child(root, &list->items[i].type, &type_node);
        if (result != EG_OK) return result;
        result = eg_node_add_child(type_node, &list->items[i].name, &name_node);
        if (result != EG_OK) return result;
        result = eg_node_add_child(name_node, &list->items[i].language, &language_node);
        if (result != EG_OK) return result;
        language_node->item = &list->items[i];
    }
    eg_node_sort(root);
    return EG_OK;
}

static eg_result eg_assign_dir_offsets(eg_res_node *node, uint32_t *offset) {
    size_t i;
    if (node->child_count == 0u) return EG_OK;
    if (*offset > UINT32_MAX - (uint32_t)(16u + node->child_count * 8u)) return EG_ERROR_INVALID_RESOURCE;
    node->dir_offset = *offset;
    *offset += (uint32_t)(16u + node->child_count * 8u);
    for (i = 0u; i < node->child_count; i++) {
        eg_result result = eg_assign_dir_offsets(&node->children[i], offset);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static eg_result eg_assign_name_offsets(eg_res_node *node, uint32_t *offset) {
    size_t i;
    if (node->id.is_string) {
        uint32_t bytes = (uint32_t)(2u + node->id.name_length * 2u);
        if (*offset > UINT32_MAX - bytes) return EG_ERROR_INVALID_RESOURCE;
        node->name_offset = *offset;
        *offset += bytes;
        *offset = (uint32_t)eg_align_size(*offset, 2u);
    }
    for (i = 0u; i < node->child_count; i++) {
        eg_result result = eg_assign_name_offsets(&node->children[i], offset);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static eg_result eg_assign_data_offsets(eg_res_node *node, uint32_t *offset) {
    size_t i;
    if (node->child_count == 0u && node->item != NULL) {
        *offset = (uint32_t)eg_align_size(*offset, 4u);
        node->data_entry_offset = *offset;
        *offset += 16u;
        return EG_OK;
    }
    for (i = 0u; i < node->child_count; i++) {
        eg_result result = eg_assign_data_offsets(&node->children[i], offset);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static eg_result eg_assign_blob_offsets(eg_res_node *node, uint32_t *offset) {
    size_t i;
    if (node->child_count == 0u && node->item != NULL) {
        *offset = (uint32_t)eg_align_size(*offset, 4u);
        node->data_offset = *offset;
        if ((uint64_t)*offset + node->item->size > UINT32_MAX) return EG_ERROR_INVALID_RESOURCE;
        *offset += (uint32_t)eg_align_size(node->item->size, 4u);
        return EG_OK;
    }
    for (i = 0u; i < node->child_count; i++) {
        eg_result result = eg_assign_blob_offsets(&node->children[i], offset);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static size_t eg_node_named_count(const eg_res_node *node) {
    size_t count = 0u;
    size_t i;
    for (i = 0u; i < node->child_count; i++) {
        if (node->children[i].id.is_string) count++;
    }
    return count;
}

static eg_result eg_emit_resource_tree(eg_res_node *node, eg_buffer *buffer, uint32_t resource_rva) {
    size_t i;
    if (node->child_count != 0u) {
        size_t named_count = eg_node_named_count(node);
        if (node->dir_offset + 16u + node->child_count * 8u > buffer->size) return EG_ERROR_INVALID_RESOURCE;
        eg_write_u16(buffer->data, node->dir_offset + 12u, (uint16_t)named_count);
        eg_write_u16(buffer->data, node->dir_offset + 14u, (uint16_t)(node->child_count - named_count));
        for (i = 0u; i < node->child_count; i++) {
            eg_res_node *child = &node->children[i];
            uint32_t name_raw = child->id.is_string ? (0x80000000u | child->name_offset) : child->id.id;
            uint32_t value_raw = child->child_count != 0u ? (0x80000000u | child->dir_offset) : child->data_entry_offset;
            size_t entry_offset = node->dir_offset + 16u + i * 8u;
            eg_write_u32(buffer->data, entry_offset, name_raw);
            eg_write_u32(buffer->data, entry_offset + 4u, value_raw);
        }
    }
    if (node->id.is_string) {
        size_t pos = node->name_offset;
        eg_write_u16(buffer->data, pos, (uint16_t)node->id.name_length);
        for (i = 0u; i < node->id.name_length; i++) eg_write_u16(buffer->data, pos + 2u + i * 2u, node->id.name[i]);
    }
    if (node->child_count == 0u && node->item != NULL) {
        eg_write_u32(buffer->data, node->data_entry_offset, resource_rva + node->data_offset);
        eg_write_u32(buffer->data, node->data_entry_offset + 4u, (uint32_t)node->item->size);
        eg_write_u32(buffer->data, node->data_entry_offset + 8u, node->item->code_page);
        if (node->item->size != 0u) memcpy(buffer->data + node->data_offset, node->item->data, node->item->size);
    }
    for (i = 0u; i < node->child_count; i++) {
        eg_result result = eg_emit_resource_tree(&node->children[i], buffer, resource_rva);
        if (result != EG_OK) return result;
    }
    return EG_OK;
}

static eg_result eg_serialize_resources(eg_resource_list *list, uint32_t resource_rva, unsigned char **out_data, size_t *out_size) {
    eg_res_node root;
    uint32_t offset = 0u;
    eg_buffer buffer;
    eg_result result;
    *out_data = NULL;
    *out_size = 0u;
    memset(&root, 0, sizeof(root));
    memset(&buffer, 0, sizeof(buffer));
    result = eg_build_resource_tree(list, &root);
    if (result == EG_OK) result = eg_assign_dir_offsets(&root, &offset);
    if (result == EG_OK) result = eg_assign_name_offsets(&root, &offset);
    offset = (uint32_t)eg_align_size(offset, 4u);
    if (result == EG_OK) result = eg_assign_data_offsets(&root, &offset);
    offset = (uint32_t)eg_align_size(offset, 4u);
    if (result == EG_OK) result = eg_assign_blob_offsets(&root, &offset);
    if (result == EG_OK) result = eg_buffer_resize(&buffer, offset);
    if (result == EG_OK) result = eg_emit_resource_tree(&root, &buffer, resource_rva);
    eg_node_free(&root);
    if (result != EG_OK) {
        eg_buffer_free(&buffer);
        return result;
    }
    *out_data = buffer.data;
    *out_size = buffer.size;
    return EG_OK;
}

static int eg_find_resource_section(const eg_pe_info *info, size_t *out_index) {
    size_t i;
    for (i = 0u; i < info->section_count; i++) {
        const eg_section *section = &info->sections[i];
        uint32_t span = section->virtual_size > section->raw_size ? section->virtual_size : section->raw_size;
        if (info->resource_rva != 0u &&
            info->resource_rva >= section->virtual_address &&
            info->resource_rva < section->virtual_address + span) {
            *out_index = i;
            return 1;
        }
    }
    for (i = 0u; i < info->section_count; i++) {
        if (strcmp(info->sections[i].name, ".rsrc") == 0) {
            *out_index = i;
            return 1;
        }
    }
    return 0;
}

static eg_result eg_compute_resource_target_rva(const eg_pe_file *file, size_t resource_size, uint32_t *out_rva) {
    size_t section_index;
    if (resource_size > UINT32_MAX) return EG_ERROR_UNSUPPORTED_PE;
    if (eg_find_resource_section(&file->info, &section_index) &&
        resource_size <= file->info.sections[section_index].raw_size) {
        *out_rva = file->info.sections[section_index].virtual_address;
        return EG_OK;
    }
    if (file->info.section_count == 0u) return EG_ERROR_UNSUPPORTED_PE;
    {
        eg_section last = file->info.sections[file->info.section_count - 1u];
        *out_rva = eg_align_u32(
            last.virtual_address + (last.virtual_size > last.raw_size ? last.virtual_size : last.raw_size),
            file->info.section_alignment
        );
    }
    return EG_OK;
}

static eg_result eg_write_resources_to_image(
    const eg_pe_file *file,
    const unsigned char *resource_data,
    size_t resource_size,
    unsigned char **out_image,
    size_t *out_size
) {
    unsigned char *image;
    size_t image_size = file->size;
    size_t section_index;
    uint32_t resource_rva;
    uint32_t resource_raw_size;
    size_t resource_raw_ptr;
    if (resource_size > UINT32_MAX) return EG_ERROR_UNSUPPORTED_PE;
    image = (unsigned char *)eg_alloc(image_size);
    if (image == NULL) return EG_ERROR_OUT_OF_MEMORY;
    memcpy(image, file->data, image_size);
    if (eg_find_resource_section(&file->info, &section_index) &&
        resource_size <= file->info.sections[section_index].raw_size &&
        file->info.sections[section_index].raw_ptr + file->info.sections[section_index].raw_size <= image_size) {
        eg_section section = file->info.sections[section_index];
        resource_rva = section.virtual_address;
        resource_raw_ptr = section.raw_ptr;
        resource_raw_size = section.raw_size;
        memset(image + resource_raw_ptr, 0, resource_raw_size);
        memcpy(image + resource_raw_ptr, resource_data, resource_size);
        eg_write_u32(
            image,
            section.header_offset + 8u,
            section.virtual_size > (uint32_t)resource_size
                ? section.virtual_size
                : (uint32_t)resource_size
        );
    } else {
        uint16_t new_count;
        size_t new_header_offset;
        size_t raw_ptr;
        uint32_t virtual_address;
        uint32_t virtual_size;
        uint32_t raw_size;
        eg_section last;
        if (file->info.section_count == 0u) {
            free(image);
            return EG_ERROR_UNSUPPORTED_PE;
        }
        new_header_offset = file->info.section_table_offset + (size_t)file->info.section_count * 40u;
        if (new_header_offset + 40u > file->info.size_of_headers || new_header_offset + 40u > image_size) {
            free(image);
            return EG_ERROR_UNSUPPORTED_PE;
        }
        last = file->info.sections[file->info.section_count - 1u];
        virtual_address = eg_align_u32(last.virtual_address + (last.virtual_size > last.raw_size ? last.virtual_size : last.raw_size), file->info.section_alignment);
        virtual_size = (uint32_t)resource_size;
        raw_size = eg_align_u32((uint32_t)resource_size, file->info.file_alignment);
        raw_ptr = eg_align_size(image_size, file->info.file_alignment);
        if (raw_ptr + raw_size < raw_ptr) {
            free(image);
            return EG_ERROR_UNSUPPORTED_PE;
        }
        {
            unsigned char *next = (unsigned char *)realloc(image, raw_ptr + raw_size);
            if (next == NULL) {
                free(image);
                return EG_ERROR_OUT_OF_MEMORY;
            }
            image = next;
            memset(image + image_size, 0, raw_ptr + raw_size - image_size);
            image_size = raw_ptr + raw_size;
        }
        memcpy(image + raw_ptr, resource_data, resource_size);
        memset(image + new_header_offset, 0, 40u);
        memcpy(image + new_header_offset, ".rsrc", 5u);
        eg_write_u32(image, new_header_offset + 8u, virtual_size);
        eg_write_u32(image, new_header_offset + 12u, virtual_address);
        eg_write_u32(image, new_header_offset + 16u, raw_size);
        eg_write_u32(image, new_header_offset + 20u, (uint32_t)raw_ptr);
        eg_write_u32(image, new_header_offset + 36u, 0x40000040u);
        new_count = (uint16_t)(file->info.section_count + 1u);
        eg_write_u16(image, file->info.number_sections_field, new_count);
        eg_write_u32(image, file->info.size_image_field, eg_align_u32(virtual_address + virtual_size, file->info.section_alignment));
        resource_rva = virtual_address;
    }
    eg_write_u32(image, file->info.resource_rva_field, resource_rva);
    eg_write_u32(image, file->info.resource_size_field, (uint32_t)resource_size);
    *out_image = image;
    *out_size = image_size;
    return EG_OK;
}

eg_result eg_pe_write_file(const eg_pe_file *file, const eg_resource_update *update, const char *output_path) {
    return eg_pe_write_file_with_io(eg_default_io(), file, update, output_path);
}

eg_result eg_pe_write_file_with_io(
    const eg_io_ops *ops,
    const eg_pe_file *file,
    const eg_resource_update *update,
    const char *output_path
) {
    eg_resource_list list;
    unsigned char *resource_data = NULL;
    size_t resource_size = 0u;
    unsigned char *image = NULL;
    size_t image_size = 0u;
    uint32_t target_rva = 0u;
    eg_result result;
    result = eg_validate_write_ops(ops);
    if (result != EG_OK) return result;
    if (file == NULL || output_path == NULL) return EG_ERROR_INVALID_ARGUMENT;
    if (file->path != NULL && strcmp(file->path, output_path) == 0) return EG_ERROR_INVALID_ARGUMENT;
    memset(&list, 0, sizeof(list));
    result = eg_load_resources(file, &list);
    if (result == EG_OK) result = eg_apply_updates(&list, update);
    if (result == EG_OK) {
        result = eg_serialize_resources(&list, 0u, &resource_data, &resource_size);
    }
    if (result == EG_OK) {
        result = eg_compute_resource_target_rva(file, resource_size, &target_rva);
    }
    if (result == EG_OK) {
        free(resource_data);
        resource_data = NULL;
        resource_size = 0u;
        result = eg_serialize_resources(&list, target_rva, &resource_data, &resource_size);
    }
    if (result == EG_OK) result = eg_write_resources_to_image(file, resource_data, resource_size, &image, &image_size);
    if (result == EG_OK) result = eg_io_write_all(ops, output_path, image, image_size);
    free(resource_data);
    free(image);
    eg_resource_list_free(&list);
    return result;
}

static int eg_json_is_allowed_key(const char *key, const char *const *allowed, size_t count) {
    size_t i;
    for (i = 0u; i < count; i++) {
        if (strcmp(key, allowed[i]) == 0) return 1;
    }
    return 0;
}

static eg_result eg_json_check_keys(yyjson_val *obj, const char *const *allowed, size_t count) {
    yyjson_obj_iter iter = yyjson_obj_iter_with(obj);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *text = yyjson_get_str(key);
        if (text == NULL || !eg_json_is_allowed_key(text, allowed, count)) return EG_ERROR_INVALID_JSON;
    }
    return EG_OK;
}

static eg_result eg_json_uint32(yyjson_val *obj, const char *name, uint32_t max_value, uint32_t *out_value) {
    yyjson_val *value = yyjson_obj_get(obj, name);
    uint64_t number;
    if (value == NULL || !yyjson_is_uint(value)) return EG_ERROR_INVALID_JSON;
    number = yyjson_get_uint(value);
    if (number > max_value) return EG_ERROR_INVALID_JSON;
    *out_value = (uint32_t)number;
    return EG_OK;
}

static eg_result eg_json_required_string(yyjson_val *obj, const char *name, char **out_text) {
    yyjson_val *value = yyjson_obj_get(obj, name);
    const char *text;
    if (value == NULL || !yyjson_is_str(value)) return EG_ERROR_INVALID_JSON;
    text = yyjson_get_str(value);
    *out_text = eg_strdup_len(text, yyjson_get_len(value));
    return *out_text == NULL ? EG_ERROR_OUT_OF_MEMORY : EG_OK;
}

static eg_result eg_parse_version_quad_text(const char *text, eg_version_quad *quad) {
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    char tail;
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return EG_ERROR_INVALID_JSON;
    if (a > UINT16_MAX || b > UINT16_MAX || c > UINT16_MAX || d > UINT16_MAX) return EG_ERROR_INVALID_JSON;
    quad->major = (uint16_t)a;
    quad->minor = (uint16_t)b;
    quad->patch = (uint16_t)c;
    quad->build = (uint16_t)d;
    return EG_OK;
}

static eg_result eg_json_fixed_string_or_number(yyjson_val *value, const char *kind, uint32_t *out_value) {
    if (yyjson_is_uint(value)) {
        uint64_t number = yyjson_get_uint(value);
        if (number > UINT32_MAX) return EG_ERROR_INVALID_JSON;
        *out_value = (uint32_t)number;
        return EG_OK;
    }
    if (!yyjson_is_str(value)) return EG_ERROR_INVALID_JSON;
    if (strcmp(kind, "fileOS") == 0) {
        const char *text = yyjson_get_str(value);
        if (strcmp(text, "windows32") == 0) {
            *out_value = 0x00040004u;
            return EG_OK;
        }
    }
    if (strcmp(kind, "fileType") == 0) {
        const char *text = yyjson_get_str(value);
        if (strcmp(text, "app") == 0) {
            *out_value = 1u;
            return EG_OK;
        }
        if (strcmp(text, "dll") == 0) {
            *out_value = 2u;
            return EG_OK;
        }
    }
    return EG_ERROR_INVALID_JSON;
}

static eg_result eg_parse_json_fixed(yyjson_val *fixed_obj, eg_version_fixed_update *fixed) {
    static const char *const keys[] = { "fileVersion", "productVersion", "fileFlags", "fileOS", "fileType" };
    yyjson_val *value;
    eg_result result;
    if (fixed_obj == NULL) return EG_OK;
    if (!yyjson_is_obj(fixed_obj)) return EG_ERROR_INVALID_JSON;
    result = eg_json_check_keys(fixed_obj, keys, sizeof(keys) / sizeof(keys[0]));
    if (result != EG_OK) return result;
    value = yyjson_obj_get(fixed_obj, "fileVersion");
    if (value != NULL) {
        if (!yyjson_is_str(value)) return EG_ERROR_INVALID_JSON;
        result = eg_parse_version_quad_text(yyjson_get_str(value), &fixed->file_version);
        if (result != EG_OK) return result;
        fixed->has_file_version = 1;
    }
    value = yyjson_obj_get(fixed_obj, "productVersion");
    if (value != NULL) {
        if (!yyjson_is_str(value)) return EG_ERROR_INVALID_JSON;
        result = eg_parse_version_quad_text(yyjson_get_str(value), &fixed->product_version);
        if (result != EG_OK) return result;
        fixed->has_product_version = 1;
    }
    value = yyjson_obj_get(fixed_obj, "fileFlags");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) > UINT32_MAX) return EG_ERROR_INVALID_JSON;
        fixed->file_flags = (uint32_t)yyjson_get_uint(value);
        fixed->has_file_flags = 1;
    }
    value = yyjson_obj_get(fixed_obj, "fileOS");
    if (value != NULL) {
        result = eg_json_fixed_string_or_number(value, "fileOS", &fixed->file_os);
        if (result != EG_OK) return result;
        fixed->has_file_os = 1;
    }
    value = yyjson_obj_get(fixed_obj, "fileType");
    if (value != NULL) {
        result = eg_json_fixed_string_or_number(value, "fileType", &fixed->file_type);
        if (result != EG_OK) return result;
        fixed->has_file_type = 1;
    }
    return EG_OK;
}

eg_result eg_load_json_file(const char *path, eg_json_document **out_document) {
    return eg_load_json_file_with_io(eg_default_io(), path, out_document);
}

eg_result eg_load_json_file_with_io(const eg_io_ops *ops, const char *path, eg_json_document **out_document) {
    static const char *const root_keys[] = { "strings", "version", "icons", "raw" };
    unsigned char *bytes = NULL;
    size_t byte_size = 0u;
    yyjson_read_err error;
    yyjson_doc *json = NULL;
    yyjson_val *root;
    eg_owned_json_document *owned = NULL;
    eg_result result;
    if (path == NULL || out_document == NULL) return EG_ERROR_INVALID_ARGUMENT;
    *out_document = NULL;
    result = eg_io_read_all(ops, path, &bytes, &byte_size);
    if (result != EG_OK) return result;
    memset(&error, 0, sizeof(error));
    json = yyjson_read_opts((char *)bytes, byte_size, 0, NULL, &error);
    if (json == NULL) {
        free(bytes);
        return EG_ERROR_INVALID_JSON;
    }
    root = yyjson_doc_get_root(json);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(json);
        free(bytes);
        return EG_ERROR_INVALID_JSON;
    }
    owned = (eg_owned_json_document *)eg_calloc(1u, sizeof(*owned));
    if (owned == NULL) {
        yyjson_doc_free(json);
        free(bytes);
        return EG_ERROR_OUT_OF_MEMORY;
    }
    owned->magic = EG_JSON_MAGIC;
    owned->document.base_dir = eg_path_dirname(path);
    if (owned->document.base_dir == NULL) result = EG_ERROR_OUT_OF_MEMORY;
    if (result == EG_OK) result = eg_json_check_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]));
    if (result == EG_OK) {
        yyjson_val *strings = yyjson_obj_get(root, "strings");
        if (strings != NULL) {
            yyjson_arr_iter iter;
            yyjson_val *entry;
            size_t index = 0u;
            if (!yyjson_is_arr(strings)) result = EG_ERROR_INVALID_JSON;
            else {
                owned->document.string_count = yyjson_get_len(strings);
                owned->strings = (eg_json_string_update *)eg_calloc(owned->document.string_count, sizeof(*owned->strings));
                if (owned->strings == NULL && owned->document.string_count != 0u) result = EG_ERROR_OUT_OF_MEMORY;
                yyjson_arr_iter_init(strings, &iter);
                while (result == EG_OK && (entry = yyjson_arr_iter_next(&iter)) != NULL) {
                    static const char *const keys[] = { "id", "language", "value" };
                    uint32_t temp;
                    if (!yyjson_is_obj(entry)) result = EG_ERROR_INVALID_JSON;
                    if (result == EG_OK) result = eg_json_check_keys(entry, keys, sizeof(keys) / sizeof(keys[0]));
                    if (result == EG_OK) result = eg_json_uint32(entry, "id", UINT32_MAX, &owned->strings[index].id);
                    if (result == EG_OK) result = eg_json_uint32(entry, "language", UINT16_MAX, &temp);
                    if (result == EG_OK) owned->strings[index].language = (uint16_t)temp;
                    if (result == EG_OK) result = eg_json_required_string(entry, "value", (char **)&owned->strings[index].value_utf8);
                    index++;
                }
                owned->document.strings = owned->strings;
            }
        }
    }
    if (result == EG_OK) {
        yyjson_val *icons = yyjson_obj_get(root, "icons");
        if (icons != NULL) {
            yyjson_arr_iter iter;
            yyjson_val *entry;
            size_t index = 0u;
            if (!yyjson_is_arr(icons)) result = EG_ERROR_INVALID_JSON;
            else {
                owned->document.icon_count = yyjson_get_len(icons);
                owned->icons = (eg_json_icon_update *)eg_calloc(owned->document.icon_count, sizeof(*owned->icons));
                if (owned->icons == NULL && owned->document.icon_count != 0u) result = EG_ERROR_OUT_OF_MEMORY;
                yyjson_arr_iter_init(icons, &iter);
                while (result == EG_OK && (entry = yyjson_arr_iter_next(&iter)) != NULL) {
                    static const char *const keys[] = { "id", "language", "path" };
                    uint32_t temp;
                    if (!yyjson_is_obj(entry)) result = EG_ERROR_INVALID_JSON;
                    if (result == EG_OK) result = eg_json_check_keys(entry, keys, sizeof(keys) / sizeof(keys[0]));
                    if (result == EG_OK) result = eg_json_uint32(entry, "id", UINT32_MAX, &owned->icons[index].id);
                    if (result == EG_OK) result = eg_json_uint32(entry, "language", UINT16_MAX, &temp);
                    if (result == EG_OK) owned->icons[index].language = (uint16_t)temp;
                    if (result == EG_OK) result = eg_json_required_string(entry, "path", (char **)&owned->icons[index].path);
                    index++;
                }
                owned->document.icons = owned->icons;
            }
        }
    }
    if (result == EG_OK) {
        yyjson_val *raw = yyjson_obj_get(root, "raw");
        if (raw != NULL) {
            yyjson_arr_iter iter;
            yyjson_val *entry;
            size_t index = 0u;
            if (!yyjson_is_arr(raw)) result = EG_ERROR_INVALID_JSON;
            else {
                owned->document.raw_count = yyjson_get_len(raw);
                owned->raw = (eg_json_raw_update *)eg_calloc(owned->document.raw_count, sizeof(*owned->raw));
                if (owned->raw == NULL && owned->document.raw_count != 0u) result = EG_ERROR_OUT_OF_MEMORY;
                yyjson_arr_iter_init(raw, &iter);
                while (result == EG_OK && (entry = yyjson_arr_iter_next(&iter)) != NULL) {
                    static const char *const keys[] = { "type", "id", "language", "path" };
                    uint32_t temp;
                    if (!yyjson_is_obj(entry)) result = EG_ERROR_INVALID_JSON;
                    if (result == EG_OK) result = eg_json_check_keys(entry, keys, sizeof(keys) / sizeof(keys[0]));
                    if (result == EG_OK) result = eg_json_uint32(entry, "type", UINT32_MAX, &owned->raw[index].type);
                    if (result == EG_OK) result = eg_json_uint32(entry, "id", UINT32_MAX, &owned->raw[index].id);
                    if (result == EG_OK) result = eg_json_uint32(entry, "language", UINT16_MAX, &temp);
                    if (result == EG_OK) owned->raw[index].language = (uint16_t)temp;
                    if (result == EG_OK) result = eg_json_required_string(entry, "path", (char **)&owned->raw[index].path);
                    index++;
                }
                owned->document.raw = owned->raw;
            }
        }
    }
    if (result == EG_OK) {
        yyjson_val *version = yyjson_obj_get(root, "version");
        if (version != NULL) {
            static const char *const keys[] = { "language", "codePage", "fixed", "strings" };
            yyjson_val *value;
            uint32_t temp;
            if (!yyjson_is_obj(version)) result = EG_ERROR_INVALID_JSON;
            if (result == EG_OK) result = eg_json_check_keys(version, keys, sizeof(keys) / sizeof(keys[0]));
            if (result == EG_OK) result = eg_json_uint32(version, "language", UINT16_MAX, &temp);
            if (result == EG_OK) {
                owned->version.language = (uint16_t)temp;
                owned->version.code_page = EG_DEFAULT_CODE_PAGE;
            }
            value = yyjson_obj_get(version, "codePage");
            if (result == EG_OK && value != NULL) {
                if (!yyjson_is_uint(value) || yyjson_get_uint(value) > UINT16_MAX) result = EG_ERROR_INVALID_JSON;
                else {
                    owned->version.code_page = (uint16_t)yyjson_get_uint(value);
                    owned->version.has_code_page = 1;
                }
            }
            if (result == EG_OK) result = eg_parse_json_fixed(yyjson_obj_get(version, "fixed"), &owned->version.fixed);
            value = yyjson_obj_get(version, "strings");
            if (result == EG_OK && value != NULL) {
                yyjson_obj_iter iter;
                yyjson_val *key;
                size_t index = 0u;
                if (!yyjson_is_obj(value)) result = EG_ERROR_INVALID_JSON;
                else {
                    owned->version.string_count = yyjson_get_len(value);
                    owned->version_strings = (eg_json_version_string_update *)eg_calloc(owned->version.string_count, sizeof(*owned->version_strings));
                    if (owned->version_strings == NULL && owned->version.string_count != 0u) result = EG_ERROR_OUT_OF_MEMORY;
                    yyjson_obj_iter_init(value, &iter);
                    while (result == EG_OK && (key = yyjson_obj_iter_next(&iter)) != NULL) {
                        yyjson_val *val = yyjson_obj_iter_get_val(key);
                        if (!yyjson_is_str(val)) result = EG_ERROR_INVALID_JSON;
                        else {
                            owned->version_strings[index].key_utf8 = eg_strdup_len(yyjson_get_str(key), yyjson_get_len(key));
                            owned->version_strings[index].value_utf8 = eg_strdup_len(yyjson_get_str(val), yyjson_get_len(val));
                            if (owned->version_strings[index].key_utf8 == NULL || owned->version_strings[index].value_utf8 == NULL) {
                                result = EG_ERROR_OUT_OF_MEMORY;
                            }
                        }
                        index++;
                    }
                    owned->version.strings = owned->version_strings;
                }
            }
            if (result == EG_OK) {
                owned->has_version = 1;
                owned->document.version = &owned->version;
            }
        }
    }
    yyjson_doc_free(json);
    free(bytes);
    if (result != EG_OK) {
        eg_release_json(&owned->document);
        return result;
    }
    *out_document = &owned->document;
    return EG_OK;
}

void eg_release_json(eg_json_document *document) {
    eg_owned_json_document *owned;
    size_t i;
    if (document == NULL) return;
    owned = (eg_owned_json_document *)document;
    if (owned->magic != EG_JSON_MAGIC) return;
    for (i = 0u; i < owned->document.string_count; i++) free((void *)owned->strings[i].value_utf8);
    for (i = 0u; i < owned->document.icon_count; i++) free((void *)owned->icons[i].path);
    for (i = 0u; i < owned->document.raw_count; i++) free((void *)owned->raw[i].path);
    for (i = 0u; i < owned->version.string_count; i++) {
        free((void *)owned->version_strings[i].key_utf8);
        free((void *)owned->version_strings[i].value_utf8);
    }
    free((void *)owned->document.base_dir);
    free(owned->strings);
    free(owned->icons);
    free(owned->raw);
    free(owned->version_strings);
    memset(owned, 0, sizeof(*owned));
    free(owned);
}

eg_result eg_json_document_to_update(const eg_json_document *document, eg_resource_update **out_update) {
    return eg_json_document_to_update_with_io(eg_default_io(), document, out_update);
}

eg_result eg_json_document_to_update_with_io(
    const eg_io_ops *ops,
    const eg_json_document *document,
    eg_resource_update **out_update
) {
    eg_resource_update *update = NULL;
    eg_result result;
    size_t i;
    result = eg_validate_read_ops(ops);
    if (result != EG_OK) return result;
    if (document == NULL || out_update == NULL) return EG_ERROR_INVALID_ARGUMENT;
    *out_update = NULL;
    result = eg_resource_update_create(&update);
    if (result != EG_OK) return result;
    for (i = 0u; result == EG_OK && i < document->string_count; i++) {
        result = eg_resource_update_set_string_utf8(update, document->strings[i].id, document->strings[i].language, document->strings[i].value_utf8);
    }
    if (result == EG_OK && document->version != NULL) {
        const eg_json_version_update *version = document->version;
        if (version->fixed.has_file_version || version->fixed.has_product_version ||
            version->fixed.has_file_flags || version->fixed.has_file_os || version->fixed.has_file_type) {
            result = eg_resource_update_set_version_fixed(update, version->language, &version->fixed);
        }
        for (i = 0u; result == EG_OK && i < version->string_count; i++) {
            result = eg_resource_update_set_version_string_utf8(
                update,
                version->language,
                version->has_code_page ? version->code_page : EG_DEFAULT_CODE_PAGE,
                version->strings[i].key_utf8,
                version->strings[i].value_utf8
            );
        }
    }
    for (i = 0u; result == EG_OK && i < document->icon_count; i++) {
        char *path = eg_path_join(document->base_dir, document->icons[i].path);
        unsigned char *data = NULL;
        size_t size = 0u;
        if (path == NULL) result = EG_ERROR_OUT_OF_MEMORY;
        if (result == EG_OK) result = eg_io_read_all(ops, path, &data, &size);
        if (result == EG_OK) result = eg_resource_update_set_icon_from_ico_data(update, document->icons[i].id, document->icons[i].language, data, size);
        free(data);
        free(path);
    }
    for (i = 0u; result == EG_OK && i < document->raw_count; i++) {
        char *path = eg_path_join(document->base_dir, document->raw[i].path);
        unsigned char *data = NULL;
        size_t size = 0u;
        eg_resource_key key;
        memset(&key, 0, sizeof(key));
        key.type.id = document->raw[i].type;
        key.name.id = document->raw[i].id;
        key.language = document->raw[i].language;
        if (path == NULL) result = EG_ERROR_OUT_OF_MEMORY;
        if (result == EG_OK) result = eg_io_read_all(ops, path, &data, &size);
        if (result == EG_OK) result = eg_resource_update_set_data(update, &key, data, size);
        free(data);
        free(path);
    }
    if (result != EG_OK) {
        eg_resource_update_destroy(update);
        return result;
    }
    *out_update = update;
    return EG_OK;
}
