output "web_public_ip" {
  value = aws_instance.web.public_ip
}

output "web_private_ip" {
  description = "This is DB_BIND on the web node and the host in DATABASE_URL on every agent. Getting it wrong — leaving it on loopback — is the single most common way a multi-node deployment fails."
  value       = aws_instance.web.private_ip
}

# The addresses operators actually reach the boxes on, via a jump through web.
#
# It has to be the private address. A security group rule that references
# another security group only matches traffic arriving on a PRIVATE IP inside
# the VPC — connect from the web node to a box's PUBLIC IP and the packets
# leave through the internet gateway and come back with no group membership
# attached, so the rule does not match and the connection times out.
output "agent_private_ips" {
  description = "Index 0 is agent-1, bound to participant 1."
  value       = aws_instance.agent[*].private_ip
}

output "agent_public_ips" {
  description = "Outbound only. Nothing can connect IN — the worker group accepts SSH from the web group alone."
  value       = aws_instance.agent[*].public_ip
}

output "golden_private_ip" {
  value = one(aws_instance.golden[*].private_ip)
}

# -i is spelled out rather than relying on the agent: the key is dedicated to
# this project, so it does not have one of the default filenames ssh offers
# automatically, and a fresh shell will not have it loaded.
locals {
  identity = trimsuffix(var.public_key_path, ".pub")
}

# ------------------------------------------------------- consumed by scripts
# ops/aws/*.sh derives everything from `terraform output -json`, so that no
# script ever takes a hostname or an IP as an argument — the failure that costs
# an event is a stale address pasted from yesterday into a command that looks
# right.

output "ssh_identity" {
  description = "Private key path matching the imported public key. Note the value may contain a literal ~, which a shell will not expand inside a variable."
  value       = local.identity
}

output "aws_region" {
  value = var.aws_region
}

output "s3_bucket" {
  value = var.s3_bucket
}

output "ssh" {
  description = "Ready-to-paste SSH. Agents are reached through the web node: ops/aws/ssh.sh agent1."
  value = {
    web = "ssh -i ${local.identity} ubuntu@${aws_instance.web.public_ip}"
  }
}

output "admin_tunnel" {
  description = "The operator API has no authentication. Reaching it IS SSH access to the web node."
  value       = "ssh -i ${local.identity} -N -L 8081:127.0.0.1:8081 ubuntu@${aws_instance.web.public_ip}"
}

# ---- consumed by ops/aws/provision-web-m8.sh to write /opt/flashmatch/
# provisioner.env, so the on-demand box daemon's launch parameters are derived
# from Terraform rather than hand-typed onto the web node.
output "subnet_id" {
  value = var.subnet_id
}

output "agent_security_group_id" {
  value = aws_security_group.worker.id
}

output "instance_profile" {
  value = local.instance_profile
}

output "key_name" {
  value = aws_key_pair.this.key_name
}

output "agent_instance_type" {
  value = var.agent_instance_type
}

output "agent_core_count" {
  value = var.agent_core_count
}
