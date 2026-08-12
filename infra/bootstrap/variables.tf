variable "aws_region" {
  description = "Region everything lives in. The deployer policy is scoped to it."
  type        = string
  default     = "us-east-1"
}

variable "aws_profile" {
  description = "Local AWS CLI profile with permission to create IAM roles and an S3 bucket. Only bootstrap needs this much; ../ runs as the deployer role."
  type        = string
  default     = "iicpc"
}

variable "bucket_name" {
  description = "Artifact bucket. Defaults to me-platform-artifacts-<account-id>, because bucket names are globally unique and a fixed name collides the moment two people try it."
  type        = string
  default     = null
}

variable "force_destroy_bucket" {
  description = "Let `terraform destroy` delete a bucket that still has objects in it. True is right for a throwaway test and wrong for the real event, where the bucket holds every submission's source."
  type        = bool
  default     = true
}

variable "node_role_name" {
  description = "Role and instance profile attached to all three instances. The deployer's iam:PassRole is scoped to exactly this name, so changing it here means changing it there."
  type        = string
  default     = "me-platform-node"
}

variable "deployer_role_name" {
  description = "Role that ../ assumes. Carries the permission set we intend to ask a company cloud team for, and nothing more."
  type        = string
  default     = "me-platform-deployer"
}
