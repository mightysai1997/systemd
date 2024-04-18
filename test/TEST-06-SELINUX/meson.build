test_params += {
        'mkosi_image_name': 'testsuite-06-selinux-system',
}
testsuite_06_mkosi = custom_target('testsuite_06_mkosi',
        build_always_stale : true,
        output : 'testsuite_06',
        console : true,
        command : ['mkosi', '-C', meson.project_source_root(), '--image', test_params['mkosi_image_name'], '--format=disk', '--output-dir', meson.project_build_root() / '@OUTPUT@', '--without-tests', '-fi', 'build'],
        depends : [executables_by_name['bootctl'], executables_by_name['systemd-measure'], executables_by_name['systemd-repart'], ukify],
)
test_params += {
        'mkosi_output_path' : testsuite_06_mkosi,
        'setup_selinux' : true,
        'mkosi_args': [
                '--kernel-command-line-extra=' + ' '.join([
                        'systemd.wants=autorelabel.service',
                        'systemd.wants=firstboot-autorelabel.service',
                ]),
        ],
        'depends': [testsuite_06_mkosi],
}
