/* engraver - Edits resources in existing Win32 PE files
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#ifndef ENGRAVER_H
#define ENGRAVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result codes returned by libengraver functions.
 */
typedef enum eg_result {
    EG_OK = 0,
    EG_ERROR_INVALID_ARGUMENT,
    EG_ERROR_IO,
    EG_ERROR_INVALID_JSON,
    EG_ERROR_INVALID_PE,
    EG_ERROR_UNSUPPORTED_PE,
    EG_ERROR_INVALID_RESOURCE,
    EG_ERROR_DUPLICATE_RESOURCE,
    EG_ERROR_OUT_OF_MEMORY
} eg_result;

/**
 * File I/O operations used by the `_with_io` APIs.
 *
 * @remarks `read()` reports EOF by setting `out_size` to 0 and returning
 * EG_OK. `write()` must write the full requested size or return an error.
 * `close()` is called after a successful open, including when a later read or
 * write operation fails.
 */
typedef struct eg_io_ops {
    void *context;
    eg_result (*open_read)(void *context, const char *path, void **out_handle);
    eg_result (*open_write)(void *context, const char *path, void **out_handle);
    eg_result (*read)(void *context, void *handle, void *buffer, size_t size, size_t *out_size);
    eg_result (*write)(void *context, void *handle, const void *buffer, size_t size);
    eg_result (*close)(void *context, void *handle);
} eg_io_ops;

/**
 * Opaque handle for a loaded PE image.
 */
typedef struct eg_pe_file eg_pe_file;

/**
 * Opaque collection of resource updates.
 */
typedef struct eg_resource_update eg_resource_update;

/**
 * Numeric or UTF-16 resource identifier.
 *
 * @remarks v1 JSON input accepts numeric identifiers only. The string form is
 * available so existing named resources can be preserved and low-level callers
 * can address them directly.
 */
typedef struct eg_resource_id {
    int is_string;
    uint32_t id;
    const uint16_t *name_utf16;
    size_t name_length;
} eg_resource_id;

/**
 * Full PE resource key: type, name, and language.
 */
typedef struct eg_resource_key {
    eg_resource_id type;
    eg_resource_id name;
    uint16_t language;
} eg_resource_key;

/**
 * Four-part version number.
 */
typedef struct eg_version_quad {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t build;
} eg_version_quad;

/**
 * Partial update for VS_FIXEDFILEINFO fields.
 */
typedef struct eg_version_fixed_update {
    int has_file_version;
    eg_version_quad file_version;
    int has_product_version;
    eg_version_quad product_version;
    int has_file_flags;
    uint32_t file_flags;
    int has_file_os;
    uint32_t file_os;
    int has_file_type;
    uint32_t file_type;
} eg_version_fixed_update;

/**
 * JSON string resource update entry.
 */
typedef struct eg_json_string_update {
    uint32_t id;
    uint16_t language;
    const char *value_utf8;
} eg_json_string_update;

/**
 * JSON icon resource update entry.
 */
typedef struct eg_json_icon_update {
    uint32_t id;
    uint16_t language;
    const char *path;
} eg_json_icon_update;

/**
 * JSON raw resource update entry.
 */
typedef struct eg_json_raw_update {
    uint32_t type;
    uint32_t id;
    uint16_t language;
    const char *path;
} eg_json_raw_update;

/**
 * JSON version string update entry.
 */
typedef struct eg_json_version_string_update {
    const char *key_utf8;
    const char *value_utf8;
} eg_json_version_string_update;

/**
 * JSON version resource update entry.
 *
 * @remarks The resource name is fixed to 1, which is the conventional
 * RT_VERSION resource name used by Windows version resources.
 */
typedef struct eg_json_version_update {
    uint16_t language;
    uint16_t code_page;
    int has_code_page;
    eg_version_fixed_update fixed;
    const eg_json_version_string_update *strings;
    size_t string_count;
} eg_json_version_update;

/**
 * Parsed JSON document used by eg_json_document_to_update().
 *
 * @remarks This structure may be zero-initialized and manually filled by a
 * caller. A document returned by eg_load_json_file() owns its strings and arrays
 * and must be released with eg_release_json(). Manually constructed documents
 * must not be passed to eg_release_json().
 */
typedef struct eg_json_document {
    const char *base_dir;
    const eg_json_string_update *strings;
    size_t string_count;
    const eg_json_icon_update *icons;
    size_t icon_count;
    const eg_json_raw_update *raw;
    size_t raw_count;
    const eg_json_version_update *version;
} eg_json_document;

/**
 * Read and parse an engraver JSON update file.
 *
 * @param path JSON file path.
 * @param out_document Receives an owned document on success.
 * @return Result code.
 */
eg_result eg_load_json_file(const char *path, eg_json_document **out_document);

/**
 * Read and parse an engraver JSON update file using custom I/O.
 *
 * @param ops Custom I/O operations.
 * @param path JSON file path passed to ops.
 * @param out_document Receives an owned document on success.
 * @return Result code.
 */
eg_result eg_load_json_file_with_io(
    const eg_io_ops *ops,
    const char *path,
    eg_json_document **out_document
);

/**
 * Convert a parsed or manually constructed JSON document to resource updates.
 *
 * @param document JSON document.
 * @param out_update Receives an owned update object on success.
 * @return Result code.
 */
eg_result eg_json_document_to_update(const eg_json_document *document, eg_resource_update **out_update);

/**
 * Convert a JSON document to resource updates using custom I/O for assets.
 *
 * @param ops Custom I/O operations.
 * @param document JSON document.
 * @param out_update Receives an owned update object on success.
 * @return Result code.
 */
eg_result eg_json_document_to_update_with_io(
    const eg_io_ops *ops,
    const eg_json_document *document,
    eg_resource_update **out_update
);

/**
 * Release a document returned by eg_load_json_file().
 *
 * @param document Owned JSON document or NULL.
 */
void eg_release_json(eg_json_document *document);

/**
 * Load a PE file from disk.
 *
 * @param path PE file path.
 * @param out_file Receives an owned PE handle on success.
 * @return Result code.
 */
eg_result eg_pe_open_file(const char *path, eg_pe_file **out_file);

/**
 * Load a PE file using custom I/O.
 *
 * @param ops Custom I/O operations.
 * @param path PE file path passed to ops.
 * @param out_file Receives an owned PE handle on success.
 * @return Result code.
 */
eg_result eg_pe_open_file_with_io(
    const eg_io_ops *ops,
    const char *path,
    eg_pe_file **out_file
);

/**
 * Write a PE image with resource updates applied.
 *
 * @param file Loaded PE file.
 * @param update Resource updates to apply.
 * @param output_path Destination path. v1 does not support in-place output.
 * @return Result code.
 */
eg_result eg_pe_write_file(const eg_pe_file *file, const eg_resource_update *update, const char *output_path);

/**
 * Write a PE image with resource updates applied using custom I/O.
 *
 * @param ops Custom I/O operations.
 * @param file Loaded PE file.
 * @param update Resource updates to apply.
 * @param output_path Destination path passed to ops.
 * @return Result code.
 */
eg_result eg_pe_write_file_with_io(
    const eg_io_ops *ops,
    const eg_pe_file *file,
    const eg_resource_update *update,
    const char *output_path
);

/**
 * Release a loaded PE file.
 *
 * @param file PE handle or NULL.
 */
void eg_pe_close(eg_pe_file *file);

/**
 * Create an empty resource update collection.
 *
 * @param out_update Receives an owned update collection on success.
 * @return Result code.
 */
eg_result eg_resource_update_create(eg_resource_update **out_update);

/**
 * Release a resource update collection.
 *
 * @param update Update collection or NULL.
 */
void eg_resource_update_destroy(eg_resource_update *update);

/**
 * Add or replace a raw resource by full resource key.
 *
 * @param update Update collection.
 * @param key Resource key.
 * @param data Resource bytes.
 * @param size Resource byte length.
 * @return Result code.
 */
eg_result eg_resource_update_set_data(
    eg_resource_update *update,
    const eg_resource_key *key,
    const void *data,
    size_t size
);

/**
 * Add or replace one RT_STRING entry.
 *
 * @param update Update collection.
 * @param id Logical string identifier.
 * @param language Windows LANGID.
 * @param value UTF-8 string value.
 * @return Result code.
 */
eg_result eg_resource_update_set_string_utf8(
    eg_resource_update *update,
    uint32_t id,
    uint16_t language,
    const char *value
);

/**
 * Add or replace one version string key.
 *
 * @param update Update collection.
 * @param language Windows LANGID.
 * @param code_page Version string table code page.
 * @param key Version string key.
 * @param value UTF-8 string value.
 * @return Result code.
 */
eg_result eg_resource_update_set_version_string_utf8(
    eg_resource_update *update,
    uint16_t language,
    uint16_t code_page,
    const char *key,
    const char *value
);

/**
 * Add or replace selected VS_FIXEDFILEINFO fields.
 *
 * @param update Update collection.
 * @param language Windows LANGID.
 * @param fixed Partial fixed-info update.
 * @return Result code.
 */
eg_result eg_resource_update_set_version_fixed(
    eg_resource_update *update,
    uint16_t language,
    const eg_version_fixed_update *fixed
);

/**
 * Add or replace an icon group from .ico data.
 *
 * @param update Update collection.
 * @param group_id RT_GROUP_ICON resource identifier.
 * @param language Windows LANGID.
 * @param data ICO file bytes.
 * @param size ICO file byte length.
 * @return Result code.
 */
eg_result eg_resource_update_set_icon_from_ico_data(
    eg_resource_update *update,
    uint32_t group_id,
    uint16_t language,
    const void *data,
    size_t size
);

/**
 * Convert a result code to a stable English string.
 *
 * @param result Result code.
 * @return Static string for diagnostics.
 */
const char *eg_result_string(eg_result result);

#ifdef __cplusplus
}
#endif

#endif
