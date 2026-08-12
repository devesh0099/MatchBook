output "s3_bucket" {
  description = "Artifact bucket. Goes into ../terraform.tfvars and into worker.env on every node."
  value       = aws_s3_bucket.artifacts.id
}

output "instance_profile" {
  description = "Instance profile name for all three instances."
  value       = aws_iam_instance_profile.node.name
}

output "deployer_role_arn" {
  description = "Assume this in ../. Copy it into ../terraform.tfvars as deployer_role_arn."
  value       = aws_iam_role.deployer.arn
}

output "tfvars_snippet" {
  description = "Paste into ../terraform.tfvars."
  value       = <<-EOT
    deployer_role_arn = "${aws_iam_role.deployer.arn}"
    instance_profile  = "${aws_iam_instance_profile.node.name}"
    s3_bucket         = "${aws_s3_bucket.artifacts.id}"
  EOT
}
