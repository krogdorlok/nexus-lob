#!/usr/bin/env bash
# One-time AWS setup for benchmark runs, safe to rerun:
#   * a private S3 bucket for results (public access blocked, runs expire after a year)
#   * an EC2 instance role that can only write objects under runs/ in that bucket
#   * a security group with no inbound rules (the instance is never reachable)
# Prints every command; pass --apply to execute.
#   scripts/aws/setup.sh [--apply]
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=config.sh
source ./config.sh
for arg in "$@"; do
  case $arg in
    --apply) APPLY=1 ;;
    *) echo "unknown flag $arg" >&2 && exit 2 ;;
  esac
done

BUCKET=$(bucket_name)
echo "account bucket: $BUCKET (region $REGION)"

if aws_ s3api head-bucket --bucket "$BUCKET" >/dev/null 2>&1; then
  echo "bucket exists"
else
  run aws_ s3api create-bucket --bucket "$BUCKET" \
    --create-bucket-configuration "LocationConstraint=$REGION"
fi
run aws_ s3api put-public-access-block --bucket "$BUCKET" --public-access-block-configuration \
  BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
# Versioning: an overwritten or deleted run object stays recoverable, so a run can never
# silently replace an earlier run's numbers.
run aws_ s3api put-bucket-versioning --bucket "$BUCKET" --versioning-configuration Status=Enabled
run aws_ s3api put-bucket-lifecycle-configuration --bucket "$BUCKET" --lifecycle-configuration \
  '{"Rules":[{"ID":"expire-runs","Status":"Enabled","Filter":{"Prefix":"runs/"},"Expiration":{"Days":365},"NoncurrentVersionExpiration":{"NoncurrentDays":365}}]}'

if aws_ iam get-role --role-name "$NAME" >/dev/null 2>&1; then
  # Reuse it only if it carries nothing beyond this script's single inline policy.
  attached=$(aws_ iam list-attached-role-policies --role-name "$NAME" \
    --query 'AttachedPolicies[].PolicyName' --output text)
  inline=$(aws_ iam list-role-policies --role-name "$NAME" --query 'PolicyNames' --output text)
  if [[ -n $attached || ($inline != write-run-results && -n $inline) ]]; then
    echo "role $NAME has extra policies ($attached $inline); remove them or run teardown.sh" >&2
    exit 1
  fi
  echo "role exists"
else
  run aws_ iam create-role --role-name "$NAME" --description "NexusLOB benchmark instance" \
    --assume-role-policy-document \
    '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]}'
fi
# The only permission the instance ever has: put objects under runs/ in this one bucket.
run aws_ iam put-role-policy --role-name "$NAME" --policy-name write-run-results \
  --policy-document \
  "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Action\":\"s3:PutObject\",\"Resource\":\"arn:aws:s3:::$BUCKET/runs/*\"}]}"

if roles=$(aws_ iam get-instance-profile --instance-profile-name "$NAME" \
  --query 'InstanceProfile.Roles[].RoleName' --output text 2>/dev/null); then
  if [[ $roles != "$NAME" ]]; then
    echo "instance profile $NAME holds roles '$roles', expected only $NAME" >&2
    exit 1
  fi
  echo "instance profile exists"
else
  run aws_ iam create-instance-profile --instance-profile-name "$NAME"
  run aws_ iam add-role-to-instance-profile --instance-profile-name "$NAME" --role-name "$NAME"
fi

VPC=$(default_vpc)
if [[ $VPC == None ]]; then
  echo "no default VPC in $REGION; create one with: aws ec2 create-default-vpc" >&2
  exit 1
fi
SG=$(security_group "$VPC")
if [[ $SG == None ]]; then
  run aws_ ec2 create-security-group --group-name "$NAME" --vpc-id "$VPC" \
    --description "NexusLOB benchmark host, no inbound access" >/dev/null
  SG=$( ((APPLY)) && security_group "$VPC" || echo "<new group>")
fi
if [[ $SG == "<new group>" ]]; then  # dry run: show what a fresh group would get
  ingress=0
  egress='"IpProtocol": "-1"'
else
  ingress=$(aws_ ec2 describe-security-groups --group-ids "$SG" \
    --query 'length(SecurityGroups[0].IpPermissions)' --output text)
  egress=$(aws_ ec2 describe-security-groups --group-ids "$SG" --output json \
    --query 'SecurityGroups[0].IpPermissionsEgress')
fi
if ((ingress != 0)); then
  echo "security group $SG has inbound rules; it must have none" >&2
  exit 1
fi
# Outbound: only HTTP and HTTPS (Ubuntu mirrors, snap, GitHub, S3). DNS, the instance
# metadata service and the time service are link-local and not filtered by groups.
if [[ $egress == *'"IpProtocol": "-1"'* ]]; then
  run aws_ ec2 revoke-security-group-egress --group-id "$SG" \
    --ip-permissions 'IpProtocol=-1,IpRanges=[{CidrIp=0.0.0.0/0}]' >/dev/null
fi
for port in 80 443; do
  if [[ $egress != *"\"FromPort\": $port"* ]]; then
    run aws_ ec2 authorize-security-group-egress --group-id "$SG" \
      --ip-permissions "IpProtocol=tcp,FromPort=$port,ToPort=$port,IpRanges=[{CidrIp=0.0.0.0/0}]" >/dev/null
  fi
done
echo "security group $SG: no inbound, outbound tcp 80 and 443 only"

((APPLY)) && echo "setup complete" || echo "dry run only; rerun with --apply to execute"
