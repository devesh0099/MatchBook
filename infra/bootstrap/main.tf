# Bootstrap — run once, before ../.
#
# Creates the artifact bucket, the instance role every node carries, and a role
# holding the deployment permission set. These are the only resources here that
# require IAM write; everything in ../ runs without it.
#
#   terraform -chdir=infra/bootstrap init
#   terraform -chdir=infra/bootstrap apply

terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region  = var.aws_region
  profile = var.aws_profile
}

data "aws_caller_identity" "current" {}

# Resolves an STS session ARN back to the ROLE that issued it, and passes an IAM
# user ARN through unchanged. Needed because caller_identity.arn is a session
# ARN whenever bootstrap is run via SSO, role chaining or CI:
#
#   arn:aws:sts::123456789012:assumed-role/AWSReservedSSO_Admin_9f2c/alice
#
# IAM rejects that in a Principal element outright — MalformedPolicyDocument,
# "Invalid principal in policy" — so the deployer role below simply never
# created for anyone not using a long-lived IAM user.
data "aws_iam_session_context" "current" {
  arn = data.aws_caller_identity.current.arn
}

# Every name this module creates, derived from one prefix. The explicit
# variables stay as overrides for an account that already has a resource of the
# default name.
locals {
  bucket             = coalesce(var.bucket_name, "${var.name_prefix}-artifacts-${data.aws_caller_identity.current.account_id}")
  node_role_name     = coalesce(var.node_role_name, "${var.name_prefix}-node")
  deployer_role_name = coalesce(var.deployer_role_name, "${var.name_prefix}-deployer")

  # The tag the deployer policy conditions on and ops/aws/status.sh filters by.
  project_tag = var.name_prefix
  tags        = { Project = local.project_tag, ManagedBy = "terraform" }
}

# ----------------------------------------------------------------- artifacts
# Source, compiled binaries and histograms, keyed on sha256. Nothing in the
# platform creates this bucket; without it every /run and /submit returns an
# opaque "internal error", because the API stores source before it does
# anything else.

resource "aws_s3_bucket" "artifacts" {
  bucket        = local.bucket
  force_destroy = var.force_destroy_bucket

  tags = local.tags
}

resource "aws_s3_bucket_public_access_block" "artifacts" {
  bucket                  = aws_s3_bucket.artifacts.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# The node role holds s3:PutObject on the whole bucket, and both worker nodes
# execute participant-submitted code. An escape that overwrote other people's
# source would be a scoring-integrity compromise with no undo — versioning is
# what makes it recoverable, and at a day's worth of submissions it is free.
resource "aws_s3_bucket_versioning" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id
  versioning_configuration {
    status = var.bucket_versioning ? "Enabled" : "Suspended"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

# -------------------------------------------------------------- node role
# Attached to all three instances, and the ONLY AWS access the running platform
# has: read and write one bucket. No EC2, no Secrets Manager, no CloudWatch.
# The API stores submission source in S3 before it does anything else, so
# without this every /run and /submit returns an opaque internal error.

resource "aws_iam_role" "node" {
  name = local.node_role_name

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })

  tags = local.tags
}

resource "aws_iam_role_policy" "node" {
  name = "${local.node_role_name}-s3"
  role = aws_iam_role.node.id

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect   = "Allow"
      Action   = ["s3:GetObject", "s3:PutObject", "s3:ListBucket"]
      Resource = [aws_s3_bucket.artifacts.arn, "${aws_s3_bucket.artifacts.arn}/*"]
    }]
  })
}

# The instance profile is the thing EC2 actually accepts; the role alone is not
# attachable. Same name as the role by convention, and it is the name that goes
# in `iam_instance_profile` on the instances.
resource "aws_iam_instance_profile" "node" {
  name = local.node_role_name
  role = aws_iam_role.node.name
}

# ---------------------------------------------------------- deployer role
# Deliberately not admin: it can launch and destroy the platform's instances in
# one region and pass the node role to EC2, and nothing else. The same policy
# document works attached to a user instead of a role.

resource "aws_iam_role" "deployer" {
  name = local.deployer_role_name

  # Trusted by whoever ran bootstrap — as a durable role or user ARN, never as
  # the session ARN. Override with var.deployer_trusted_principal to hand the
  # role to somebody else instead.
  #
  # A caveat that survives this fix: if the trusted principal is an SSO
  # permission-set role, re-provisioning that permission set DELETES and
  # recreates the role, and IAM then rewrites this Principal to the old role's
  # opaque unique id (AROA...). The trust breaks silently and permanently. For
  # anything longer-lived than an event, trust a stable IAM user or a role you
  # own outright.
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect = "Allow"
      Principal = {
        AWS = coalesce(
          var.deployer_trusted_principal,
          data.aws_iam_session_context.current.issuer_arn,
        )
      }
      Action = "sts:AssumeRole"
    }]
  })

  max_session_duration = 3600
  tags                 = local.tags
}

resource "aws_iam_role_policy" "deployer" {
  name = "${local.deployer_role_name}-policy"
  role = aws_iam_role.deployer.id

  # Kept in its own file so the policy can be read, or applied by hand, without
  # going through Terraform.
  policy = templatefile("${path.module}/deployer-policy.json.tftpl", {
    region      = var.aws_region
    account_id  = data.aws_caller_identity.current.account_id
    bucket      = local.bucket
    node_role   = local.node_role_name
    project_tag = local.project_tag
  })
}
