#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# qos-mt6000.sh : gere le qdisc racine cake_mq du port WAN a partir de
# /etc/config/qos-mt6000.
#
# usage : qos-mt6000.sh start | stop | check | status
#   start  : valide la configuration, remplace le qdisc racine (tc qdisc
#            replace, jamais change), applique les reglages debugfs de
#            cake_mq, puis verifie le resultat
#   stop   : supprime le qdisc racine du port (retour au qdisc par defaut)
#   check  : valide la configuration et affiche les commandes, sans rien
#            appliquer (dry-run)
#   status : affiche la configuration, le qdisc reel et les reglages debugfs
#
# Toutes les valeurs lues dans UCI sont validees avant d'etre utilisees dans
# une commande : une valeur invalide arrete l'operation sans rien modifier.

# shellcheck disable=SC3043
NAME=qos-mt6000
TC=${QOS_TC:-tc}
UCI=${QOS_UCI:-uci}
DEBUGFS=${QOS_DEBUGFS:-/sys/kernel/debug/cake_mq}
SYSNET=${QOS_SYSNET:-/sys/class/net}
LOCKDIR=${QOS_LOCKDIR:-/tmp/qos-mt6000.lock}
LOCK_TRIES=${QOS_LOCK_TRIES:-15}
KNOBS="sync_time_ns active_window_ns active_release tin_share tin_hold tin_cap"

log() {
	logger -t "$NAME" "$*" 2> /dev/null
	echo "$NAME: $*" >&2
}

cfg() {
	"$UCI" -q get "$NAME.global.$1" 2> /dev/null
}

get() {
	local v
	v=$(cfg "$1")
	printf '%s\n' "${v:-$2}"
}

load_config() {
	ENABLED=$(get enabled 1)
	DEV=$(get device eth1)
	QDISC=$(get qdisc cake_mq)
	BW=$(get bandwidth 850Mbit)
	DIFF=$(get diffserv diffserv4)
	FLOW=$(get flow_isolation triple-isolate)
	NAT=$(get nat 1)
	WASH=$(get wash 1)
	ACK=$(get ack_filter ack-filter)
	SPLIT=$(get split_gso 1)
	RTT=$(get rtt 30ms)
	LINK=$(get link_type noatm)
	OVH=$(get overhead 38)
	MPU=$(get mpu 84)
	MEMLIMIT=$(cfg memlimit)
}

is_uint() {
	case "$1" in
	'' | *[!0-9]*) return 1 ;;
	esac
	return 0
}

is_int() {
	case "$1" in
	-*) is_uint "${1#-}" ;;
	*) is_uint "$1" ;;
	esac
}

is_bool() {
	case "$1" in
	0 | 1) return 0 ;;
	esac
	return 1
}

is_ifname() {
	printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9_.@-]{1,15}$'
}

is_rate() {
	[ "$1" = unlimited ] && return 0
	printf '%s\n' "$1" | grep -Eq '^[0-9]+(\.[0-9]+)?[kKmMgGtT]?bit$'
}

is_rtt() {
	case "$1" in
	datacentre | lan | metro | regional | internet | oceanic | satellite | interplanetary) return 0 ;;
	esac
	printf '%s\n' "$1" | grep -Eq '^[0-9]+(us|ms|s)$'
}

is_memlimit() {
	printf '%s\n' "$1" | grep -Eq '^[0-9]+([kKmMgG][bB]?)?$'
}

# validate : verifie toutes les valeurs, signale chacune de celles qui sont
# invalides, et renvoie 1 s'il y en a au moins une.
validate() {
	local err=0 k v

	is_ifname "$DEV" || { log "device invalide : '$DEV'"; err=1; }
	case "$QDISC" in
	cake_mq | cake) ;;
	*) log "qdisc invalide : '$QDISC' (cake_mq ou cake)"; err=1 ;;
	esac
	is_rate "$BW" || { log "bandwidth invalide : '$BW' (ex. 850Mbit ou unlimited)"; err=1; }
	case "$DIFF" in
	besteffort | diffserv3 | diffserv4 | diffserv8 | precedence) ;;
	*) log "diffserv invalide : '$DIFF'"; err=1 ;;
	esac
	case "$FLOW" in
	flowblind | srchost | dsthost | hosts | flows | dual-srchost | dual-dsthost | triple-isolate) ;;
	*) log "flow_isolation invalide : '$FLOW'"; err=1 ;;
	esac
	is_bool "$NAT" || { log "nat invalide : '$NAT' (0 ou 1)"; err=1; }
	is_bool "$WASH" || { log "wash invalide : '$WASH' (0 ou 1)"; err=1; }
	case "$ACK" in
	none | ack-filter | ack-filter-aggressive) ;;
	*) log "ack_filter invalide : '$ACK'"; err=1 ;;
	esac
	is_bool "$SPLIT" || { log "split_gso invalide : '$SPLIT' (0 ou 1)"; err=1; }
	is_rtt "$RTT" || { log "rtt invalide : '$RTT' (ex. 30ms ou regional)"; err=1; }
	case "$LINK" in
	noatm | atm | ptm) ;;
	*) log "link_type invalide : '$LINK' (noatm, atm ou ptm)"; err=1 ;;
	esac
	if is_int "$OVH" && [ "$OVH" -ge -64 ] && [ "$OVH" -le 256 ]; then :; else
		log "overhead invalide : '$OVH' (entier de -64 a 256)"
		err=1
	fi
	if is_uint "$MPU" && [ "$MPU" -le 256 ]; then :; else
		log "mpu invalide : '$MPU' (entier de 0 a 256)"
		err=1
	fi
	if [ -n "$MEMLIMIT" ] && ! is_memlimit "$MEMLIMIT"; then
		log "memlimit invalide : '$MEMLIMIT' (ex. 32Mb)"
		err=1
	fi

	for k in $KNOBS; do
		v=$(cfg "$k")
		[ -n "$v" ] || continue
		if ! is_uint "$v"; then
			log "$k invalide : '$v' (entier positif)"
			err=1
			continue
		fi
		case "$k" in
		tin_share)
			[ "$v" -le 1 ] || { log "tin_share invalide : '$v' (0 ou 1)"; err=1; }
			;;
		tin_hold)
			[ "$v" -ge 1 ] || { log "tin_hold invalide : '$v' (au moins 1)"; err=1; }
			;;
		tin_cap)
			[ "$v" -le 100 ] || { log "tin_cap invalide : '$v' (0 a 100)"; err=1; }
			;;
		esac
	done

	return "$err"
}

build_args() {
	ARGS="bandwidth $BW $DIFF"
	if [ "$NAT" = 1 ]; then ARGS="$ARGS nat"; else ARGS="$ARGS nonat"; fi
	ARGS="$ARGS $FLOW"
	if [ "$WASH" = 1 ]; then ARGS="$ARGS wash"; else ARGS="$ARGS nowash"; fi
	case "$ACK" in
	none) ARGS="$ARGS no-ack-filter" ;;
	*) ARGS="$ARGS $ACK" ;;
	esac
	if [ "$SPLIT" = 1 ]; then ARGS="$ARGS split-gso"; else ARGS="$ARGS no-split-gso"; fi
	ARGS="$ARGS rtt $RTT $LINK overhead $OVH mpu $MPU"
	[ -z "$MEMLIMIT" ] || ARGS="$ARGS memlimit $MEMLIMIT"
}

lock() {
	local i=0
	while ! mkdir "$LOCKDIR" 2> /dev/null; do
		i=$((i + 1))
		if [ "$i" -ge "$LOCK_TRIES" ]; then
			log "verrou $LOCKDIR occupe, abandon"
			return 1
		fi
		sleep 1
	done
	trap 'rmdir "$LOCKDIR" 2> /dev/null' EXIT INT TERM
}

current_qdisc() {
	"$TC" qdisc show dev "$DEV" root 2> /dev/null | head -n 1
}

apply_knobs() {
	local k v f
	[ "$QDISC" = cake_mq ] || return 0
	if [ -z "$QOS_DEBUGFS" ] && [ ! -d "$DEBUGFS" ]; then
		mount -t debugfs none /sys/kernel/debug 2> /dev/null
	fi
	for k in $KNOBS; do
		v=$(cfg "$k")
		[ -n "$v" ] || continue
		f="$DEBUGFS/$k"
		if [ ! -w "$f" ]; then
			log "reglage $k ignore : $f absent ou en lecture seule"
			continue
		fi
		printf '%s' "$v" > "$f" || log "ecriture de $k=$v echouee"
	done
}

do_start() {
	local out cur
	load_config
	is_bool "$ENABLED" || { log "enabled invalide : '$ENABLED' (0 ou 1)"; return 1; }
	if [ "$ENABLED" = 0 ]; then
		log "desactive (enabled=0), qdisc non modifie"
		return 0
	fi
	validate || return 1
	build_args
	if [ ! -d "$SYSNET/$DEV" ]; then
		log "interface $DEV absente, qdisc non modifie"
		return 1
	fi
	# shellcheck disable=SC2086
	if ! out=$("$TC" qdisc replace dev "$DEV" root "$QDISC" $ARGS 2>&1); then
		log "echec de tc qdisc replace : $out"
		return 1
	fi
	cur=$(current_qdisc)
	case "$cur" in
	"qdisc $QDISC "*) ;;
	*)
		log "verification echouee, qdisc racine de $DEV : '$cur'"
		return 1
		;;
	esac
	apply_knobs
	log "actif sur $DEV : $QDISC $ARGS"
	return 0
}

do_stop() {
	local cur
	load_config
	is_ifname "$DEV" || { log "device invalide : '$DEV'"; return 1; }
	cur=$(current_qdisc)
	case "$cur" in
	"qdisc $QDISC "*)
		"$TC" qdisc del dev "$DEV" root && log "qdisc racine supprime sur $DEV"
		;;
	*)
		log "pas de qdisc $QDISC racine sur $DEV, rien a supprimer"
		;;
	esac
	return 0
}

do_check() {
	local k v
	load_config
	is_bool "$ENABLED" || { log "enabled invalide : '$ENABLED' (0 ou 1)"; return 1; }
	if [ "$ENABLED" = 0 ]; then
		echo "desactive (enabled=0) : aucune commande"
		return 0
	fi
	validate || return 1
	build_args
	echo "tc qdisc replace dev $DEV root $QDISC $ARGS"
	if [ "$QDISC" = cake_mq ]; then
		for k in $KNOBS; do
			v=$(cfg "$k")
			[ -z "$v" ] || echo "printf '$v' > $DEBUGFS/$k"
		done
	fi
	return 0
}

do_status() {
	local k
	load_config
	echo "== configuration =="
	do_check
	echo "== qdisc reel sur $DEV =="
	current_qdisc
	echo "== reglages debugfs =="
	for k in $KNOBS; do
		if [ -r "$DEBUGFS/$k" ]; then
			echo "$k=$(cat "$DEBUGFS/$k")"
		fi
	done
	return 0
}

usage() {
	echo "usage : $0 start | stop | check | status" >&2
}

case "$1" in
start)
	lock && do_start
	exit $?
	;;
stop)
	lock && do_stop
	exit $?
	;;
check)
	do_check
	exit $?
	;;
status)
	do_status
	exit $?
	;;
*)
	usage
	exit 1
	;;
esac
