#!/bin/bash

SKIP_RC=4
RC=0

nft --version >/dev/null 2>&1 || {
	echo "SKIP: missing nft tool"
	exit $SKIP_RC
}

auditd --help >/dev/null 2>&1
[ $? -eq 2 ] || {
	echo "SKIP: missing auditd tool"
	exit $SKIP_RC
}

tmpdir=$(mktemp -d)
audit_log="$tmpdir/audit.log"
cat >"$tmpdir/auditd.conf" <<EOF
write_logs = no
space_left = 75
EOF
auditd -f -c "$tmpdir" >"$audit_log" &
audit_pid=$!
trap 'kill $audit_pid; rm -rf $tmpdir' EXIT
sleep 1

logread() {
	grep 'type=NETFILTER_CFG' "$audit_log" | \
		sed -e 's/\(type\|msg\|pid\)=[^ ]* //g' \
		    -e 's/\(table=[^:]*\):[0-9]*/\1/'
}

do_test() { # (cmd, log)
	echo -n "testing for cmd: $1 ... "
	echo >"$audit_log"
	$1 >/dev/null || exit 1
	diff -q <(echo "$2") <(logread) >/dev/null && { echo "OK"; return; }
	echo "FAIL"
	diff -u <(echo "$2") <(logread)
	((RC++))
}

nft flush ruleset

for table in t1 t2; do
	echo "add table $table"
	for chain in c1 c2 c3; do
		echo "add chain $table $chain"
		echo "add rule $table $chain counter accept"
		echo "add rule $table $chain counter accept"
		echo "add rule $table $chain counter accept"
	done
done | nft -f - || exit 1

do_test 'nft reset rules t1 c2' \
	'table=t1 family=2 entries=3 op=nft_reset_rule subj=kernel comm="nft"'

do_test 'nft reset rules table t1' \
	'table=t1 family=2 entries=9 op=nft_reset_rule subj=kernel comm="nft"'

do_test 'nft reset rules' \
	'table=t1 family=2 entries=9 op=nft_reset_rule subj=kernel comm="nft"
table=t2 family=2 entries=9 op=nft_reset_rule subj=kernel comm="nft"'

for ((i = 0; i < 500; i++)); do
	echo "add rule t2 c3 counter accept comment \"rule $i\""
done | nft -f - || exit 1

do_test 'nft reset rules t2 c3' \
	'table=t2 family=2 entries=189 op=nft_reset_rule subj=kernel comm="nft"
table=t2 family=2 entries=188 op=nft_reset_rule subj=kernel comm="nft"
table=t2 family=2 entries=126 op=nft_reset_rule subj=kernel comm="nft"'

exit $RC
