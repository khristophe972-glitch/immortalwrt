#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Tests de qos-mt6000.sh avec de faux tc, uci et logger : rien n'est applique
# au systeme. Usage : sh tests/test-qos-mt6000.sh [chemin/du/shell]
# (par defaut sh ; essayer aussi "busybox ash")

SH=${1:-sh}
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../files/usr/lib/qos-mt6000/qos-mt6000.sh
PASS=0
FAIL=0

setup() {
	T=$(mktemp -d)
	mkdir -p "$T/bin" "$T/sys/eth1" "$T/dbg"
	: > "$T/uci.conf"
	: > "$T/tc.log"
	cat > "$T/bin/uci" <<'X'
#!/bin/sh
[ "$1" = "-q" ] && shift
[ "$1" = get ] || exit 1
v=$(grep -m1 "^$2=" "$QOS_T/uci.conf" | cut -d= -f2-)
[ -n "$v" ] || exit 1
printf '%s\n' "$v"
X
	cat > "$T/bin/tc" <<'X'
#!/bin/sh
echo "$*" >> "$QOS_T/tc.log"
[ "$1 $2" = "qdisc show" ] && { [ -f "$QOS_T/state" ] && cat "$QOS_T/state"; exit 0; }
if [ "$1 $2" = "qdisc replace" ]; then
	[ -n "$TC_FAIL" ] && { echo "RTNETLINK answers: Invalid argument" >&2; exit 2; }
	dev=$4; type=$6; shift 6
	echo "qdisc $type 8001: root $*" > "$QOS_T/state"
	exit 0
fi
if [ "$1 $2" = "qdisc del" ]; then rm -f "$QOS_T/state"; exit 0; fi
exit 1
X
	printf '#!/bin/sh\nexit 0\n' > "$T/bin/logger"
	chmod +x "$T/bin/uci" "$T/bin/tc" "$T/bin/logger"
	for k in sync_time_ns active_window_ns active_release tin_share tin_hold tin_cap; do : > "$T/dbg/$k"; done
	export QOS_T="$T" QOS_TC="$T/bin/tc" QOS_UCI="$T/bin/uci" QOS_SYSNET="$T/sys" \
		QOS_DEBUGFS="$T/dbg" QOS_LOCKDIR="$T/lock" QOS_LOCK_TRIES=2 PATH="$T/bin:$PATH"
	unset TC_FAIL
}

teardown() { rm -rf "$T"; }
set_cfg() { printf 'qos-mt6000.global.%s=%s\n' "$1" "$2" >> "$T/uci.conf"; }
run() { OUT=$($SH "$SCRIPT" "$@" 2>&1); RC=$?; }

ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; echo "       sortie : $OUT"; }
expect_rc() { [ "$RC" = "$1" ] && ok "$2" || ko "$2 (rc=$RC, attendu $1)"; }
expect_out() { case "$OUT" in *"$1"*) ok "$2" ;; *) ko "$2 (absent : $1)" ;; esac; }
expect_no_tc() { [ ! -s "$T/tc.log" ] && ok "$1" || ko "$1 (tc appele : $(cat "$T/tc.log"))"; }
expect_tc() { grep -qF -- "$1" "$T/tc.log" && ok "$2" || ko "$2 (log tc : $(cat "$T/tc.log"))"; }

echo "[defauts]"
setup
run check
expect_rc 0 "check avec les valeurs par defaut"
expect_out "tc qdisc replace dev eth1 root cake_mq bandwidth 850Mbit diffserv4 nat triple-isolate wash ack-filter split-gso rtt 30ms noatm overhead 38 mpu 84" "commande identique au qdisc de production"
teardown

echo "[start nominal]"
setup
set_cfg sync_time_ns 200000
set_cfg active_window_ns 250000
set_cfg tin_share 0
set_cfg tin_hold 8
set_cfg tin_cap 0
run start
expect_rc 0 "start reussit"
expect_tc "qdisc replace dev eth1 root cake_mq bandwidth 850Mbit diffserv4 nat triple-isolate wash ack-filter split-gso rtt 30ms noatm overhead 38 mpu 84" "tc qdisc replace appele avec les bons arguments"
grep -q 'qdisc change' "$T/tc.log" && ko "jamais tc qdisc change" || ok "jamais tc qdisc change"
[ "$(cat "$T/dbg/sync_time_ns")" = 200000 ] && ok "debugfs sync_time_ns ecrit" || ko "debugfs sync_time_ns"
[ "$(cat "$T/dbg/active_window_ns")" = 250000 ] && ok "debugfs active_window_ns ecrit" || ko "debugfs active_window_ns"
[ "$(cat "$T/dbg/tin_hold")" = 8 ] && ok "debugfs tin_hold ecrit" || ko "debugfs tin_hold"
[ -s "$T/dbg/active_release" ] && ko "active_release non ecrit s'il est absent" || ok "active_release non ecrit s'il est absent"
[ ! -d "$T/lock" ] && ok "verrou libere" || ko "verrou libere"
run status
expect_out "== qdisc reel sur eth1 ==" "status affiche le qdisc reel"
expect_out "qdisc cake_mq" "status montre cake_mq"
teardown

echo "[variantes d'options]"
setup
set_cfg nat 0; set_cfg wash 0; set_cfg ack_filter none; set_cfg split_gso 0
set_cfg flow_isolation dual-srchost; set_cfg diffserv diffserv8
set_cfg rtt regional; set_cfg link_type ptm; set_cfg overhead -14; set_cfg mpu 64
set_cfg memlimit 32Mb; set_cfg bandwidth 2.5Gbit
run check
expect_out "bandwidth 2.5Gbit diffserv8 nonat dual-srchost nowash no-ack-filter no-split-gso rtt regional ptm overhead -14 mpu 64 memlimit 32Mb" "toutes les branches (nonat, nowash, no-ack-filter, no-split-gso, memlimit)"
teardown

setup
set_cfg bandwidth unlimited; set_cfg qdisc cake; set_cfg tin_cap 50
run check
expect_out "root cake bandwidth unlimited" "qdisc cake et bandwidth unlimited"
case "$OUT" in *"printf"*) ko "pas de reglage debugfs avec cake simple" ;; *) ok "pas de reglage debugfs avec cake simple" ;; esac
teardown

echo "[validation : valeurs invalides]"
for kv in "bandwidth abc" "bandwidth 850" "rtt 30" "rtt 30ms;touch/tmp/pwn" "diffserv foo" "flow_isolation x" "nat 2" "wash yes" \
	"ack_filter maybe" "split_gso 5" "link_type dsl" "overhead 999" "overhead abc" "mpu -1" "mpu 999" "memlimit 3x" \
	"device eth1;reboot" "device a/b" "qdisc htb" "tin_cap 101" "tin_share 2" "tin_hold 0" "sync_time_ns abc" "enabled maybe"; do
	setup
	set_cfg "${kv%% *}" "${kv#* }"
	run start
	expect_rc 1 "start refuse : $kv"
	expect_no_tc "  tc non appele pour : $kv"
	teardown
done

setup
set_cfg rtt 'abc; touch /tmp/qos-injection-test'
run start
[ ! -e /tmp/qos-injection-test ] && ok "aucune injection de commande" || { ko "injection de commande"; rm -f /tmp/qos-injection-test; }
teardown

echo "[enabled=0, interface absente, echec de tc]"
setup
set_cfg enabled 0
run start
expect_rc 0 "enabled=0 : succes sans rien faire"
expect_no_tc "enabled=0 : tc non appele"
teardown

setup
rmdir "$T/sys/eth1"
run start
expect_rc 1 "interface absente : echec"
expect_no_tc "interface absente : tc non appele"
teardown

setup
TC_FAIL=1 run start
expect_rc 1 "echec de tc : start echoue"
expect_out "echec de tc qdisc replace" "echec de tc : message clair"
teardown

echo "[verrou]"
setup
mkdir "$T/lock"
run start
expect_rc 1 "verrou occupe : abandon"
expect_no_tc "verrou occupe : tc non appele"
teardown

echo "[stop]"
setup
run start
: > "$T/tc.log"
run stop
expect_rc 0 "stop reussit"
expect_tc "qdisc del dev eth1 root" "qdisc racine supprime"
: > "$T/tc.log"
run stop
expect_rc 0 "stop sans qdisc : succes"
grep -q 'qdisc del' "$T/tc.log" && ko "stop sans qdisc : pas de suppression" || ok "stop sans qdisc : pas de suppression"
teardown

echo "[usage]"
setup
run bogus
expect_rc 1 "commande inconnue : usage et rc 1"
teardown

echo
echo "$PASS reussis, $FAIL en echec ($SH)"
[ "$FAIL" = 0 ]
