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
if [ "$1 $2" = "qdisc add" ]; then
	[ -n "$TC_CLSACT_FAIL" ] && { echo "RTNETLINK answers: File exists" >&2; exit 2; }
	exit 0
fi
if [ "$1" = "filter" ]; then
	[ -n "$TC_FILTER_FAIL" ] && { echo "RTNETLINK answers: Invalid argument" >&2; exit 2; }
	exit 0
fi
exit 1
X
	printf '#!/bin/sh\nexit 0\n' > "$T/bin/logger"
	chmod +x "$T/bin/uci" "$T/bin/tc" "$T/bin/logger"
	for k in sync_time_ns active_window_ns active_release active_hold tin_share tin_hold tin_cap rebalance ewma_shift v1_headroom_shift v1_sat_shift v2_spare_pct; do : > "$T/dbg/$k"; done
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
set_cfg rebalance 2
run start
expect_rc 0 "start reussit"
expect_tc "qdisc replace dev eth1 root cake_mq bandwidth 850Mbit diffserv4 nat triple-isolate wash ack-filter split-gso rtt 30ms noatm overhead 38 mpu 84" "tc qdisc replace appele avec les bons arguments"
grep -q 'qdisc change' "$T/tc.log" && ko "jamais tc qdisc change" || ok "jamais tc qdisc change"
[ "$(cat "$T/dbg/sync_time_ns")" = 200000 ] && ok "debugfs sync_time_ns ecrit" || ko "debugfs sync_time_ns"
[ "$(cat "$T/dbg/active_window_ns")" = 250000 ] && ok "debugfs active_window_ns ecrit" || ko "debugfs active_window_ns"
[ "$(cat "$T/dbg/tin_hold")" = 8 ] && ok "debugfs tin_hold ecrit" || ko "debugfs tin_hold"
[ "$(cat "$T/dbg/rebalance")" = 2 ] && ok "debugfs rebalance ecrit" || ko "debugfs rebalance"
[ -s "$T/dbg/active_release" ] && ko "active_release non ecrit s'il est absent" || ok "active_release non ecrit s'il est absent"
[ ! -d "$T/lock" ] && ok "verrou libere" || ko "verrou libere"
run status
expect_out "== qdisc reel sur eth1 ==" "status affiche le qdisc reel"
expect_out "qdisc cake_mq" "status montre cake_mq"
teardown

echo "[reglages de tuning : knobs neufs, valeurs aux bornes acceptees]"
setup
set_cfg active_release 1
set_cfg active_hold 2
set_cfg ewma_shift 8
set_cfg v1_headroom_shift 1
set_cfg v1_sat_shift 8
set_cfg v2_spare_pct 100
run start
expect_rc 0 "start accepte les knobs neufs"
[ "$(cat "$T/dbg/active_release")" = 1 ] && ok "debugfs active_release ecrit" || ko "debugfs active_release"
[ "$(cat "$T/dbg/active_hold")" = 2 ] && ok "debugfs active_hold ecrit" || ko "debugfs active_hold"
[ "$(cat "$T/dbg/ewma_shift")" = 8 ] && ok "debugfs ewma_shift ecrit (borne haute)" || ko "debugfs ewma_shift"
[ "$(cat "$T/dbg/v1_headroom_shift")" = 1 ] && ok "debugfs v1_headroom_shift ecrit (borne basse)" || ko "debugfs v1_headroom_shift"
[ "$(cat "$T/dbg/v1_sat_shift")" = 8 ] && ok "debugfs v1_sat_shift ecrit (borne haute)" || ko "debugfs v1_sat_shift"
[ "$(cat "$T/dbg/v2_spare_pct")" = 100 ] && ok "debugfs v2_spare_pct ecrit (borne haute)" || ko "debugfs v2_spare_pct"
teardown

echo "[rebalance : 0, 1 et 2 acceptes, 3 refuse]"
for m in 0 1 2; do
	setup
	set_cfg rebalance "$m"
	run start
	expect_rc 0 "rebalance $m accepte"
	[ "$(cat "$T/dbg/rebalance")" = "$m" ] && ok "debugfs rebalance ecrit ($m)" || ko "debugfs rebalance ($m)"
	teardown
done

echo "[sync_time_ns et active_hold : cadence et produit bornes]"
setup
set_cfg active_hold 0
run start
expect_rc 0 "active_hold 0 (defaut amont) accepte"
teardown

setup
set_cfg sync_time_ns 1
set_cfg active_hold 1000000000
run start
expect_rc 0 "sync_time_ns 1 et active_hold 1000000000 acceptes"
teardown

setup
set_cfg sync_time_ns 1000000000
set_cfg active_hold 1
run start
expect_rc 0 "sync_time_ns 1000000000 (borne haute) accepte"
teardown

setup
set_cfg sync_time_ns 200000
set_cfg active_hold 5000
run start
expect_rc 0 "active_hold x sync_time_ns = 1000000000 ns, borne exacte acceptee"
teardown

setup
set_cfg sync_time_ns 200000
set_cfg active_hold 5001
run start
expect_rc 1 "active_hold x sync_time_ns > 1000000000 ns refuse"
expect_no_tc "  tc non appele pour le produit hors borne"
teardown

setup
set_cfg sync_time_ns 0200000
set_cfg active_hold 5001
run start
expect_rc 1 "sync_time_ns 0200000 lu en decimal (200000), pas en octal : produit refuse"
expect_no_tc "  tc non appele (sync_time_ns a zero de tete)"
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
	"device eth1;reboot" "device a/b" "qdisc htb" "tin_cap 101" "tin_share 2" "tin_hold 0" "rebalance 3" "sync_time_ns abc" \
	"ewma_shift abc" "ewma_shift 0" "ewma_shift 9" "v1_headroom_shift 0" "v1_sat_shift 9" "v2_spare_pct 101" \
	"sync_time_ns 0" "sync_time_ns 1000000001" "sync_time_ns 99999999999999999999" \
	"active_hold 1000000001" "active_hold 99999999999999999999" "enabled maybe"; do
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

echo "[ecn : keep par defaut]"
setup
run start
expect_rc 0 "start sans options ECN"
grep -q 'clsact' "$T/tc.log" && ko "aucun clsact par defaut" || ok "aucun clsact par defaut"
grep -q 'filter add' "$T/tc.log" && ko "aucun filtre add par defaut" || ok "aucun filtre add par defaut"
teardown

echo "[ecn : clear en egress]"
setup
set_cfg ecn_egress clear
run start
expect_rc 0 "start avec ecn_egress=clear"
expect_tc "qdisc add dev eth1 clsact" "clsact pose"
expect_tc "filter add dev eth1 egress prio 1 protocol ip u32 match u32 0 0 action pedit ex munge offset 0 u32 clear retain 0x00030000 pipe action csum iph" "filtre IPv4 egress"
expect_tc "filter add dev eth1 egress prio 1 protocol ipv6 matchall action pedit ex munge offset 0 u32 clear retain 0x00300000" "filtre IPv6 egress"
grep -q 'filter add dev eth1 ingress' "$T/tc.log" && ko "aucun filtre ingress" || ok "aucun filtre ingress"
teardown

echo "[ecn : clear dans les deux sens]"
setup
set_cfg ecn_egress clear
set_cfg ecn_ingress clear
run start
expect_rc 0 "start avec les deux sens a clear"
expect_tc "filter add dev eth1 ingress prio 1 protocol ip u32" "filtre IPv4 ingress"
expect_tc "filter add dev eth1 ingress prio 1 protocol ipv6 matchall" "filtre IPv6 ingress"
teardown

echo "[ecn : valeur invalide refusee]"
setup
set_cfg ecn_egress effacer
run start
expect_rc 1 "ecn_egress invalide : rc 1"
expect_out "ecn_egress invalide" "message d'erreur"
teardown

echo "[ecn : echec du filtre = fail-open]"
setup
set_cfg ecn_egress clear
TC_FILTER_FAIL=1
export TC_FILTER_FAIL
run start
expect_rc 0 "filtre en echec : start reussit quand meme"
expect_out "non pose" "echec journalise"
expect_tc "qdisc replace dev eth1 root cake_mq" "le qdisc racine est bien pose"
unset TC_FILTER_FAIL
teardown

echo "[ecn : stop retire les filtres]"
setup
set_cfg ecn_egress clear
run start
: > "$T/tc.log"
run stop
expect_rc 0 "stop reussit"
expect_tc "filter del dev eth1 egress prio 1" "filtres egress retires"
expect_tc "filter del dev eth1 ingress prio 1" "filtres ingress retires"
teardown

echo "[ecn : retour a keep efface les filtres]"
setup
set_cfg ecn_egress clear
run start
: > "$T/tc.log"
: > "$T/uci.conf"
set_cfg ecn_egress keep
run start
expect_rc 0 "start apres retour a keep"
expect_tc "filter del dev eth1 egress prio 1" "filtres retires au passage a keep"
grep -q 'filter add' "$T/tc.log" && ko "aucun filtre repose" || ok "aucun filtre repose"
teardown

echo "[usage]"
setup
run bogus
expect_rc 1 "commande inconnue : usage et rc 1"
teardown

echo
echo "$PASS reussis, $FAIL en echec ($SH)"
[ "$FAIL" = 0 ]
