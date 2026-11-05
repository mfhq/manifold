#
# manifold-fetch: The sandboxed download helper of manifoldd
#

manifold_fetch_exe = executable('manifold-fetch',
    ['manifold-fetch.c',
     'mff-downloader.c',
     'mff-downloader.h',
     mf_fetch_interface_src,
     mf_daemon_shared_src,
    ],
    c_args: hardening_c_args + ['-DG_LOG_DOMAIN="manifold-fetch"'],
    link_args: hardening_link_args,
    include_directories: [root_inc_dir, shared_inc_dir, mf_lib_inc],
    dependencies: [
        glib_dep,
        gio_dep,
        libsystemd_dep,
        curl_dep,
    ],
    install: true,
    install_dir: get_option('libexecdir'),
)
