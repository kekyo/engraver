#include "engraver.h"

#include <stdio.h>
#include <string.h>

static int run_engraver(const char *input_path, const char *json_path, const char *output_path) {
    eg_json_document *document = NULL;
    eg_resource_update *update = NULL;
    eg_pe_file *file = NULL;
    eg_result result;
    if (strcmp(input_path, output_path) == 0) {
        fprintf(stderr, "engraver: input and output paths must differ\n");
        return 2;
    }
    result = eg_load_json_file(json_path, &document);
    if (result != EG_OK) {
        fprintf(stderr, "engraver: failed to load JSON: %s\n", eg_result_string(result));
        return 1;
    }
    result = eg_json_document_to_update(document, &update);
    if (result != EG_OK) {
        fprintf(stderr, "engraver: failed to create update: %s\n", eg_result_string(result));
        eg_release_json(document);
        return 1;
    }
    result = eg_pe_open_file(input_path, &file);
    if (result != EG_OK) {
        fprintf(stderr, "engraver: failed to open PE: %s\n", eg_result_string(result));
        eg_resource_update_destroy(update);
        eg_release_json(document);
        return 1;
    }
    result = eg_pe_write_file(file, update, output_path);
    eg_pe_close(file);
    eg_resource_update_destroy(update);
    eg_release_json(document);
    if (result != EG_OK) {
        fprintf(stderr, "engraver: failed to write PE: %s\n", eg_result_string(result));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: engraver <input-pe> <updates-json> <output-pe>\n");
        return 2;
    }
    return run_engraver(argv[1], argv[2], argv[3]);
}
