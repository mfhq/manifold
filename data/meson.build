
#
# systemd units
#
units_conf = configuration_data()
units_conf.set('MF_LIBEXECDIR', mf_libexecdir)
units_conf.set('MF_RUN_INTERFACE', mf_run_interface)
units_conf.set('MF_RUN_SOCKET_PATH', mf_run_socket)
units_conf.set('MF_INTERFACE', mf_interface)
units_conf.set('MF_SOCKET_PATH', mf_socket)

foreach unit : ['manifold-run.socket',
                'manifold-run.service',
                'manifold.socket',
                'manifold.service']
    configure_file(
        input: unit + '.in',
        output: unit,
        configuration: units_conf,
        install: true,
        install_dir: systemd_system_unit_dir,
    )
endforeach

#
# polkit actions
#
install_data('org.freedesktop.manifold.policy',
    install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)

#
# udev rules
#
udev_conf = configuration_data()
udev_conf.set('MF_STORAGE_DIR', mf_storage_dir)

configure_file(
    input: '60-manifold.rules.in',
    output: '60-manifold.rules',
    configuration: udev_conf,
    install: true,
    install_dir: udev_rules_dir,
)

#
# Shell/environment integration with the host
#
exports_conf = configuration_data()
exports_conf.set('MF_EXPORTS_DIR', mf_storage_dir / 'exports')

configure_file(
    input: 'manifold-profile.sh.in',
    output: 'manifold.sh',
    configuration: exports_conf,
    install: true,
    install_dir: get_option('profile-dir'),
)
configure_file(
    input: 'manifold.fish.in',
    output: 'manifold.fish',
    configuration: exports_conf,
    install: true,
    install_dir: get_option('datadir') / 'fish' / 'vendor_conf.d',
)
configure_file(
    input: '990-manifold.conf.in',
    output: '990-manifold.conf',
    configuration: exports_conf,
    install: true,
    install_dir: mf_prefix / 'lib' / 'environment.d',
)

#
# AppArmor profile
#
apparmor_parser_exe = find_program('apparmor_parser',
                               dirs: ['/usr/sbin', '/sbin'],
                               version: '>= 4.0',
                               required: get_option('apparmor'),
)
if apparmor_parser_exe.found()
    apparmor_conf = configuration_data()
    apparmor_conf.set('MF_BINDIR', mf_bindir)

    configure_file(
        input: 'mfrun.apparmor.in',
        output: 'mfrun',
        configuration: apparmor_conf,
        install: true,
        install_dir: get_option('apparmor-dir'),
    )
endif
