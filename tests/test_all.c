#include "engraver.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct test_buffer {
    unsigned char *data;
    size_t size;
} test_buffer;

typedef struct test_section {
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t raw_size;
    uint32_t raw_ptr;
} test_section;

typedef struct test_resource {
    uint32_t type;
    uint32_t name;
    uint32_t language;
    const unsigned char *data;
    size_t size;
} test_resource;

typedef struct test_resource_list {
    test_resource items[128];
    size_t count;
} test_resource_list;

typedef struct test_memory_file {
    const char *path;
    const unsigned char *read_data;
    size_t read_size;
    unsigned char *write_data;
    size_t write_size;
    size_t write_capacity;
} test_memory_file;

typedef struct test_memory_fs {
    test_memory_file *files;
    size_t file_count;
    const char *fail_open_read_path;
    const char *fail_open_write_path;
    const char *fail_read_path;
    const char *fail_write_path;
} test_memory_fs;

typedef struct test_memory_handle {
    test_memory_file *file;
    size_t offset;
    int write_mode;
} test_memory_handle;

static int test_fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    return 1;
}

#define CHECK(expr) do { if (!(expr)) return test_fail(__FILE__, __LINE__, #expr); } while (0)

static uint16_t read_u16(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 2u > size) return 0u;
    return (uint16_t)data[offset] | (uint16_t)((uint16_t)data[offset + 1u] << 8u);
}

static uint32_t read_u32(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 4u > size) return 0u;
    return (uint32_t)data[offset] |
        ((uint32_t)data[offset + 1u] << 8u) |
        ((uint32_t)data[offset + 2u] << 16u) |
        ((uint32_t)data[offset + 3u] << 24u);
}

static void write_u16(unsigned char *data, size_t offset, uint16_t value) {
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1u] = (unsigned char)((value >> 8u) & 0xffu);
}

static void write_u32(unsigned char *data, size_t offset, uint32_t value) {
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1u] = (unsigned char)((value >> 8u) & 0xffu);
    data[offset + 2u] = (unsigned char)((value >> 16u) & 0xffu);
    data[offset + 3u] = (unsigned char)((value >> 24u) & 0xffu);
}

static uint32_t align_u32(uint32_t value, uint32_t alignment) {
    return (uint32_t)(((uint64_t)value + alignment - 1u) / alignment * alignment);
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (size != 0u && fwrite(data, 1u, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int read_file(const char *path, test_buffer *buffer) {
    FILE *file = fopen(path, "rb");
    long size;
    buffer->data = NULL;
    buffer->size = 0u;
    if (file == NULL) return 0;
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    buffer->data = (unsigned char *)malloc((size_t)size == 0u ? 1u : (size_t)size);
    if (buffer->data == NULL) {
        fclose(file);
        return 0;
    }
    buffer->size = (size_t)size;
    if (buffer->size != 0u && fread(buffer->data, 1u, buffer->size, file) != buffer->size) {
        fclose(file);
        free(buffer->data);
        buffer->data = NULL;
        return 0;
    }
    fclose(file);
    return 1;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0) return 1;
    return 1;
}

static test_memory_file *memory_find_file(test_memory_fs *fs, const char *path) {
    size_t i;
    for (i = 0u; i < fs->file_count; i++) {
        if (strcmp(fs->files[i].path, path) == 0) return &fs->files[i];
    }
    return NULL;
}

static eg_result memory_open_read(void *context, const char *path, void **out_handle) {
    test_memory_fs *fs = (test_memory_fs *)context;
    test_memory_file *file;
    test_memory_handle *handle;
    if (fs->fail_open_read_path != NULL && strcmp(fs->fail_open_read_path, path) == 0) return EG_ERROR_IO;
    file = memory_find_file(fs, path);
    if (file == NULL || file->read_data == NULL) return EG_ERROR_IO;
    handle = (test_memory_handle *)calloc(1u, sizeof(*handle));
    if (handle == NULL) return EG_ERROR_OUT_OF_MEMORY;
    handle->file = file;
    handle->write_mode = 0;
    *out_handle = handle;
    return EG_OK;
}

static eg_result memory_open_write(void *context, const char *path, void **out_handle) {
    test_memory_fs *fs = (test_memory_fs *)context;
    test_memory_file *file;
    test_memory_handle *handle;
    if (fs->fail_open_write_path != NULL && strcmp(fs->fail_open_write_path, path) == 0) return EG_ERROR_IO;
    file = memory_find_file(fs, path);
    if (file == NULL) return EG_ERROR_IO;
    free(file->write_data);
    file->write_data = NULL;
    file->write_size = 0u;
    file->write_capacity = 0u;
    handle = (test_memory_handle *)calloc(1u, sizeof(*handle));
    if (handle == NULL) return EG_ERROR_OUT_OF_MEMORY;
    handle->file = file;
    handle->write_mode = 1;
    *out_handle = handle;
    return EG_OK;
}

static eg_result memory_read(void *context, void *raw_handle, void *buffer, size_t size, size_t *out_size) {
    test_memory_fs *fs = (test_memory_fs *)context;
    test_memory_handle *handle = (test_memory_handle *)raw_handle;
    size_t available;
    size_t amount;
    if (fs->fail_read_path != NULL && strcmp(fs->fail_read_path, handle->file->path) == 0) return EG_ERROR_IO;
    if (handle->write_mode) return EG_ERROR_IO;
    available = handle->file->read_size - handle->offset;
    amount = available < size ? available : size;
    if (amount != 0u) memcpy(buffer, handle->file->read_data + handle->offset, amount);
    handle->offset += amount;
    *out_size = amount;
    return EG_OK;
}

static eg_result memory_write(void *context, void *raw_handle, const void *buffer, size_t size) {
    test_memory_fs *fs = (test_memory_fs *)context;
    test_memory_handle *handle = (test_memory_handle *)raw_handle;
    unsigned char *next;
    if (fs->fail_write_path != NULL && strcmp(fs->fail_write_path, handle->file->path) == 0) return EG_ERROR_IO;
    if (!handle->write_mode) return EG_ERROR_IO;
    if (handle->file->write_size + size > handle->file->write_capacity) {
        size_t next_capacity = handle->file->write_capacity == 0u ? 256u : handle->file->write_capacity;
        while (next_capacity < handle->file->write_size + size) next_capacity *= 2u;
        next = (unsigned char *)realloc(handle->file->write_data, next_capacity);
        if (next == NULL) return EG_ERROR_OUT_OF_MEMORY;
        handle->file->write_data = next;
        handle->file->write_capacity = next_capacity;
    }
    if (size != 0u) memcpy(handle->file->write_data + handle->file->write_size, buffer, size);
    handle->file->write_size += size;
    return EG_OK;
}

static eg_result memory_close(void *context, void *raw_handle) {
    (void)context;
    free(raw_handle);
    return EG_OK;
}

static eg_io_ops memory_ops(test_memory_fs *fs) {
    eg_io_ops ops;
    ops.context = fs;
    ops.open_read = memory_open_read;
    ops.open_write = memory_open_write;
    ops.read = memory_read;
    ops.write = memory_write;
    ops.close = memory_close;
    return ops;
}

static void memory_files_free(test_memory_file *files, size_t count) {
    size_t i;
    for (i = 0u; i < count; i++) free(files[i].write_data);
}

static int create_minimal_pe(const char *path, int pe64, uint32_t size_of_headers) {
    const uint32_t file_alignment = 0x200u;
    const uint32_t section_alignment = 0x1000u;
    const uint32_t header_size = size_of_headers < 0x400u ? 0x400u : size_of_headers;
    const uint32_t text_raw_ptr = align_u32(header_size, file_alignment);
    const uint32_t text_raw_size = 0x200u;
    const uint32_t file_size = text_raw_ptr + text_raw_size;
    const uint32_t nt = 0x80u;
    const uint16_t optional_size = pe64 ? 240u : 224u;
    const uint32_t optional = nt + 24u;
    const uint32_t section = optional + optional_size;
    const uint32_t data_dir = pe64 ? 112u : 96u;
    unsigned char *data = (unsigned char *)calloc(1u, file_size);
    int ok;
    if (data == NULL) return 0;
    write_u16(data, 0u, 0x5a4du);
    write_u32(data, 0x3cu, nt);
    memcpy(data + nt, "PE\0\0", 4u);
    write_u16(data, nt + 4u, pe64 ? 0x8664u : 0x14cu);
    write_u16(data, nt + 6u, 1u);
    write_u16(data, nt + 20u, optional_size);
    write_u16(data, nt + 22u, 0x0102u);
    write_u16(data, optional, pe64 ? 0x20bu : 0x10bu);
    write_u32(data, optional + 16u, 0x1000u);
    write_u32(data, optional + 20u, 0x1000u);
    if (pe64) {
        write_u32(data, optional + 24u, 0x00400000u);
    } else {
        write_u32(data, optional + 28u, 0x00400000u);
    }
    write_u32(data, optional + 32u, section_alignment);
    write_u32(data, optional + 36u, file_alignment);
    write_u16(data, optional + 40u, 6u);
    write_u16(data, optional + 48u, 6u);
    write_u32(data, optional + 56u, 0x2000u);
    write_u32(data, optional + 60u, size_of_headers);
    write_u16(data, optional + 68u, 3u);
    write_u32(data, optional + data_dir - 4u, 16u);
    memcpy(data + section, ".text", 5u);
    write_u32(data, section + 8u, 1u);
    write_u32(data, section + 12u, 0x1000u);
    write_u32(data, section + 16u, text_raw_size);
    write_u32(data, section + 20u, text_raw_ptr);
    write_u32(data, section + 36u, 0x60000020u);
    data[text_raw_ptr] = 0xc3u;
    ok = write_file(path, data, file_size);
    free(data);
    return ok;
}

static size_t rva_to_offset(
    const test_section *sections,
    size_t section_count,
    uint32_t rva
) {
    size_t i;
    for (i = 0u; i < section_count; i++) {
        uint32_t span = sections[i].virtual_size > sections[i].raw_size ? sections[i].virtual_size : sections[i].raw_size;
        if (rva >= sections[i].virtual_address && rva < sections[i].virtual_address + span) {
            return sections[i].raw_ptr + (rva - sections[i].virtual_address);
        }
    }
    return (size_t)-1;
}

static int parse_resource_dir(
    const unsigned char *data,
    size_t size,
    size_t base,
    uint32_t resource_size,
    uint32_t dir_rel,
    int level,
    uint32_t type,
    uint32_t name,
    const test_section *sections,
    size_t section_count,
    test_resource_list *list
) {
    size_t dir = base + dir_rel;
    uint32_t count;
    uint32_t i;
    if (dir_rel + 16u > resource_size || dir + 16u > size) return 0;
    count = (uint32_t)read_u16(data, size, dir + 12u) + (uint32_t)read_u16(data, size, dir + 14u);
    for (i = 0u; i < count; i++) {
        size_t entry = dir + 16u + (size_t)i * 8u;
        uint32_t id_raw = read_u32(data, size, entry);
        uint32_t child_raw = read_u32(data, size, entry + 4u);
        uint32_t id;
        if ((id_raw & 0x80000000u) != 0u) return 0;
        id = id_raw & 0xffffu;
        if (level < 2) {
            if ((child_raw & 0x80000000u) == 0u) return 0;
            if (!parse_resource_dir(
                data,
                size,
                base,
                resource_size,
                child_raw & 0x7fffffffu,
                level + 1,
                level == 0 ? id : type,
                level == 1 ? id : name,
                sections,
                section_count,
                list
            )) return 0;
        } else {
            uint32_t data_entry = child_raw & 0x7fffffffu;
            uint32_t data_rva;
            uint32_t data_size;
            size_t data_off;
            if ((child_raw & 0x80000000u) != 0u) return 0;
            if (data_entry + 16u > resource_size) return 0;
            data_rva = read_u32(data, size, base + data_entry);
            data_size = read_u32(data, size, base + data_entry + 4u);
            data_off = rva_to_offset(sections, section_count, data_rva);
            if (data_off == (size_t)-1 || data_off + data_size > size || list->count >= 128u) return 0;
            list->items[list->count].type = type;
            list->items[list->count].name = name;
            list->items[list->count].language = id;
            list->items[list->count].data = data + data_off;
            list->items[list->count].size = data_size;
            list->count++;
        }
    }
    return 1;
}

static int parse_resources(const unsigned char *data, size_t size, test_resource_list *list) {
    uint32_t nt;
    uint16_t sections_count;
    uint16_t optional_size;
    uint32_t optional;
    uint16_t magic;
    uint32_t data_dir;
    uint32_t resource_rva;
    uint32_t resource_size;
    uint32_t section_table;
    test_section sections[16];
    size_t base;
    size_t i;
    memset(list, 0, sizeof(*list));
    if (size < 0x100u || read_u16(data, size, 0u) != 0x5a4du) return 0;
    nt = read_u32(data, size, 0x3cu);
    if (nt + 24u > size || memcmp(data + nt, "PE\0\0", 4u) != 0) return 0;
    sections_count = read_u16(data, size, nt + 6u);
    optional_size = read_u16(data, size, nt + 20u);
    optional = nt + 24u;
    magic = read_u16(data, size, optional);
    data_dir = magic == 0x20bu ? 112u : 96u;
    resource_rva = read_u32(data, size, optional + data_dir + 16u);
    resource_size = read_u32(data, size, optional + data_dir + 20u);
    section_table = optional + optional_size;
    if (sections_count > 16u || section_table + (uint32_t)sections_count * 40u > size) return 0;
    for (i = 0u; i < sections_count; i++) {
        size_t off = section_table + i * 40u;
        sections[i].virtual_size = read_u32(data, size, off + 8u);
        sections[i].virtual_address = read_u32(data, size, off + 12u);
        sections[i].raw_size = read_u32(data, size, off + 16u);
        sections[i].raw_ptr = read_u32(data, size, off + 20u);
    }
    base = rva_to_offset(sections, sections_count, resource_rva);
    if (base == (size_t)-1 || base + resource_size > size) return 0;
    return parse_resource_dir(data, size, base, resource_size, 0u, 0, 0u, 0u, sections, sections_count, list);
}

static const test_resource *find_resource(const test_resource_list *list, uint32_t type, uint32_t name, uint32_t language) {
    size_t i;
    for (i = 0u; i < list->count; i++) {
        if (list->items[i].type == type && list->items[i].name == name && list->items[i].language == language) {
            return &list->items[i];
        }
    }
    return NULL;
}

static int string_block_equals(const test_resource *resource, uint32_t id, const char *expected) {
    size_t index = id % 16u;
    size_t offset = 0u;
    size_t i;
    for (i = 0u; i < 16u; i++) {
        uint16_t length;
        size_t j;
        if (offset + 2u > resource->size) return 0;
        length = read_u16(resource->data, resource->size, offset);
        offset += 2u;
        if (offset + (size_t)length * 2u > resource->size) return 0;
        if (i == index) {
            if (strlen(expected) != length) return 0;
            for (j = 0u; j < length; j++) {
                if (read_u16(resource->data, resource->size, offset + j * 2u) != (unsigned char)expected[j]) return 0;
            }
            return 1;
        }
        offset += (size_t)length * 2u;
    }
    return 0;
}

static int contains_utf16_ascii(const test_resource *resource, const char *text) {
    size_t len = strlen(text);
    size_t offset;
    if (len == 0u) return 1;
    for (offset = 0u; offset + len * 2u <= resource->size; offset += 2u) {
        size_t i;
        int matched = 1;
        for (i = 0u; i < len; i++) {
            if (read_u16(resource->data, resource->size, offset + i * 2u) != (unsigned char)text[i]) {
                matched = 0;
                break;
            }
        }
        if (matched) return 1;
    }
    return 0;
}

static int write_test_assets(void) {
    static const unsigned char icon[] = {
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x10, 0x10, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x16, 0x00, 0x00, 0x00,
        'I', 'C', 'O', 'N', 'D', 'A', 'T', 'A'
    };
    static const char manifest[] = "<assembly manifestVersion=\"1.0\"></assembly>";
    static const char json[] =
        "{\n"
        "  \"strings\": [{ \"id\": 2, \"language\": 1033, \"value\": \"NewString\" }],\n"
        "  \"version\": {\n"
        "    \"language\": 1033,\n"
        "    \"codePage\": 1200,\n"
        "    \"fixed\": { \"fileVersion\": \"1.2.3.4\", \"fileType\": \"app\" },\n"
        "    \"strings\": { \"CompanyName\": \"NewCo\", \"ProductName\": \"NewProduct\" }\n"
        "  },\n"
        "  \"icons\": [{ \"id\": 1, \"language\": 1033, \"path\": \"assets/app.ico\" }],\n"
        "  \"raw\": [{ \"type\": 24, \"id\": 1, \"language\": 1033, \"path\": \"assets/manifest.xml\" }]\n"
        "}\n";
    static const char bad_json[] = "{ \"schema\": \"nope\" }\n";
    static const char duplicate_json[] =
        "{ \"strings\": ["
        "{ \"id\": 1, \"language\": 1033, \"value\": \"A\" },"
        "{ \"id\": 1, \"language\": 1033, \"value\": \"B\" }"
        "] }\n";
    static const char big_json[] =
        "{ \"raw\": [{ \"type\": 25, \"id\": 2, \"language\": 1033, \"path\": \"assets/big.bin\" }] }\n";
    unsigned char big[3000];
    size_t i;
    for (i = 0u; i < sizeof(big); i++) big[i] = (unsigned char)(i & 0xffu);
    CHECK(write_file("build/test-run/assets/app.ico", icon, sizeof(icon)));
    CHECK(write_file("build/test-run/assets/manifest.xml", manifest, sizeof(manifest) - 1u));
    CHECK(write_file("build/test-run/assets/big.bin", big, sizeof(big)));
    CHECK(write_file("build/test-run/update.json", json, strlen(json)));
    CHECK(write_file("build/test-run/bad.json", bad_json, strlen(bad_json)));
    CHECK(write_file("build/test-run/duplicate.json", duplicate_json, strlen(duplicate_json)));
    CHECK(write_file("build/test-run/big.json", big_json, strlen(big_json)));
    return 0;
}

static int seed_pe(const char *input, const char *output) {
    eg_pe_file *file = NULL;
    eg_resource_update *update = NULL;
    eg_resource_key key;
    eg_version_fixed_update fixed;
    eg_result result;
    static const unsigned char keep_data[] = { 'k', 'e', 'e', 'p' };
    memset(&key, 0, sizeof(key));
    memset(&fixed, 0, sizeof(fixed));
    result = eg_resource_update_create(&update);
    CHECK(result == EG_OK);
    result = eg_resource_update_set_string_utf8(update, 1u, 1033u, "KeepMe");
    CHECK(result == EG_OK);
    result = eg_resource_update_set_version_string_utf8(update, 1033u, 1200u, "CompanyName", "OldCo");
    CHECK(result == EG_OK);
    result = eg_resource_update_set_version_string_utf8(update, 1033u, 1200u, "FileDescription", "OldDesc");
    CHECK(result == EG_OK);
    fixed.has_product_version = 1;
    fixed.product_version.major = 9u;
    fixed.product_version.minor = 8u;
    fixed.product_version.patch = 7u;
    fixed.product_version.build = 6u;
    result = eg_resource_update_set_version_fixed(update, 1033u, &fixed);
    CHECK(result == EG_OK);
    key.type.id = 10u;
    key.name.id = 77u;
    key.language = 1033u;
    result = eg_resource_update_set_data(update, &key, keep_data, sizeof(keep_data));
    CHECK(result == EG_OK);
    result = eg_pe_open_file(input, &file);
    CHECK(result == EG_OK);
    result = eg_pe_write_file(file, update, output);
    CHECK(result == EG_OK);
    eg_pe_close(file);
    eg_resource_update_destroy(update);
    return 0;
}

static int test_cli_update_preserves_resources(void) {
    test_buffer output;
    test_resource_list list;
    const test_resource *resource;
    uint16_t icon_id;
    memset(&output, 0, sizeof(output));
    CHECK(create_minimal_pe("build/test-run/base32.exe", 0, 0x400u));
    CHECK(seed_pe("build/test-run/base32.exe", "build/test-run/seeded.exe") == 0);
    CHECK(system("build/engraver build/test-run/seeded.exe build/test-run/update.json build/test-run/out.exe") == 0);
    CHECK(read_file("build/test-run/out.exe", &output));
    CHECK(parse_resources(output.data, output.size, &list));
    resource = find_resource(&list, 10u, 77u, 1033u);
    CHECK(resource != NULL);
    CHECK(resource->size == 4u && memcmp(resource->data, "keep", 4u) == 0);
    resource = find_resource(&list, 6u, 1u, 1033u);
    CHECK(resource != NULL);
    CHECK(string_block_equals(resource, 1u, "KeepMe"));
    CHECK(string_block_equals(resource, 2u, "NewString"));
    resource = find_resource(&list, 16u, 1u, 1033u);
    CHECK(resource != NULL);
    CHECK(contains_utf16_ascii(resource, "CompanyName"));
    CHECK(contains_utf16_ascii(resource, "NewCo"));
    CHECK(contains_utf16_ascii(resource, "FileDescription"));
    CHECK(contains_utf16_ascii(resource, "OldDesc"));
    CHECK(contains_utf16_ascii(resource, "ProductName"));
    CHECK(contains_utf16_ascii(resource, "NewProduct"));
    resource = find_resource(&list, 24u, 1u, 1033u);
    CHECK(resource != NULL);
    CHECK(contains_utf16_ascii(resource, "") && resource->size > 20u);
    resource = find_resource(&list, 14u, 1u, 1033u);
    CHECK(resource != NULL);
    CHECK(resource->size == 20u);
    icon_id = read_u16(resource->data, resource->size, 18u);
    resource = find_resource(&list, 3u, icon_id, 1033u);
    CHECK(resource != NULL);
    CHECK(resource->size == 8u && memcmp(resource->data, "ICONDATA", 8u) == 0);
    free(output.data);
    return 0;
}

static int test_pe64_raw_update(void) {
    eg_json_document *document = NULL;
    eg_resource_update *update = NULL;
    eg_pe_file *file = NULL;
    test_buffer output;
    test_resource_list list;
    const test_resource *resource;
    eg_result result;
    memset(&output, 0, sizeof(output));
    CHECK(create_minimal_pe("build/test-run/base64.exe", 1, 0x400u));
    result = eg_load_json_file("build/test-run/update.json", &document);
    CHECK(result == EG_OK);
    result = eg_json_document_to_update(document, &update);
    CHECK(result == EG_OK);
    result = eg_pe_open_file("build/test-run/base64.exe", &file);
    CHECK(result == EG_OK);
    result = eg_pe_write_file(file, update, "build/test-run/out64.exe");
    CHECK(result == EG_OK);
    eg_pe_close(file);
    eg_resource_update_destroy(update);
    eg_release_json(document);
    CHECK(read_file("build/test-run/out64.exe", &output));
    CHECK(parse_resources(output.data, output.size, &list));
    resource = find_resource(&list, 24u, 1u, 1033u);
    CHECK(resource != NULL);
    resource = find_resource(&list, 14u, 1u, 1033u);
    CHECK(resource != NULL);
    free(output.data);
    return 0;
}

static int test_large_update_moves_to_new_resource_section(void) {
    test_buffer output;
    test_resource_list list;
    const test_resource *resource;
    size_t i;
    memset(&output, 0, sizeof(output));
    CHECK(system("build/engraver build/test-run/seeded.exe build/test-run/big.json build/test-run/big-out.exe") == 0);
    CHECK(read_file("build/test-run/big-out.exe", &output));
    CHECK(parse_resources(output.data, output.size, &list));
    resource = find_resource(&list, 25u, 2u, 1033u);
    CHECK(resource != NULL);
    CHECK(resource->size == 3000u);
    for (i = 0u; i < resource->size; i++) CHECK(resource->data[i] == (unsigned char)(i & 0xffu));
    resource = find_resource(&list, 10u, 77u, 1033u);
    CHECK(resource != NULL);
    CHECK(resource->size == 4u && memcmp(resource->data, "keep", 4u) == 0);
    free(output.data);
    return 0;
}

static int test_custom_io_ops_update_flow(void) {
    test_buffer update_json;
    test_buffer icon;
    test_buffer manifest;
    test_buffer seeded;
    test_memory_file files[5];
    test_memory_fs fs;
    eg_io_ops ops;
    eg_json_document *document = NULL;
    eg_resource_update *update = NULL;
    eg_pe_file *file = NULL;
    test_resource_list list;
    const test_resource *resource;
    eg_result result;
    memset(&update_json, 0, sizeof(update_json));
    memset(&icon, 0, sizeof(icon));
    memset(&manifest, 0, sizeof(manifest));
    memset(&seeded, 0, sizeof(seeded));
    memset(files, 0, sizeof(files));
    memset(&fs, 0, sizeof(fs));
    CHECK(read_file("build/test-run/update.json", &update_json));
    CHECK(read_file("build/test-run/assets/app.ico", &icon));
    CHECK(read_file("build/test-run/assets/manifest.xml", &manifest));
    CHECK(read_file("build/test-run/seeded.exe", &seeded));
    files[0].path = "mem/update.json";
    files[0].read_data = update_json.data;
    files[0].read_size = update_json.size;
    files[1].path = "mem/assets/app.ico";
    files[1].read_data = icon.data;
    files[1].read_size = icon.size;
    files[2].path = "mem/assets/manifest.xml";
    files[2].read_data = manifest.data;
    files[2].read_size = manifest.size;
    files[3].path = "mem/seeded.exe";
    files[3].read_data = seeded.data;
    files[3].read_size = seeded.size;
    files[4].path = "mem/out.exe";
    fs.files = files;
    fs.file_count = sizeof(files) / sizeof(files[0]);
    ops = memory_ops(&fs);
    result = eg_load_json_file_with_io(&ops, "mem/update.json", &document);
    CHECK(result == EG_OK);
    result = eg_json_document_to_update_with_io(&ops, document, &update);
    CHECK(result == EG_OK);
    result = eg_pe_open_file_with_io(&ops, "mem/seeded.exe", &file);
    CHECK(result == EG_OK);
    result = eg_pe_write_file_with_io(&ops, file, update, "mem/out.exe");
    CHECK(result == EG_OK);
    CHECK(files[4].write_data != NULL);
    CHECK(parse_resources(files[4].write_data, files[4].write_size, &list));
    resource = find_resource(&list, 6u, 1u, 1033u);
    CHECK(resource != NULL);
    CHECK(string_block_equals(resource, 1u, "KeepMe"));
    CHECK(string_block_equals(resource, 2u, "NewString"));
    resource = find_resource(&list, 24u, 1u, 1033u);
    CHECK(resource != NULL);
    resource = find_resource(&list, 14u, 1u, 1033u);
    CHECK(resource != NULL);
    eg_pe_close(file);
    eg_resource_update_destroy(update);
    eg_release_json(document);
    memory_files_free(files, sizeof(files) / sizeof(files[0]));
    free(update_json.data);
    free(icon.data);
    free(manifest.data);
    free(seeded.data);
    return 0;
}

static int test_custom_io_error_cases(void) {
    test_buffer update_json;
    test_buffer seeded;
    test_memory_file files[3];
    test_memory_fs fs;
    eg_io_ops ops;
    eg_io_ops invalid_ops;
    eg_json_document *document = NULL;
    eg_resource_update *update = NULL;
    eg_pe_file *file = NULL;
    eg_result result;
    memset(&update_json, 0, sizeof(update_json));
    memset(&seeded, 0, sizeof(seeded));
    memset(files, 0, sizeof(files));
    memset(&fs, 0, sizeof(fs));
    CHECK(read_file("build/test-run/update.json", &update_json));
    CHECK(read_file("build/test-run/seeded.exe", &seeded));
    files[0].path = "mem/update.json";
    files[0].read_data = update_json.data;
    files[0].read_size = update_json.size;
    files[1].path = "mem/seeded.exe";
    files[1].read_data = seeded.data;
    files[1].read_size = seeded.size;
    files[2].path = "mem/out.exe";
    fs.files = files;
    fs.file_count = sizeof(files) / sizeof(files[0]);
    ops = memory_ops(&fs);
    invalid_ops = ops;
    invalid_ops.read = NULL;
    result = eg_load_json_file_with_io(NULL, "mem/update.json", &document);
    CHECK(result == EG_ERROR_INVALID_ARGUMENT);
    result = eg_load_json_file_with_io(&invalid_ops, "mem/update.json", &document);
    CHECK(result == EG_ERROR_INVALID_ARGUMENT);
    result = eg_json_document_to_update_with_io(NULL, &(eg_json_document){0}, &update);
    CHECK(result == EG_ERROR_INVALID_ARGUMENT);
    result = eg_pe_open_file_with_io(NULL, "mem/seeded.exe", &file);
    CHECK(result == EG_ERROR_INVALID_ARGUMENT);
    result = eg_pe_write_file_with_io(NULL, (const eg_pe_file *)1, NULL, "mem/out.exe");
    CHECK(result == EG_ERROR_INVALID_ARGUMENT);
    fs.fail_open_read_path = "mem/update.json";
    result = eg_load_json_file_with_io(&ops, "mem/update.json", &document);
    CHECK(result == EG_ERROR_IO);
    fs.fail_open_read_path = NULL;
    fs.fail_read_path = "mem/update.json";
    result = eg_load_json_file_with_io(&ops, "mem/update.json", &document);
    CHECK(result == EG_ERROR_IO);
    fs.fail_read_path = NULL;
    result = eg_load_json_file_with_io(&ops, "mem/update.json", &document);
    CHECK(result == EG_OK);
    result = eg_pe_open_file_with_io(&ops, "mem/seeded.exe", &file);
    CHECK(result == EG_OK);
    result = eg_resource_update_create(&update);
    CHECK(result == EG_OK);
    fs.fail_open_write_path = "mem/out.exe";
    result = eg_pe_write_file_with_io(&ops, file, update, "mem/out.exe");
    CHECK(result == EG_ERROR_IO);
    fs.fail_open_write_path = NULL;
    fs.fail_write_path = "mem/out.exe";
    result = eg_pe_write_file_with_io(&ops, file, update, "mem/out.exe");
    CHECK(result == EG_ERROR_IO);
    eg_resource_update_destroy(update);
    eg_pe_close(file);
    eg_release_json(document);
    memory_files_free(files, sizeof(files) / sizeof(files[0]));
    free(update_json.data);
    free(seeded.data);
    return 0;
}

static int test_error_cases(void) {
    eg_json_document *document = NULL;
    eg_resource_update *update = NULL;
    eg_pe_file *file = NULL;
    eg_resource_key key;
    eg_result result;
    static const unsigned char data[] = { 1u, 2u, 3u };
    result = eg_load_json_file("build/test-run/bad.json", &document);
    CHECK(result == EG_ERROR_INVALID_JSON);
    result = eg_load_json_file("build/test-run/duplicate.json", &document);
    CHECK(result == EG_OK);
    result = eg_json_document_to_update(document, &update);
    CHECK(result == EG_ERROR_DUPLICATE_RESOURCE);
    eg_release_json(document);
    memset(&key, 0, sizeof(key));
    result = eg_resource_update_create(&update);
    CHECK(result == EG_OK);
    key.type.id = 1u;
    key.name.id = 1u;
    key.language = 1033u;
    result = eg_resource_update_set_data(update, &key, data, sizeof(data));
    CHECK(result == EG_OK);
    result = eg_resource_update_set_data(update, &key, data, sizeof(data));
    CHECK(result == EG_ERROR_DUPLICATE_RESOURCE);
    eg_resource_update_destroy(update);
    CHECK(create_minimal_pe("build/test-run/tight.exe", 0, 0x1b8u));
    result = eg_resource_update_create(&update);
    CHECK(result == EG_OK);
    result = eg_resource_update_set_data(update, &key, data, sizeof(data));
    CHECK(result == EG_OK);
    result = eg_pe_open_file("build/test-run/tight.exe", &file);
    CHECK(result == EG_OK);
    result = eg_pe_write_file(file, update, "build/test-run/tight-out.exe");
    CHECK(result == EG_ERROR_UNSUPPORTED_PE);
    eg_pe_close(file);
    eg_resource_update_destroy(update);
    CHECK(system("build/engraver build/test-run/base32.exe build/test-run/update.json build/test-run/base32.exe >/dev/null 2>/dev/null") != 0);
    return 0;
}

int main(void) {
    CHECK(ensure_dir("build/test-run"));
    CHECK(ensure_dir("build/test-run/assets"));
    CHECK(write_test_assets() == 0);
    CHECK(test_cli_update_preserves_resources() == 0);
    CHECK(test_pe64_raw_update() == 0);
    CHECK(test_large_update_moves_to_new_resource_section() == 0);
    CHECK(test_custom_io_ops_update_flow() == 0);
    CHECK(test_custom_io_error_cases() == 0);
    CHECK(test_error_cases() == 0);
    return 0;
}
