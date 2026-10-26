#!/bin/bash
# EC2 user data for one benchmark run, rendered by launch_bench.sh (the __PLACEHOLDERS__ are
# plain tokens, validated there). Runs as root on Ubuntu 24.04. Whatever happens, the instance
# shuts down, and launch_bench.sh launches it with shutdown behaviour "terminate", so nothing
# is left running.
set -euxo pipefail
exec > >(tee /var/log/nexus-bench.log) 2>&1  # the console log gets a copy for debugging

BUCKET='__BUCKET__'
PREFIX='__PREFIX__'
COMMIT='__COMMIT__'
REPO='__REPO__'
export NEXUS_AMI='__AMI__' NEXUS_INSTANCE_TYPE='__INSTANCE_TYPE__'  # recorded in manifest.txt
export PATH=$PATH:/snap/bin  # the snap-installed aws CLI lives here

# Uploads the log, then a DONE (success) or FAILED marker, always last, then shuts down.
finish() {
  rc=$?
  aws s3 cp /var/log/nexus-bench.log "s3://$BUCKET/$PREFIX/run.log" || true
  if ((rc == 0)); then
    echo ok | aws s3 cp - "s3://$BUCKET/$PREFIX/DONE" || true
  else
    echo "exit $rc" | aws s3 cp - "s3://$BUCKET/$PREFIX/FAILED" || true
  fi
  shutdown -h now
}
trap finish EXIT
shutdown -h +__MAX_MINUTES__    # hard deadline, even if a step below hangs

snap wait system seed.loaded    # snapd is not ready this early in boot until seeding ends
snap install aws-cli --classic  # before the long steps, so finish() can upload the log

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential g++-13 cmake ninja-build git python3 \
  linux-tools-common "linux-tools-$(uname -r)"

git clone "$REPO" /opt/nexus
cd /opt/nexus
git checkout "$COMMIT"
CC=gcc-13 CXX=g++-13 REPS=5 REQUIRE_PERF=1 scripts/aws/remote_bench.sh "s3://$BUCKET/$PREFIX"
