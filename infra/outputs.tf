output "web_public_ip" {
  value = aws_instance.web.public_ip
}

output "web_private_ip" {
  description = "This is DB_BIND on the web node and the host in DATABASE_URL on both workers. Getting it wrong — leaving it on loopback — is the single most common way a three-node deployment fails."
  value       = aws_instance.web.private_ip
}

output "pool_public_ip" {
  value = aws_instance.pool.public_ip
}

output "bench_public_ip" {
  value = aws_instance.bench.public_ip
}

# -i is spelled out rather than relying on the agent: the key is dedicated to
# this project, so it does not have one of the default filenames ssh offers
# automatically, and a fresh shell will not have it loaded.
locals {
  identity = trimsuffix(var.public_key_path, ".pub")
}

output "ssh" {
  description = "Ready-to-paste SSH commands."
  value = {
    web   = "ssh -i ${local.identity} ubuntu@${aws_instance.web.public_ip}"
    pool  = "ssh -i ${local.identity} ubuntu@${aws_instance.pool.public_ip}"
    bench = "ssh -i ${local.identity} ubuntu@${aws_instance.bench.public_ip}"
  }
}

output "admin_tunnel" {
  description = "The operator API has no authentication. Reaching it IS SSH access to the web node."
  value       = "ssh -i ${local.identity} -N -L 8081:127.0.0.1:8081 ubuntu@${aws_instance.web.public_ip}"
}

output "worker_env" {
  description = "Paste into /opt/mebench/worker.env on BOTH workers, after setting the Postgres password."
  value       = <<-EOT
    DATABASE_URL=postgres://mebench:<POSTGRES_PASSWORD>@${aws_instance.web.private_ip}:5432/mebench
    S3_BUCKET=${var.s3_bucket}
    AWS_REGION=${var.aws_region}
  EOT
}

output "web_env" {
  description = "Export on the web node BEFORE the first `docker compose up`. POSTGRES_PASSWORD is read only when the cluster is first initialised; changing it later means deleting the pgdata volume."
  value       = <<-EOT
    export DB_BIND='${aws_instance.web.private_ip}'
    export AWS_REGION='${var.aws_region}'
    export S3_BUCKET='${var.s3_bucket}'
    export POSTGRES_PASSWORD='<something long>'
    export SITE_ADDRESS=':80'
  EOT
}
