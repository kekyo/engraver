# engraver

Edits resources in existing Win32 PE files.

---

## What Is This?

In a POSIX environment, it is common in porting projects for developers to want to embed resources into Windows PE binary files.
Examples include updating version information or setting icons.

However, the development environment must have a toolchain such as `windres` installed.

While there are other libraries for manipulating the PE binary format,
they can be cumbersome to use—even when you only want to manipulate resources—because they involve a large API.

engraver is a small library implemented entirely in C99 that
specializes solely in editing the resource information of PE binary files after the fact.
You can easily integrate it into your project.

The project contains:

- `libengraver`: C99 library published as `libengraver.a` and `libengraver.so`
- `engraver`: small CLI wrapper around the library

## Build

```sh
make clean all test
```

The build creates:

- `build/libengraver.a`
- `build/libengraver.so`
- `build/engraver`

## CLI

```sh
build/engraver <input-pe> <updates-json> <output-pe>
```

The input and output paths must differ. Resources not mentioned by JSON are preserved.

## JSON

The JSON root object may contain `strings`, `version`, `icons`, and `raw`.

```json
{
  "strings": [{ "id": 1, "language": 1041, "value": "Title" }],
  "version": {
    "language": 1041,
    "codePage": 1200,
    "fixed": {
      "fileVersion": "1.2.3.4",
      "productVersion": "1.2.3.4",
      "fileType": "app"
    },
    "strings": {
      "CompanyName": "Example",
      "FileDescription": "Example App"
    }
  },
  "icons": [{ "id": 1, "language": 1041, "path": "assets/app.ico" }],
  "raw": [{ "type": 24, "id": 1, "language": 1041, "path": "assets/manifest.xml" }]
}
```

Asset paths are resolved relative to the JSON file directory. v1 accepts numeric `id`, `type`, and `language` values. Icon paths must point to `.ico` files.

## Library Flow

```c
eg_json_document *document = NULL;
eg_resource_update *update = NULL;
eg_pe_file *file = NULL;

eg_load_json_file("updates.json", &document);
eg_json_document_to_update(document, &update);
eg_pe_open_file("input.exe", &file);
eg_pe_write_file(file, update, "output.exe");

eg_pe_close(file);
eg_resource_update_destroy(update);
eg_release_json(document);
```

The functions above use the default POSIX file I/O implementation.
Use the `_with_io` variants when embedding libengraver in an environment that
needs custom file handles or virtual files.

```c
eg_io_ops ops = {
    .context = user_context,
    .open_read = open_read,
    .open_write = open_write,
    .read = read_file,
    .write = write_file,
    .close = close_file
};

eg_load_json_file_with_io(&ops, "updates.json", &document);
eg_json_document_to_update_with_io(&ops, document, &update);
eg_pe_open_file_with_io(&ops, "input.exe", &file);
eg_pe_write_file_with_io(&ops, file, update, "output.exe");
```

`read()` reports EOF by returning `EG_OK` with `*out_size == 0`.
`write()` must write the full requested size or return an error.
`close()` is called after a successful open, including when a later read or write fails.

---

## License

Under MIT.
