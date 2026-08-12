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

locals {
  bucket = coalesce(var.bucket_name, "me-platform-artifacts-${data.aws_caller_identity.current.account_id}")
}

# ----------------------------------------------------------------- artifacts
# Source, compiled binaries and histograms, keyed on sha256. Nothing in the
# platform creates this bucket; without it every /run and /submit returns an
# opaque "internal error", because the API stores source before it does
# anything else.

resource "aws_s3_bucket" "artifacts" {
  bucket        = local.bucket
  force_destroy = var.force_destroy_bucket

  tags = { Project = "me-platform" }
}

resource "aws_s3_bucket_public_access_block" "artifacts" {
  bucket                  = aws_s3_bucket.artifacts.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
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
  name = var.node_role_name

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })

  tags = { Project = "me-platform" }
}

resource "aws_iam_role_policy" "node" {
  name = "${var.node_role_name}-s3"
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
  name = var.node_role_name
  role = aws_iam_role.node.name
}

# ---------------------------------------------------------- deployer role
# Deliberately not admin: it can launch and destroy the platform's instances in
# one region and pass the node role to EC2, and nothing else. The same policy
# document works attached to a user instead of a role.

resource "aws_iam_role" "deployer" {
  name = var.deployer_role_name

  # Trusted by whoever ran bootstrap. Change this to the principal that should
  # be able to assume the role — an SSO principal, or another user's ARN.
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { AWS = data.aws_caller_identity.current.arn }
      Action    = "sts:AssumeRole"
    }]
  })

  max_session_duration = 3600
  tags                 = { Project = "me-platform" }
}

resource "aws_iam_role_policy" "deployer" {
  name = "${var.deployer_role_name}-policy"
  role = aws_iam_role.deployer.id

  # Kept in its own file so the policy can be read, or applied by hand, without
  # going through Terraform.
  policy = templatefile("${path.module}/deployer-policy.json.tftpl", {
    region     = var.aws_region
    account_id = data.aws_caller_identity.current.account_id
    bucket     = local.bucket
    node_role  = var.node_role_name
  })
}
