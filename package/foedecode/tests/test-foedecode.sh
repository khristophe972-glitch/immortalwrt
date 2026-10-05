#!/bin/sh
# Tests hors-ligne de foedecode.
#
# Les lignes d'entrée suivent le FORMAT RÉEL de mtk_ppe_debugfs.c (noyau 6.18.52) :
#   seq_printf(m, "%05x %s %7s", index, state, type)             (l.103)
#   " ppe=%d" (patch fork 999-3001, inséré après le type)
#   " orig=…" / " new=…"                                          (l.129-143)
#   " eth=%pM->%pM etype=%04x vlan=%d,%d ib1=%08x ib2=%08x"
#   " packets=%llu bytes=%llu\n"                                  (l.159-164)
#
# ⚠️ Les valeurs ib2 sont SYNTHÉTIQUES (fabriquées pour exercer les masques) ;
# les adresses sont ANONYMISÉES (192.168.8.x côté LAN, 203.0.113.x /
# 2001:db8::/32 côté public, RFC 5737/3849). États (BND/UNB), types ("IPv4 5T"
# etc.) et ordre des champs reproduisent le format réel.
set -eu

BIN=${1:-$(dirname "$0")/../files/usr/bin/foedecode}
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

fail=0
squeeze() { printf '%s' "$1" | tr -s ' ' | sed 's/^ //;s/ $//'; }
check() { # desc attendu obtenu
	if [ "$(squeeze "$2")" = "$(squeeze "$3")" ]; then
		echo "ok   - $1"
	else
		echo "FAIL - $1"
		echo "       attendu : $2"
		echo "       obtenu  : $3"
		fail=1
	fi
}
nocheck() { # desc interdit obtenu
	if printf '%s' "$2" | grep -q -- "$3"; then
		echo "FAIL - $1 (trouvé '$3')"
		fail=1
	else
		echo "ok   - $1"
	fi
}

# --- v2 (MT7986) + champ ppe= (999-3001), état BND --------------------------
#   v2 : QID bits[6:0], PSE_QOS bit[8], DEST_PORT bits[12:9], DSCP bits[31:24]
#   0x00000314 -> QID=0x14(20) PSE=1 DEST=1 DSCP=0x00
#   0xb8000314 -> idem mais DSCP=0xb8(184 -> DSCP 46, ECN 0)
cat >"$TMP" <<'EOF'
0000b BND IPv4 5T ppe=1 orig=192.168.8.51:51001->203.0.113.2:443 new=100.64.0.5:40001->93.184.216.34:443 eth=aa:bb:cc:00:11:23->11:22:33:44:55:66 etype=0800 vlan=0,0 ib1=25000000 ib2=00000314 packets=77 bytes=12000
0000c BND IPv4 5T ppe=0 orig=192.168.8.52:51002->203.0.113.2:80 new=100.64.0.6:40002->93.184.216.34:80 eth=aa:bb:cc:00:11:24->11:22:33:44:55:66 etype=0800 vlan=0,0 ib1=25000000 ib2=b8000314 packets=88 bytes=13000
EOF

out=$(sh "$BIN" -v 2 -f "$TMP")
check "BND v2 : DSCP=0 -> ECN 0, QID 20, PSE 1, DEST 1, FLOW orig+new séparés" \
	"1 BND IPv4 5T 20 0x00( 0) 0 1 1 orig=192.168.8.51:51001->203.0.113.2:443 new=100.64.0.5:40001->93.184.216.34:443" \
	"$(printf '%s\n' "$out" | sed -n 2p)"
check "BND v2 : DSCP=0xb8 -> DSCP 46, ECN 0, QID 20" \
	"0 BND IPv4 5T 20 0xb8(46) 0 1 1 orig=192.168.8.52:51002->203.0.113.2:80 new=100.64.0.6:40002->93.184.216.34:80" \
	"$(printf '%s\n' "$out" | sed -n 3p)"

# --- v1 (MT7622/29), sans champ ppe= (interface mainline), état BND ---------
# ib2=007c0437 : DSCP=0x00 ; QID bits[3:0]=0x7(7) ; PSE_QOS bit[4]=1 ;
#   DEST_PORT bits[7:5]=1
cat >"$TMP" <<'EOF'
00001 BND IPv4 5T orig=192.168.8.2:1234->203.0.113.9:53 new=100.64.0.1:40000->93.184.216.34:53 eth=aa:bb:cc:dd:ee:01->aa:bb:cc:dd:ee:02 etype=0800 vlan=1,0 ib1=25000000 ib2=007c0437 packets=5 bytes=400
EOF

out=$(sh "$BIN" -v 1 -f "$TMP")
check "BND v1 : QID=7, PSE=1, DEST=1" \
	"0 BND IPv4 5T 7 0x00( 0) 0 1 1 orig=192.168.8.2:1234->203.0.113.9:53 new=100.64.0.1:40000->93.184.216.34:53" \
	"$(printf '%s\n' "$out" | sed -n 2p)"

# --- état UNB (pré-liaison matérielle) : AUCUN champ décodé -----------------
# Lignes réelles observées sur le routeur : état UNB, ib2 = bruit (QID>16 files,
# TOS arbitraire). Ici ib2=84000075 décoderait QID=117 et TOS=0x84 SI on ne
# filtrait pas sur BND — le test prouve que ces valeurs ne sortent PAS.
cat >"$TMP" <<'EOF'
00012 UNB IPv4 5T ppe=0 orig=192.168.8.10:52345->203.0.113.7:443 new=0.0.0.0:0 eth=aa:bb:cc:00:00:01->aa:bb:cc:00:00:02 etype=0800 vlan=0,0 ib1=1a0a5c00 ib2=84000075 packets=0 bytes=0
00013 UNB IPv6 5T ppe=1 orig=2001:db8::1:51000->2001:db8:1::2:443 eth=aa:bb:cc:00:00:03->aa:bb:cc:00:00:04 etype=86dd vlan=0,0 ib1=1c0a5c00 ib2=8400007f packets=0 bytes=0
EOF

out=$(sh "$BIN" -v 2 -f "$TMP")
check "UNB IPv4 : tous les champs décodés = '-'" \
	"0 UNB IPv4 5T - - - - - orig=192.168.8.10:52345->203.0.113.7:443 new=0.0.0.0:0" \
	"$(printf '%s\n' "$out" | sed -n 2p)"
check "UNB IPv6 : '-' + orig seul (pas de new= pour IPv6)" \
	"1 UNB IPv6 5T - - - - - orig=2001:db8::1:51000->2001:db8:1::2:443" \
	"$(printf '%s\n' "$out" | sed -n 3p)"
# Preuve négative : la valeur de bruit ne doit nulle part apparaître.
nocheck "UNB : la valeur QID de bruit (117) n'est pas affichée" "$out" "117"
nocheck "UNB : la valeur TOS de bruit (0x84) n'est pas affichée" "$out" "0x84"

# --- type « IPv4 5T » (avec espace) non tronqué -----------------------------
check "type avec espace non tronqué" "IPv4 5T" \
	"$(printf '%s\n' "$out" | sed -n 2p | awk '{print $3" "$4}')"

[ "$fail" = 0 ] && echo "Tous les tests passent." || echo "ÉCHEC."
exit "$fail"
