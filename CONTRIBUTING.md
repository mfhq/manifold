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

## Terms

Manifold has a number of concepts that are easy to describe in a roundabout
way. Please use the terms below in comments, messages and documentation.

### Components

* **Component**: anything Manifold installs. It has a component ID, like
  `org.example.myapp`, and a kind. AppStream metadata (AsComponent) describes
  it as well, but AppStream component kinds are different from Manifold component kinds.
* **Kind**: what a component is. A *core* is the root filesystem. A *platform*
  is a set of libraries that many applications share. A *kit* holds the
  packages one or more applications need beyond the layer below. An *app* is
  an application, and a *plugin* extends an application.
* **Layer**: the position of a component in a stack. From the bottom to the
  top, the layers are core, platform, kit, app and plugin.
* **Runtime**: a core, a platform or a kit. The term also covers SDKs when it
  is about the store, where SDKs are kept in the `runtimes` directory.
* **SDK**: a component with the development files of one layer. It is an
  image of its own, which is layered on the runtime of the same layer.
* **SDK stack**: the runtimes of an application together with the SDK of
  every layer.
* **Version** and **revision**: the version is given by the publisher, and
  the revision tells rebuilds of the same version apart. The **full version**
  is both in one string, `<version>-<revision>`.
* **Image**: the file with the contents of one version of a component, an
  EROFS filesystem protected by dm-verity.
* **Manifest**: the JSON document that describes an image.
* **Bundle**: a `.manifold` file that contains one or more images with their
  manifests and catalog data.

### Relations between components

* **Base**: the component that an image was built on, which is the layer
  directly below it. Only a core has no base.
* **Base SDK**: the SDK of the layer below, for an SDK that extends a
  platform, a kit or an application.
* **Reference**: the entry of a manifest that names a base or a base SDK,
  with its component ID, kind, version, revision, identity and content hash.
* **Exact base**: a base that is required in the version and revision the
  reference names. A platform and every SDK have an exact base. A base SDK is
  always exact.
* **Identity**: a hash that says what a component provides to the layers
  above it. A base that is not exact can be replaced by any version with the
  same identity.
* **Content hash**: a hash that is unique to one (reproducible!) build of an image.
* **Verity hash**: the dm-verity root hash of an image. The store names
  images by this hash.
* **Dependency**: a component that is only installed because another
  component needs it.

### The store

* **Store**: the directory with everything that is installed,
  `/var/lib/manifold`.
* **Version directory**: the directory of one installed version of a
  component, named by its full version. It contains the image manifest and some other data.
* **Current version**: the version that the `current` link of a component
  points to, which is the version that is used.
* **Install mode**: *manual* for a component that is kept until someone
  removes it, *auto* for a component that is removed once nothing needs it
  anymore.
* **Origin**: where a component was installed from. This is `<remote>-<channel>`
  for a repository, and `manifold-local` for a local bundle with no repository.
* **Integration**: the files of an application that are installed on the
  host, like desktop entries, icons, systemd units and udev rules. The copies
  on the host are called **exports**.
* **Privileged**: a component is privileged if its integration includes
  system services or udev rules, as those give it root access to the host.

### Repositories

* **Repository**: a set of components that a publisher serves over HTTPS.
* **Remote**: a repository that is configured on the host, with the keys it
  trusts.
* **Channel**: a named part of a repository, like `main` or `beta`.
* **Root index**: the signed file that lists the channels of a repository,
  and that every other file of the repository is ultimately checked against.
* **Cached metadata**: the copy of the indices of a remote that the last
  refresh stored.
* **Catalog**: the AppStream data that describes the components of an origin.

### Operations

* **Install set**: the components that one operation installs. Each of them
  is an **item** of the set.
* **Install plan**: the result of checking one item, which says how the item
  will be installed.
* **Removal plan**: the components and images that one operation will
  remove.
* **Superseded**: an older version that an update replaces, and that is
  removed with the update.
* **Cleanup**: removing components with the install mode *auto* that nothing
  needs anymore, and images that no component uses.
