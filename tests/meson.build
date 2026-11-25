#
# Tests
#

python_exe = import('python').find_installation('python3')

# helper functions shared by the tests
mft_test_utils_lib = static_library('mft-test-utils',
    ['mft-utils.c',
     'mft-utils.h',
     'mft-sandbox.c',
     'mft-sandbox.h',
    ],
    include_directories: [root_inc_dir],
    dependencies: [
        glib_dep,
        libglnx_dep,
        zstd_dep,
        manifold_dep.partial_dependency(includes: true, sources: true)
    ],
)
mft_test_utils_dep = declare_dependency(
    link_with: mft_test_utils_lib,
    dependencies: [glib_dep, libglnx_dep, zstd_dep, blake3_dep],
)

test_sandbox_exe = executable('mf-test-sandbox',
    ['mft-sandbox-main.c'],
    include_directories: [root_inc_dir],
    dependencies: [mft_test_utils_dep],
)

# tool that writes the zchunk files of the repositories which the end-to-end tests create
test_zchunk_exe = executable('mf-test-zchunk',
    ['mft-zchunk-main.c'],
    include_directories: [root_inc_dir],
    dependencies: [manifold_dep],
)

#
# Unit tests
#
unit_tests = {
    # tests of libmanifold
    'utils': [manifold_dep],
    'manifest': [manifold_dep, mft_test_utils_dep],
    'exports': [manifold_dep, mft_test_utils_dep],
    'bundle': [manifold_dep, mft_test_utils_dep, libzip_dep],
    'signer': [manifold_dep, mft_test_utils_dep],
    'remote': [manifold_dep, mft_test_utils_dep],
    'repo': [manifold_dep, mft_test_utils_dep],
    'zchunk': [manifold_dep, mft_test_utils_dep, zck_dep, zstd_dep],
    'store-read': [manifold_dep, mft_test_utils_dep],

    # tests of manifoldd, the store test runs in the sandbox
    'store': [manifoldd_static_dep, mft_test_utils_dep],
    'image-reader': [manifoldd_static_dep, mft_test_utils_dep],

    # tests of manifold-rund, the compose and capture tests run in the sandbox
    'compose': [manifold_rund_static_dep, mft_test_utils_dep],
    'capture': [manifold_rund_static_dep, mft_test_utils_dep],
    'host-bin': [manifold_rund_static_dep, mft_test_utils_dep],
}

foreach name, dependencies : unit_tests
    test(name,
        executable('test-' + name,
            ['test-' + name + '.c'],
            include_directories: [root_inc_dir],
            dependencies: dependencies,
        ),
        # some tests read files from the samples directory
        env: {'G_TEST_SRCDIR': meson.current_source_dir()},
        suite: 'unit',
    )
endforeach

#
# End-to-end
#

# this test is skipped if b3sum or varlinkctl are not installed
test('installer',
    test_sandbox_exe,
    args: [python_exe.full_path(), files('test-installer.py'),
           '--daemon', manifoldd_exe.full_path(), '--client', manifold_exe.full_path(),
           '--integration-prefix', mf_integration_prefix],
    depends: [manifoldd_exe, manifold_exe],
    suite: 'e2e',
    timeout: 120,
)

# runs in the sandbox, refreshes remotes from a local HTTPS server and installs components from them
test('remotes',
    test_sandbox_exe,
    args: [python_exe.full_path(), files('test-remote.py'),
           '--daemon', manifoldd_exe.full_path(), '--fetch-helper', manifold_fetch_exe.full_path(),
           '--client', manifold_exe.full_path(), '--remotes-dir', mf_sysconfdir / 'remotes',
           '--vendor-remotes-dir', mf_datadir / 'remotes',
           '--zchunk-tool', test_zchunk_exe.full_path()],
    depends: [manifoldd_exe, manifold_fetch_exe, manifold_exe, test_zchunk_exe],
    suite: 'e2e',
    timeout: 120,
)

# downloads files from a local HTTPS server. This test is skipped if openssl is not installed,
# as we need it to create the certificate of the server.
test('fetch',
    python_exe,
    args: [files('test-fetch.py'), '--helper', manifold_fetch_exe.full_path()],
    depends: [manifold_fetch_exe],
    suite: 'e2e',
    timeout: 60,
)

test('hostrun',
    python_exe,
    args: [files('test-hostrun.py'), '--runner', manifold_hostrun_exe.full_path()],
    depends: [manifold_hostrun_exe],
    suite: 'e2e',
    timeout: 60,
)

# this test starts the sandbox itself, as only a part of it has to run in the sandbox
test('runner',
    python_exe,
    args: [files('test-runner.py'), '--sandbox', test_sandbox_exe.full_path(), '--runner', manifold_run_exe.full_path()],
    depends: [test_sandbox_exe, manifold_run_exe],
    suite: 'e2e',
    timeout: 60,
)
