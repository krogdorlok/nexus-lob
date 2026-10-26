# Shared settings and helpers for scripts/aws/*.sh (sourced, not run). Every setting can be
# overridden from the environment, e.g. INSTANCE_TYPE=m7i-flex.large scripts/aws/launch_bench.sh
# shellcheck shell=bash
PROFILE=${PROFILE:-nexus}
REGION=${REGION:-us-east-2}
INSTANCE_TYPE=${INSTANCE_TYPE:-c7i-flex.large}  # free-tier eligible; 2 vCPU Intel Sapphire Rapids
MAX_MINUTES=${MAX_MINUTES:-90}                  # the instance shuts itself down by then, always
AMI=${AMI:-}  # pin an image to keep a comparison series on one OS build; empty = latest
REPO_URL=${REPO_URL:-https://github.com/krogdorlok/nexus-lob}
NAME=nexus-lob-bench  # role, instance profile, security group and instance tag
APPLY=0               # set to 1 by --apply; until then every mutating command is only printed

aws_() { aws --profile "$PROFILE" --region "$REGION" "$@"; }

# Values rendered into the instance's boot script must be plain tokens (no shell syntax).
safe() {
  [[ $2 =~ ^[A-Za-z0-9._:/@+-]+$ ]] || { echo "$1 has unexpected characters: $2" >&2 && exit 2; }
}

# The default VPC and this project's security group in it ("None" if absent).
default_vpc() {
  aws_ ec2 describe-vpcs --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text
}
security_group() {
  aws_ ec2 describe-security-groups --filters "Name=group-name,Values=$NAME" \
    "Name=vpc-id,Values=$1" --query 'SecurityGroups[0].GroupId' --output text
}

# Instances of this project that are not yet terminated.
live_instances() {
  aws_ ec2 describe-instances --filters "Name=tag:Name,Values=$NAME" \
    'Name=instance-state-name,Values=pending,running,stopping,stopped,shutting-down' \
    --query 'Reservations[].Instances[].InstanceId' --output text
}

# Prints (to stderr) a command that changes AWS state, and runs it only with --apply.
run() {
  if ((APPLY)); then
    printf '+ %s\n' "$*" >&2
    "$@"
  else
    printf '[dry-run] %s\n' "$*" >&2
  fi
}

# Globally unique, so it carries the account id; region-scoped like everything else here.
bucket_name() {
  echo "$NAME-$(aws_ sts get-caller-identity --query Account --output text)-$REGION"
}
