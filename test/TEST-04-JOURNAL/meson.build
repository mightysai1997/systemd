args = test_params['mkosi_args']
if get_option('tests-interactive-debug') == true
        args += ['--credential', meson.current_source_dir() / 'systemd.unit-dropin.testsuite-04.service']
endif
test_params += {
        'mkosi_args': args,
}
