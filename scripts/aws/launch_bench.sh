#!/usr/bin/env bash
# Launches one self-terminating benchmark run on EC2, waits for it, downloads its results from
# S3 into results/aws-<instance type>/, and compares them with the previous archived run of
# the same instance type. Without --apply it only prints what it would do.
#   scripts/aws/launch_bench.sh [--apply] [--commit SHA]
# The commit must be on GitHub: the instance clones it from there.
set -euo pipefail
cd "$(dirname "$0")/../.."
source scripts/aws/config.sh
COMMIT=HEAD
while (($#)); do
  case $1 in
    --apply) APPLY=1 ;;
    --commit) COMMIT=${2:?--commit needs a value} && shift ;;
    *) echo "unknown flag $1" >&2 && exit 2 ;;
  esac
  shift
done

if ! [[ $MAX_MINUTES =~ ^[0-9]+$ ]] || ((MAX_MINUTES < 15 || MAX_MINUTES > 180)); then
  echo "MAX_MINUTES must be a whole number from 15 to 180" >&2
  exit 2
fi
safe INSTANCE_TYPE "$INSTANCE_TYPE"
safe REPO_URL "$REPO_URL"
# The free plan only runs free-tier instance types; anything else is an explicit opt-in.
eligible=$(aws_ ec2 describe-instance-types --instance-types "$INSTANCE_TYPE" \
  --filters Name=free-tier-eligible,Values=true --query 'length(InstanceTypes)' --output text)
if [[ $eligible != 1 && -z ${ALLOW_PAID:-} ]]; then
  echo "$INSTANCE_TYPE is not free-tier eligible; set ALLOW_PAID=1 to launch it anyway" >&2
  exit 2
fi

COMMIT=$(git rev-parse "$COMMIT")
git fetch -q --prune origin
if [[ -z $(git branch -r --contains "$COMMIT" --list 'origin/*') ]]; then
  echo "commit $COMMIT is not on origin; push it first" >&2
  exit 1
fi

BUCKET=$(bucket_name)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
PREFIX="runs/$INSTANCE_TYPE/${COMMIT:0:7}/$STAMP"
TOKEN="$NAME-${COMMIT:0:12}-$STAMP"  # idempotency: a retried launch can never start a second one

VPC=$(default_vpc)
SG=$(security_group "$VPC")
if [[ $SG == None ]]; then
  if ((APPLY)); then
    echo "no $NAME security group in $VPC; run scripts/aws/setup.sh --apply first" >&2
    exit 1
  fi
  SG="<security group from setup.sh>"
fi
# Latest official Ubuntu 24.04 image from Canonical (owner 099720109477).
AMI=$(aws_ ec2 describe-images --owners 099720109477 --filters \
  'Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*' \
  'Name=state,Values=available' --query 'sort_by(Images,&CreationDate)[-1].ImageId' \
  --output text)
LIVE=$(live_instances)
if [[ -n $LIVE ]]; then
  echo "a benchmark instance is already running ($LIVE); wait for it to finish" >&2
  exit 1
fi

userdata=$(mktemp)
ID=""
finished=0
# From launch on, any exit (error, Ctrl-C, expired login) terminates the instance unless it is
# already confirmed terminated. The instance's own deadline is the backstop if this fails too.
cleanup() {
  rm -f "$userdata"
  if ((APPLY && !finished)); then
    if [[ -z $ID ]]; then
      ID=$(aws_ ec2 describe-instances --filters "Name=client-token,Values=$TOKEN" \
        --query 'Reservations[].Instances[].InstanceId' --output text 2>/dev/null || true)
    fi
    if [[ -n $ID ]]; then
      echo "terminating $ID" >&2
      aws_ ec2 terminate-instances --instance-ids "$ID" >/dev/null || true
      aws_ ec2 wait instance-terminated --instance-ids "$ID" || true
    fi
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

sed -e "s|__BUCKET__|$BUCKET|" -e "s|__PREFIX__|$PREFIX|" -e "s|__COMMIT__|$COMMIT|" \
  -e "s|__REPO__|$REPO_URL|" -e "s|__MAX_MINUTES__|$MAX_MINUTES|" \
  scripts/aws/userdata.sh > "$userdata"

echo "commit $COMMIT on $INSTANCE_TYPE ($AMI), results to s3://$BUCKET/$PREFIX"
# shellcheck disable=SC2054  # the commas are AWS CLI shorthand syntax, not array separators
launch=(aws_ ec2 run-instances --count 1 --client-token "$TOKEN" --image-id "$AMI"
  --instance-type "$INSTANCE_TYPE" --iam-instance-profile "Name=$NAME"
  --security-group-ids "$SG"
  --metadata-options HttpTokens=required,HttpEndpoint=enabled,HttpPutResponseHopLimit=1
  --instance-initiated-shutdown-behavior terminate
  --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=16,VolumeType=gp3,DeleteOnTermination=true}'
  --user-data "file://$userdata"
  --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$NAME},{Key=run,Value=$PREFIX}]"
  --query 'Instances[0].InstanceId' --output text)
if ((!APPLY)); then
  run "${launch[@]}"
  echo "--- user data the instance would run:"
  cat "$userdata"
  echo "--- dry run only; rerun with --apply to launch"
  exit 0
fi

ID=$(run "${launch[@]}")
echo "launched $ID; it shuts itself down within $MAX_MINUTES minutes"

exists() { aws_ s3api head-object --bucket "$BUCKET" --key "$PREFIX/$1" >/dev/null 2>&1; }
deadline=$((SECONDS + (MAX_MINUTES + 10) * 60))
status=""
while [[ -z $status ]]; do
  state=$(aws_ ec2 describe-instances --instance-ids "$ID" \
    --query 'Reservations[0].Instances[0].State.Name' --output text)
  if exists DONE; then
    status=ok
  elif exists FAILED; then
    status=failed
  elif [[ $state == terminated ]]; then
    exists DONE && status=ok || status=failed  # DONE may land just before termination
  elif ((SECONDS > deadline)); then
    status=timeout
  else
    echo "$(date +%H:%M:%S) $ID is $state, waiting for results"
    sleep 30
  fi
done

if [[ $status != ok ]]; then
  echo "run $status; last lines of its log:" >&2
  aws_ s3 cp "s3://$BUCKET/$PREFIX/run.log" - 2>/dev/null | tail -40 >&2 ||
    aws_ ec2 get-console-output --instance-id "$ID" --latest --output text | tail -40 >&2
  exit 1
fi

aws_ ec2 wait instance-terminated --instance-ids "$ID"
finished=1
echo "$ID is terminated"

dest="results/aws-$INSTANCE_TYPE"
rm -rf "$dest"
aws_ s3 cp --recursive "s3://$BUCKET/$PREFIX" "$dest" --only-show-errors
echo "results in $dest"

# Compare with the most recent earlier completed run of this instance type, if any.
previous=$(aws_ s3api list-objects-v2 --bucket "$BUCKET" --prefix "runs/$INSTANCE_TYPE/" \
  --query "Contents[?ends_with(Key, '/DONE')].Key" --output text | tr '\t' '\n' |
  sed 's|/DONE$||' | grep -v "^$PREFIX$" | awk -F/ '{print $NF, $0}' | sort | tail -1 |
  cut -d' ' -f2 || true)
if [[ -n $previous && $previous != None ]]; then
  base=$(mktemp -d)
  aws_ s3 cp --recursive "s3://$BUCKET/$previous" "$base" --only-show-errors
  python3 scripts/compare_results.py "$base" "$dest" > "$dest/comparison.md"
  rm -rf "$base"
  echo "compared with s3://$BUCKET/$previous: $dest/comparison.md"
else
  echo "first archived run for $INSTANCE_TYPE; later runs will be compared with it"
fi
echo "next: python3 scripts/make_results_table.py $dest RESULTS_AWS"
