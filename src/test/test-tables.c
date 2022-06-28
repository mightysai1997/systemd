/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "architecture.h"
#include "automount.h"
#include "cgroup.h"
#include "cgroup-util.h"
#include "compress.h"
#include "condition.h"
#include "device-util.h"
#include "device.h"
#include "discover-image.h"
#include "execute.h"
#include "import-util.h"
#include "install.h"
#include "job.h"
#include "journald-server.h"
#include "kill.h"
#include "link-config.h"
#include "locale-util.h"
#include "log.h"
#include "logs-show.h"
#include "mount.h"
#include "path.h"
#include "process-util.h"
#include "resolve-util.h"
#include "rlimit-util.h"
#include "scope.h"
#include "service.h"
#include "show-status.h"
#include "slice.h"
#include "socket-util.h"
#include "socket.h"
#include "swap.h"
#include "target.h"
#include "test-tables.h"
#include "timer.h"
#include "unit-name.h"
#include "unit.h"
#include "util.h"
#include "virt.h"

int main(int argc, char **argv) {
        test_table(architecture, ARCHITECTURE);
        test_table(assert_type, CONDITION_TYPE);
        test_table(automount_result, AUTOMOUNT_RESULT);
        test_table(automount_state, AUTOMOUNT_STATE);
        test_table(cgroup_controller, CGROUP_CONTROLLER);
        test_table(cgroup_device_policy, CGROUP_DEVICE_POLICY);
        test_table(cgroup_io_limit_type, CGROUP_IO_LIMIT_TYPE);
        test_table(collect_mode, COLLECT_MODE);
        test_table(condition_result, CONDITION_RESULT);
        test_table(condition_type, CONDITION_TYPE);
        test_table(device_action, SD_DEVICE_ACTION);
        test_table(device_state, DEVICE_STATE);
        test_table(dns_over_tls_mode, DNS_OVER_TLS_MODE);
        test_table(dnssec_mode, DNSSEC_MODE);
        test_table(emergency_action, EMERGENCY_ACTION);
        test_table(exec_directory_type, EXEC_DIRECTORY_TYPE);
        test_table(exec_input, EXEC_INPUT);
        test_table(exec_keyring_mode, EXEC_KEYRING_MODE);
        test_table(exec_output, EXEC_OUTPUT);
        test_table(exec_preserve_mode, EXEC_PRESERVE_MODE);
        test_table(exec_utmp_mode, EXEC_UTMP_MODE);
        test_table(image_type, IMAGE_TYPE);
        test_table(import_verify, IMPORT_VERIFY);
        test_table(job_mode, JOB_MODE);
        test_table(job_result, JOB_RESULT);
        test_table(job_state, JOB_STATE);
        test_table(job_type, JOB_TYPE);
        test_table(kill_mode, KILL_MODE);
        test_table(kill_who, KILL_WHO);
        test_table(locale_variable, VARIABLE_LC);
        test_table(log_target, LOG_TARGET);
        test_table(mac_address_policy, MAC_ADDRESS_POLICY);
        test_table(managed_oom_mode, MANAGED_OOM_MODE);
        test_table(managed_oom_preference, MANAGED_OOM_PREFERENCE);
        test_table(manager_state, MANAGER_STATE);
        test_table(manager_timestamp, MANAGER_TIMESTAMP);
        test_table(mount_exec_command, MOUNT_EXEC_COMMAND);
        test_table(mount_result, MOUNT_RESULT);
        test_table(mount_state, MOUNT_STATE);
        test_table(name_policy, NAMEPOLICY);
        test_table(namespace_type, NAMESPACE_TYPE);
        test_table(notify_access, NOTIFY_ACCESS);
        test_table(notify_state, NOTIFY_STATE);
        test_table(output_mode, OUTPUT_MODE);
        test_table(partition_designator, PARTITION_DESIGNATOR);
        test_table(path_result, PATH_RESULT);
        test_table(path_state, PATH_STATE);
        test_table(path_type, PATH_TYPE);
        test_table(protect_home, PROTECT_HOME);
        test_table(protect_system, PROTECT_SYSTEM);
        test_table(resolve_support, RESOLVE_SUPPORT);
        test_table(rlimit, RLIMIT);
        test_table(scope_result, SCOPE_RESULT);
        test_table(scope_state, SCOPE_STATE);
        test_table(service_exec_command, SERVICE_EXEC_COMMAND);
        test_table(service_restart, SERVICE_RESTART);
        test_table(service_result, SERVICE_RESULT);
        test_table(service_state, SERVICE_STATE);
        test_table(service_type, SERVICE_TYPE);
        test_table(show_status, SHOW_STATUS);
        test_table(slice_state, SLICE_STATE);
        test_table(socket_address_bind_ipv6_only, SOCKET_ADDRESS_BIND_IPV6_ONLY);
        test_table(socket_exec_command, SOCKET_EXEC_COMMAND);
        test_table(socket_result, SOCKET_RESULT);
        test_table(socket_state, SOCKET_STATE);
        test_table(split_mode, SPLIT);
        test_table(storage, STORAGE);
        test_table(swap_exec_command, SWAP_EXEC_COMMAND);
        test_table(swap_result, SWAP_RESULT);
        test_table(swap_state, SWAP_STATE);
        test_table(target_state, TARGET_STATE);
        test_table(timer_base, TIMER_BASE);
        test_table(timer_result, TIMER_RESULT);
        test_table(timer_state, TIMER_STATE);
        test_table(unit_active_state, UNIT_ACTIVE_STATE);
        test_table(unit_dependency, UNIT_DEPENDENCY);
        test_table(unit_file_change_type, UNIT_FILE_CHANGE_TYPE);
        test_table(unit_file_preset_mode, UNIT_FILE_PRESET);
        test_table(unit_file_state, UNIT_FILE_STATE);
        test_table(unit_load_state, UNIT_LOAD_STATE);
        test_table(unit_type, UNIT_TYPE);
        test_table(virtualization, VIRTUALIZATION);
        test_table(compression, COMPRESSION);

        assert_cc(sizeof(sd_device_action_t) == sizeof(int64_t));

        return EXIT_SUCCESS;
}
