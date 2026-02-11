#!/bin/sh
echo "Creating RT cgroup"
set -euo pipefail
UUID=isol
cgcreate -g cpuset:$UUID
cgset -r cpuset.cpus=0 $UUID
trap 'echo "Interrupted - moving on ...";' INT
cgexec -g cpuset:$UUID unshare bash -c "$*" || true
echo "Deleteting RT cgroup"
cgdelete cpuset:$UUID