#!/bin/bash
# Render deploy/*.yaml.tmpl -> deploy/*.yaml by substituting shell
# vars from the env. Meant to be run after `source aws_account_info/simsc-env`.
# The rendered *.yaml files are gitignored (they contain account IDs).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"

# Verify required vars are set
required=(
    AWS_REGION AWS_ACCOUNT_ID
    ECR_IMAGE IMAGE_TAG OPENBW_SERVER_IMAGE
    K8S_NAMESPACE
    COGNITO_POOL_ARN COGNITO_CLIENT_ID COGNITO_DOMAIN COGNITO_REGION
    ACM_CERT_ARN
    SITE_ORIGIN SITE_HOST
    ADMIN_TOKEN_BASE64
)
for v in "${required[@]}"; do
    if [[ -z "${!v-}" ]]; then
        echo "render.sh: missing env var $v"
        echo "did you 'source aws_account_info/simsc-env'?"
        exit 1
    fi
done

for tmpl in "$HERE"/*.yaml.tmpl; do
    out="${tmpl%.tmpl}"
    envsubst < "$tmpl" > "$out"
    echo "rendered: $(basename "$out")"
done
