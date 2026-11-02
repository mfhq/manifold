# Manifold

**Image-based deployment for system-integrated Linux applications.**

Manifold is a deployment format and toolset for Linux applications and plugins
that need the portability of a container (one consistent library environment on
every distribution) as well as the system integration of a native package:
udev rules, systemd services, device access, and (mostly) unrestricted interaction
with the host.

Manifold fills the gap between AppImage (no system integration), Flatpak (too
restrictive for some software) and distribution packages (not portable across
distributions). It is inspired by Steam's Pressure Vessel, and uses a lot of the
same technology that Valve uses for Steam as well.

Manifold's primary audience is vendors of industrial, scientific, and
professional Linux software, such as lab instruments, CNC controllers, image analysis
tools, development environments, and similar applications that don't fit the
consumer desktop app model that Flatpak already serves well.

> [!WARNING]
> Manifold is experimental. Its formats, interfaces, and APIs may still change in
> incompatible ways. A predecessor of this project has been deployed successfully
> in production, but the public version differs significantly from the internal one
> and has so far received much less testing.

## Design principles

### Deployment is not sandboxing

Manifold splits the reproducible/compatible environment problem (consistent libraries
across distributions) from the isolation problem (restricting what an application can
do), and only solves the first. By default, nothing sits between an application and the
system: There is no sandbox, no seccomp filter, and no supervising process. The
application is executed in place. An application that wants isolation is expected to set
it up on its own, using other tools. Manifold might provide sandboxing hooks in future
to make this easier and to allow custom confines to be applied, but if you want a true
sandbox-by-default, you should use Flatpak instead.

### Built on stable distributions

Curating a parallel package ecosystem and tracking dependencies takes an enormous amount
of ongoing work, and Debian, Ubuntu LTS, AlmaLinux, and similar projects already do this
for decades. Manifold builds its runtimes from these distributions (Debian by default),
which keeps build environments familiar and makes application images very quick to build.
Runtime images consist only of distribution packages, and can be reproduced from their
package list. Application vendors target a known stable distribution, and Manifold delivers
the result to whatever distribution the host runs.

### Reuse the ecosystem

Manifold contains no new filesystem, image format, or sandbox. It mostly just combines
well-established existing technology:

- Disk images laid out like [UAPI Discoverable Disk Images](https://uapi-group.org/specifications/specs/discoverable_disk_image/),
  with an EROFS filesystem partition, a dm-verity hash partition, and a signature
  partition.
- libcryptsetup to set up dm-verity on loop devices.
- overlayfs and the kernel's new mount API to assemble mount trees and hand them over to the launcher.
- `capsule-capture-libs` from [libcapsule](https://gitlab.com/fledermaus/libcapsule) to
  pass the host's graphics stack through.
- systemd for Varlink and various useful utilities.
- AppStream MetaInfo for component metadata.

### Integrate with the host

Applications run in a mount namespace that only exists for library consistency: the
user's home, `/dev`, D-Bus, the user's runtime directory, and the host's own filesystem
are all visible inside it. An application launched from a systemd unit *is* that unit's
main process. Applications can declare what to install on the host: aliases in `PATH`,
udev rules, systemd units, MIME types, and so on.

### Take the host's drivers

The graphics and compute drivers come from the host, because that is the driver stack the
host system is certified for. Everything else comes from the images, with a few exceptions
where the respective newer copy wins.


## Non-goals

- Manifold is not meant to replace Flatpak for consumer desktop apps. Flatpak's sandbox
  model works for that use case, and Manifold targets software it doesn't fit.
- Images are architecture-specific. x86_64 and aarch64 builds are separate artifacts, and
  Manifold refuses to install an image built for another architecture (for now at least).
- There is no standalone, unprivileged mode. Manifold requires a privileged installation
  step and installs applications system-wide, so an AppImage-like use case is out of scope.
- Manifold is not a sandbox. When a user launches an application, it runs in a user
  namespace, but only because that is how a process without privileges gets a mount
  namespace of its own. Nothing is hidden from the application.
- Hosts without Glibc are currently not supported. Manifold only targets Linux
  distributions with a GNU (or GNU-compatible) userland.


## Building

### Dependencies

- Meson ≥ 0.62 and a C compiler that supports C23
- GLib, GObject, and GIO ≥ 2.80
- libsystemd ≥ 257, plus the systemd and udev pkg-config files
- libcryptsetup ≥ 2.4
- libzip ≥ 1.7
- libblake3
- AppStream
- libcurl ≥ 7.62
- libelf ≥ 0.152

Runtime:
- Linux kernel ≥ 6.12
- systemd ≥ 257
- polkit ≥ 1.0
- erofs-utils ≥ 1.8.5
- Kernel compiled with: loop devices, device-mapper verity, EROFS, overlayfs

Optional:

- gobject-introspection, to build the GObject Introspection data
- `apparmor_parser` ≥ 4.0, to install the AppArmor profile
- Sphinx, to build the documentation

### Building and installing

```sh
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build

sudo systemctl daemon-reload
sudo systemctl enable --now manifold.socket manifold-run.socket
```

## Contributing

Patches, pull-requests, and bug reports are very welcome! Please make sure your code
is valid C23 and formats cleanly with `clang-format` (you can use the `./autoformat.py`
script for formatting).

## License

Manifold is licensed under the [GNU Lesser General Public License v2.1 or later](LICENSE).
