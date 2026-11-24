#
# Code the daemons share and that makes no sense to put into libmanifold
#

shared_inc_dir = include_directories('.')

mf_daemon_shared_src = files(
    'mf-varlink-util.c',
    'mf-varlink-util.h',
)

# the public interface with the manifold management daemon
mf_mngmt_varlink_src = files(
    'mf-manager-interface.c',
    'mf-manager-interface.h',
)

# the private interface between manifoldd and its download helper
mf_mngmt_fetch_varlink_src = files(
    'mf-fetch-interface.c',
    'mf-fetch-interface.h',
)

# the interface applications use to run a program on the host
mf_host_varlink_src = files(
    'mf-host-interface.c',
    'mf-host-interface.h',
)

# the links to the programs of the host, which do not use GLib
mf_host_links_src = files(
    'mf-host-links.c',
    'mf-host-links.h',
)

# the semi-private interface for running a Manifold application
mf_run_varlink_src = files(
    'mf-run-interface.c',
    'mf-run-interface.h',
)
