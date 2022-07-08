#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# vi: ts=4 sw=4 tw=0 et:

# TODO:
#   - IPv6-only stack
#   - mDNS
#   - LLMNR
#   - DoT/DoH

set -eux
set -o pipefail

: >/failed

RUN_OUT="$(mktemp)"

run() {
    "$@" |& tee "$RUN_OUT"
}

disable_ipv6() {
    sysctl -w net.ipv6.conf.all.disable_ipv6=1
}

enable_ipv6() {
    sysctl -w net.ipv6.conf.all.disable_ipv6=0
    networkctl reconfigure dns0
}

### SETUP ###
# Configure network
hostnamectl hostname ns1.unsigned.test
{
    echo "10.0.0.1               ns1.unsigned.test"
    echo "fd00:dead:beef:cafe::1 ns1.unsigned.test"
} >>/etc/hosts

mkdir -p /etc/systemd/network
cat >/etc/systemd/network/dns0.netdev <<EOF
[NetDev]
Name=dns0
Kind=dummy
EOF
cat >/etc/systemd/network/dns0.network <<EOF
[Match]
Name=dns0

[Network]
Address=10.0.0.1/24
Address=fd00:dead:beef:cafe::1/64
DNSSEC=allow-downgrade
DNS=10.0.0.1
DNS=fd00:dead:beef:cafe::1
EOF

DNS_ADDRESSES=(
    "10.0.0.1"
    "fd00:dead:beef:cafe::1"
)

{
    echo "FallbackDNS="
    echo "DNSSEC=allow-downgrade"
    echo "DNSOverTLS=opportunistic"
} >>/etc/systemd/resolved.conf
ln -svf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
# Override the default NTA list, which turns off DNSSEC validation for (among
# others) the test. domain
mkdir -p "/etc/dnssec-trust-anchors.d/"
echo local >/etc/dnssec-trust-anchors.d/local.negative

# Sign the root zone
keymgr . generate algorithm=ECDSAP256SHA256 ksk=yes zsk=yes
# Create a trust anchor for resolved with our root zone
keymgr . ds | sed 's/ DS/ IN DS/g' >/etc/dnssec-trust-anchors.d/root.positive
# Create a bind-compatible trust anchor (for delv)
# Note: the trust-anchors directive is relatively new, so use the original
#       managed-keys one until it's widespread enough
{
    echo 'managed-keys {'
    keymgr . dnskey | sed -r 's/^\. DNSKEY ([0-9]+ [0-9]+ [0-9]+) (.+)$/. static-key \1 "\2";/g'
    echo '};'
} >/etc/bind.keys

# Start the services
systemctl unmask systemd-networkd systemd-resolved
systemctl start systemd-networkd systemd-resolved
systemctl start knot
# Wait a bit for the keys to propagate
sleep 4

networkctl status
resolvectl status
resolvectl log-level debug

# Check if all the zones are valid (zone-check always returns 0, so let's check
# if it produces any errors/warnings)
(! knotc zone-check |& grep .)
# We need to manually propagate the DS records of onlinesign.test. to the parent
# zone, since they're generated online
knotc zone-begin test.
while read -ra line; do
    knotc zone-set test. "${line[@]}"
done < <(keymgr onlinesign.test ds)
knotc zone-commit test.

### SETUP END ###

: "--- nss-resolve/nss-myhostname tests"
# Sanity check
# Issue: https://github.com/systemd/systemd/issues/23951
# With IPv6 enabled
run getent -s resolve hosts ns1.unsigned.test
grep -qE "^fd00:dead:beef:cafe::1\s+ns1\.unsigned\.test" "$RUN_OUT"
# With IPv6 disabled
disable_ipv6
run getent -s resolve hosts ns1.unsigned.test
# FIXME
grep -qE "^10\.0\.0\.1\s+ns1\.unsigned\.test" "$RUN_OUT"
enable_ipv6

# Issue: https://github.com/systemd/systemd/issues/18812
# PR: https://github.com/systemd/systemd/pull/18896
# Follow-up issue: https://github.com/systemd/systemd/issues/23152
# Follow-up PR: https://github.com/systemd/systemd/pull/23161
# With IPv6 enabled
run getent -s resolve hosts localhost
grep -qE "^::1\s+localhost" "$RUN_OUT"
run getent -s myhostname hosts localhost
grep -qE "^::1\s+localhost" "$RUN_OUT"
# With IPv6 disabled
disable_ipv6
run getent -s resolve hosts localhost
grep -qE "^127\.0\.0\.1\s+localhost" "$RUN_OUT"
run getent -s myhostname hosts localhost
grep -qE "^127\.0\.0\.1\s+localhost" "$RUN_OUT"
enable_ipv6

: "--- Basic resolved tests ---"
# Issue: https://github.com/systemd/systemd/issues/22229
# PR: https://github.com/systemd/systemd/pull/22231
FILTERED_NAMES=(
    "0.in-addr.arpa"
    "255.255.255.255.in-addr.arpa"
    "0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa"
    "hello.invalid"
)

for name in "${FILTERED_NAMES[@]}"; do
    (! run host "$name")
    grep -qF "NXDOMAIN" "$RUN_OUT"
done

# Follow-up
# Issue: https://github.com/systemd/systemd/issues/22401
# PR: https://github.com/systemd/systemd/pull/22414
run dig +noall +authority +comments SRV .
grep -qF "status: NOERROR" "$RUN_OUT"
grep -qE "IN\s+SOA\s+ns1\.unsigned\.test\." "$RUN_OUT"


: "--- ZONE: unsigned.test. ---"
run dig @ns1.unsigned.test +short unsigned.test A unsigned.test AAAA
grep -qF "10.0.0.101" "$RUN_OUT"
grep -qF "fd00:dead:beef:cafe::101" "$RUN_OUT"
run resolvectl query unsigned.test
grep -qF "10.0.0.10" "$RUN_OUT"
grep -qF "fd00:dead:beef:cafe::101" "$RUN_OUT"
grep -qF "authenticated: no" "$RUN_OUT"
run dig @ns1.unsigned.test +short MX unsigned.test
grep -qF "15 mail.unsigned.test." "$RUN_OUT"
run resolvectl query --legend=no -t MX unsigned.test
grep -qF "unsigned.test IN MX 15 mail.unsigned.test" "$RUN_OUT"


: "--- ZONE: signed.systemd (static DNSSEC) ---"
# Check the trust chain (with and without systemd-resolved in between
# Issue: https://github.com/systemd/systemd/issues/22002
# PR: https://github.com/systemd/systemd/pull/23289
run delv @ns1.unsigned.test signed.test
grep -qF "; fully validated" "$RUN_OUT"
run delv signed.test
grep -qF "; fully validated" "$RUN_OUT"

for addr in "${DNS_ADDRESSES[@]}"; do
    run delv "@$addr" -t A mail.signed.test
    grep -qF "; fully validated" "$RUN_OUT"
    run delv "@$addr" -t AAAA mail.signed.test
    grep -qF "; fully validated" "$RUN_OUT"
done
run resolvectl query mail.signed.test
grep -qF "10.0.0.11" "$RUN_OUT"
grep -qF "fd00:dead:beef:cafe::11" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"

run dig +short signed.test
grep -qF "10.0.0.10" "$RUN_OUT"
run resolvectl query signed.test
grep -qF "signed.test: 10.0.0.10" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"
run dig @ns1.unsigned.test +short MX signed.test
grep -qF "10 mail.signed.test." "$RUN_OUT"
run resolvectl query --legend=no -t MX signed.test
grep -qF "signed.test IN MX 10 mail.signed.test" "$RUN_OUT"
# Check a non-existent domain
run dig +dnssec this.does.not.exist.signed.test
grep -qF "status: NXDOMAIN" "$RUN_OUT"
# Check a wildcard record
run resolvectl query -t TXT this.should.be.authenticated.wild.signed.test
grep -qF 'this.should.be.authenticated.wild.signed.test IN TXT "this is a wildcard"' "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"

# DNSSEC validation with multiple records of the same type for the same name
# Issue: https://github.com/systemd/systemd/issues/22002
# PR: https://github.com/systemd/systemd/pull/23289
check_domain() {
    local domain="${1:?}"
    local record="${2:?}"
    local message="${3:?}"
    local addr

    for addr in "${DNS_ADDRESSES[@]}"; do
        run delv "@$addr" -t "$record" "$domain"
        grep -qF "$message" "$RUN_OUT"
    done

    run delv -t "$record" "$domain"
    grep -qF "$message" "$RUN_OUT"

    run resolvectl query "$domain"
    grep -qF "authenticated: yes" "$RUN_OUT"
}

check_domain "dupe.signed.test"       "A"    "; fully validated"
check_domain "dupe.signed.test"       "AAAA" "; negative response, fully validated"
check_domain "dupe-ipv6.signed.test"  "AAAA" "; fully validated"
check_domain "dupe-ipv6.signed.test"  "A"    "; negative response, fully validated"
check_domain "dupe-mixed.signed.test" "A"    "; fully validated"
check_domain "dupe-mixed.signed.test" "AAAA" "; fully validated"

# Test resolution of CNAME chains
run resolvectl query -t A cname-chain.signed.test
grep -qF "follow14.final.signed.test IN A 10.0.0.14" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"
# Non-existing RR + CNAME chain
run dig +dnssec AAAA cname-chain.signed.test
grep -qF "status: NOERROR" "$RUN_OUT"
grep -qE "^follow14\.final\.signed\.test\..+IN\s+NSEC\s+" "$RUN_OUT"


: "--- ZONE: onlinesign.test (dynamic DNSSEC) ---"
# Check the trust chain (with and without systemd-resolved in between
# Issue: https://github.com/systemd/systemd/issues/22002
# PR: https://github.com/systemd/systemd/pull/23289
run delv @ns1.unsigned.test sub.onlinesign.test
grep -qF "; fully validated" "$RUN_OUT"
run delv sub.onlinesign.test
grep -qF "; fully validated" "$RUN_OUT"

run dig +short sub.onlinesign.test
grep -qF "10.0.0.133" "$RUN_OUT"
run resolvectl query sub.onlinesign.test
grep -qF "sub.onlinesign.test: 10.0.0.133" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"
run dig @ns1.unsigned.test +short TXT onlinesign.test
grep -qF '"hello from onlinesign"' "$RUN_OUT"
run resolvectl query --legend=no -t TXT onlinesign.test
grep -qF 'onlinesign.test IN TXT "hello from onlinesign"' "$RUN_OUT"

for addr in "${DNS_ADDRESSES[@]}"; do
    run delv "@$addr" -t A dual.onlinesign.test
    grep -qF "10.0.0.134" "$RUN_OUT"
    run delv "@$addr" -t AAAA dual.onlinesign.test
    grep -qF "fd00:dead:beef:cafe::134" "$RUN_OUT"
    run delv "@$addr" -t ANY ipv6.onlinesign.test
    grep -qF "fd00:dead:beef:cafe::135" "$RUN_OUT"
done
run resolvectl query dual.onlinesign.test
grep -qF "10.0.0.134" "$RUN_OUT"
grep -qF "fd00:dead:beef:cafe::134" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"
run resolvectl query ipv6.onlinesign.test
grep -qF "fd00:dead:beef:cafe::135" "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"

# Check a non-existent domain
# Note: mod-onlinesign utilizes Minimally Covering NSEC Records, hence the
#       different response than with "standard" DNSSEC
run dig +dnssec this.does.not.exist.onlinesign.test
grep -qF "status: NOERROR" "$RUN_OUT"
grep -qF "NSEC \\000.this.does.not.exist.onlinesign.test." "$RUN_OUT"
# Check a wildcard record
run resolvectl query -t TXT this.should.be.authenticated.wild.onlinesign.test
grep -qF 'this.should.be.authenticated.wild.onlinesign.test IN TXT "this is an onlinesign wildcard"' "$RUN_OUT"
grep -qF "authenticated: yes" "$RUN_OUT"


: "--- ZONE: untrusted.test (DNSSEC without propagated DS records) ---"
# Issue: https://github.com/systemd/systemd/issues/23955
# FIXME
run dig +short untrusted.test A untrusted.test AAAA
grep -qF "10.0.0.121" "$RUN_OUT"
grep -qF "fd00:dead:beef:cafe::121" "$RUN_OUT"
run resolvectl query untrusted.test
grep -qF "untrusted.test: 10.0.0.121" "$RUN_OUT"
grep -qF "untrusted.test: fd00:dead:beef:cafe::121" "$RUN_OUT"
grep -qF "authenticated: no" "$RUN_OUT"

# Issue: https://github.com/systemd/systemd/issues/19472
# 1) Query for a non-existing RR should return NOERROR + NSEC (?), not NXDOMAIN
# FIXME: re-enable once the issue is resolved
#run dig +dnssec AAAA untrusted.test
#grep -qF "status: NOERROR" "$RUN_OUT"
#grep -qE "^untrusted\.test\..+IN\s+NSEC\s+" "$RUN_OUT"
## 2) Query for a non-existing name should return NXDOMAIN, not SERVFAIL
#run dig +dnssec this.does.not.exist.untrusted.test
#grep -qF "status: NXDOMAIN" "$RUN_OUT"


touch /testok
rm /failed
