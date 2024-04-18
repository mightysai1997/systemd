args = test_params['mkosi_args']
if get_option('b_coverage') == true
        args += ['--credential', meson.current_source_dir() / 'systemd.unit-dropin.issue2467.socket']
endif
test_params += {
        'mkosi_args': args,
}
