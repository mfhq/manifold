# Contributing to Manifold

Patches, pull-requests, and bug reports are very welcome! This file describes
the conventions of the code, so your changes fit in with the rest of the tree.

## Formatting

Manifold is written in C23. The tree is auto-formatted, so before submitting
a change, run:

```sh
./autoformat.py
```

## Types

We use the standard C types wherever one exists: `char`, `int`, `long`,
`double`, `size_t`, `ssize_t`, `off_t` and the fixed-width types of
`<stdint.h>` (`uint8_t`, `int64_t`, etc.).

A few GLib types stay, as they are either shorter or part of the GLib API
contract:

* `gboolean` for every boolean in code that uses GLib. It is an `int`, while
  `bool` is usually a single byte, so passing a `bool *` to a GLib function
  that writes a `gboolean` corrupts memory.
* `guint`, as it is a lot shorter than `unsigned int`, and it is what GLib
  uses for lengths and IDs (`GPtrArray->len`, source and signal IDs).
* `gpointer` and `gconstpointer`.
* `guchar`, where a GLib function expects it (e.g. `g_checksum_update()`).

Some values need the GLib type even though a standard type exists:

* If GLib writes to a variable through a pointer, like `g_bytes_get_data()`,
  `g_file_get_contents()` or `g_ascii_string_to_unsigned()` do, declare the
  variable with the type GLib uses (`gsize`, `guint64`).
* GObject-Introspection annotations use the GIR type names, so write
  `(element-type guint64)`, not `uint64_t`.

Code that implements or calls a non-GLib API uses the types that API declares.
An sd-event callback takes a `uint64_t usec` and a `void *userdata`, a field
filled by `sd_json_dispatch_stdbool()` is a `bool`, `read()` returns a
`ssize_t` and the flags of `mount()` are an `unsigned long`. Programs that do
not link against GLib at all use plain C types throughout, including `bool`
and `unsigned int`.

For format strings, use the macros of `<inttypes.h>` (`PRIu64`, `PRId64`, ...)
and `%zu` for a `size_t`, and use `UINT64_MAX`, `SIZE_MAX` or `INT_MAX` for
limits instead of the `G_MAX*` macros (`G_MAXUINT` is fine for a `guint`).
Please do not use `G_GUINT64_FORMAT` and its siblings: xgettext can not
expand them in translatable strings, while it understands `PRIu64`.
