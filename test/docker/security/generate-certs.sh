#!/usr/bin/env bash
# Generates an ephemeral CA, server identity, and mTLS client identity.

set -Eeuo pipefail

if [[ -f /tls/ready ]]; then
  openssl verify \
    -CAfile /tls/ca.crt \
    /tls/server.crt \
    /tls/client.crt
  exit 0
fi

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /tls/ca.key \
  -out /tls/ca.crt \
  -sha256 -days 2 \
  -subj /CN=pg-status-test-ca

openssl req -newkey rsa:2048 -nodes \
  -keyout /tls/server.key \
  -out /tls/server.csr \
  -sha256 \
  -subj /CN=postgres-security \
  -addext subjectAltName=DNS:postgres-security
openssl x509 -req \
  -in /tls/server.csr \
  -CA /tls/ca.crt \
  -CAkey /tls/ca.key \
  -set_serial 1001 \
  -out /tls/server.crt \
  -days 2 -sha256 -copy_extensions copy

openssl req -newkey rsa:2048 -nodes \
  -keyout /tls/client.key \
  -out /tls/client.csr \
  -sha256 \
  -subj /CN=mtls_monitor \
  -addext extendedKeyUsage=clientAuth
openssl x509 -req \
  -in /tls/client.csr \
  -CA /tls/ca.crt \
  -CAkey /tls/ca.key \
  -set_serial 1002 \
  -out /tls/client.crt \
  -days 2 -sha256 -copy_extensions copy

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /tls/unknown-ca.key \
  -out /tls/unknown-ca.crt \
  -sha256 -days 2 \
  -subj /CN=pg-status-unknown-test-ca

openssl verify \
  -CAfile /tls/ca.crt \
  /tls/server.crt \
  /tls/client.crt
chown postgres:postgres /tls/server.key
chmod 0600 /tls/server.key /tls/client.key
chmod 0644 /tls/ca.crt /tls/server.crt /tls/client.crt /tls/unknown-ca.crt
rm -f /tls/ca.key /tls/unknown-ca.key /tls/server.csr /tls/client.csr
touch /tls/ready
