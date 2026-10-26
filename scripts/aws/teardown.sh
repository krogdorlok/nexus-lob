#!/usr/bin/env bash
# Removes everything setup.sh created: the security group, instance profile and role. The
# results bucket is kept unless --delete-bucket is given (it empties the bucket first).
# Prints every command; pass --apply to execute.
#   scripts/aws/teardown.sh [--apply] [--delete-bucket]
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=config.sh
source ./config.sh
DELETE_BUCKET=0
for arg in "$@"; do
  case $arg in
    --apply) APPLY=1 ;;
    --delete-bucket) DELETE_BUCKET=1 ;;
    *) echo "unknown flag $arg" >&2 && exit 2 ;;
  esac
done

LIVE=$(live_instances)
if [[ -n $LIVE ]]; then
  # shellcheck disable=SC2086  # LIVE is a whitespace-separated id list
  run aws_ ec2 terminate-instances --instance-ids $LIVE >/dev/null
  # shellcheck disable=SC2086
  ((APPLY)) && aws_ ec2 wait instance-terminated --instance-ids $LIVE
fi
SG=$(security_group "$(default_vpc)")
[[ $SG != None ]] && run aws_ ec2 delete-security-group --group-id "$SG"

if aws_ iam get-instance-profile --instance-profile-name "$NAME" >/dev/null 2>&1; then
  run aws_ iam remove-role-from-instance-profile --instance-profile-name "$NAME" --role-name "$NAME"
  run aws_ iam delete-instance-profile --instance-profile-name "$NAME"
fi
if aws_ iam get-role --role-name "$NAME" >/dev/null 2>&1; then
  run aws_ iam delete-role-policy --role-name "$NAME" --policy-name write-run-results
  run aws_ iam delete-role --role-name "$NAME"
fi

if ((DELETE_BUCKET)); then
  BUCKET=$(bucket_name)
  if aws_ s3api head-bucket --bucket "$BUCKET" >/dev/null 2>&1; then
    # The bucket is versioned: every object version and delete marker must go first.
    while :; do
      batch=$(aws_ s3api list-object-versions --bucket "$BUCKET" --max-items 400 --output json \
        --query '{Objects: [Versions, DeleteMarkers][][].{Key: Key, VersionId: VersionId}}')
      [[ $batch == *'"Key"'* ]] || break
      run aws_ s3api delete-objects --bucket "$BUCKET" --delete "$batch" >/dev/null
      ((APPLY)) || break  # a dry run shows one batch
    done
    run aws_ s3api delete-bucket --bucket "$BUCKET"
  fi
fi
((APPLY)) && echo "teardown complete" || echo "dry run only; rerun with --apply to execute"
