#!/usr/bin/env bash
set -eux

systemd-analyze log-level debug

# Multiple level process tree, parent process stays up
cat >/tmp/test56-exit-cgroup.sh <<EOF
#!/usr/bin/env bash
set -eux

# process tree: systemd -> sleep
sleep infinity &
disown

# process tree: systemd -> bash -> bash -> sleep
((sleep infinity); true) &

# process tree: systemd -> bash -> sleep
sleep infinity
EOF
chmod +x /tmp/test56-exit-cgroup.sh

# service should be stopped cleanly
(sleep 1; systemctl stop one) &
systemd-run --wait --unit=one -p ExitType=cgroup /tmp/test56-exit-cgroup.sh

# same thing with a truthy exec condition
(sleep 1; systemctl stop two) &
systemd-run --wait --unit=two -p ExitType=cgroup -p ExecCondition=true /tmp/test56-exit-cgroup.sh

# false exec condition: systemd-run should exit immediately with status code: 1
systemd-run --wait --unit=three -p ExitType=cgroup -p ExecCondition=false /tmp/test56-exit-cgroup.sh \
    && { echo 'unexpected success'; exit 1; }

# service should exit cleanly despite SIGKILL
(sleep 1; systemctl kill --signal 9 four) &
systemd-run --wait --unit=four -p ExitType=cgroup /tmp/test56-exit-cgroup.sh


# Multiple level process tree, parent process exits quickly
cat >/tmp/test56-exit-cgroup-parentless.sh <<EOF
#!/usr/bin/env bash
set -eux

# process tree: systemd -> sleep
sleep infinity &

# process tree: systemd -> bash -> sleep
((sleep infinity); true) &
EOF
chmod +x /tmp/test56-exit-cgroup-parentless.sh

# service should be stopped cleanly
(sleep 1; systemctl stop five) &
systemd-run --wait --unit=five -p ExitType=cgroup /tmp/test56-exit-cgroup-parentless.sh

# service should still exit uncleanly despite SIGKILL
(sleep 1; systemctl kill --signal 9 six) &
systemd-run --wait --unit=six -p ExitType=cgroup /tmp/test56-exit-cgroup-parentless.sh


systemd-analyze log-level info

echo OK >/testok

exit 0
