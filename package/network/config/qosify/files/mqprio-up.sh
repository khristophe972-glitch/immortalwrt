#!/bin/sh
# mqprio-up.sh — garantit que le root qdisc d'eth1 est mqprio en offload
# materiel (classes de priorite absolues -> files TX materielles).
#
# Pose le 12/09/2026. Idempotent : ne fait rien si mqprio est deja en place.
# Appele par /etc/rc.local (boot), /etc/hotplug.d/iface/13-mqprio (wan ifup)
# et le hook service_started de /etc/init.d/qosify (start/restart qosify).
#
# Pourquoi ces trois declencheurs : qosify ne fait que des `tc qdisc add` (son
# binaire ne contient aucun `replace`) -> une fois mqprio en place, ses ajouts
# echouent proprement (le root existe deja) et notre root survit. Le SEUL
# risque est son teardown `tc qdisc del dev eth1 root` (stop/restart de qosify
# ou retrait de device), d'ou le re-attache apres chaque start et chaque ifup.
#
# La map correspond a skb->priority = dscp >> 3 (patch qosify a896c2da07) :
# besteffort CS0 -> prio 0 -> TC0 ; bulk LE(8) -> prio 1 -> TC1 ;
# video AF41(34) -> prio 4 -> TC2 ; voice CS6(48) -> prio 6 -> TC3.
# NE PAS mettre video a l'index 5 : AF41 vaut 4, pas 5 (mesure du 12/09).
DEV=eth1
HANDLE=8001
MAP="0 1 0 0 2 0 3 0 0 0 0 0 0 0 0 0"

tc qdisc show dev "$DEV" | grep -q "mqprio $HANDLE:" && exit 0

logger -t mqprio-up "root $DEV n'est pas mqprio, attache"
tc qdisc del dev "$DEV" root 2>/dev/null
tc qdisc add dev "$DEV" root handle "$HANDLE:" mqprio num_tc 4 map $MAP queues 4@0 4@4 4@8 4@12 hw 1
rc=$?

if [ "$rc" -ne 0 ]; then
	logger -t mqprio-up "ECHEC attache mqprio sur $DEV (rc=$rc)"
else
	logger -t mqprio-up "mqprio attache sur $DEV (handle $HANDLE, map $MAP)"
fi
exit "$rc"
