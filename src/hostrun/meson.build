#
# manifold-hostrun: Runs a program of the host for an application
#

manifold_hostrun_exe = executable('manifold-hostrun',
    ['manifold-hostrun.c',
     mf_daemon_shared_src,
     mf_host_varlink_src,
    ],
    c_args: hardening_c_args + ['-DG_LOG_DOMAIN="manifold-hostrun"'],
    link_args: hardening_link_args,
    include_directories: [root_inc_dir, shared_inc_dir, mf_lib_inc],
    dependencies: [
        glib_dep,
        gio_dep,
        libsystemd_dep,
    ],
    install: true,
    install_dir: get_option('libexecdir'),
)
