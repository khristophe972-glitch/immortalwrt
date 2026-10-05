# foedecode

Décode le champ **`ib2`** des entrées FoE du PPE MediaTek (MT7622/MT7629/MT7986)
depuis le debugfs, sans capture de paquets.

- **Lecture seule** — aucun accès en écriture au PPE, aucune modification de flux.
- **Dépendances** : busybox `ash` + `sed` + `awk` (déjà présents dans une image
  ImmortalWrt standard).
- **Emplacement** : `/usr/bin/foedecode` (`PKGARCH:=all`, script POSIX).

## Ce que fait l'outil

Pour chaque entrée FoE **`BND`** (liée), il extrait `ib2` et le décompose selon
la version NETSYS du SoC. **Pour tout autre état (`INV`/`UNB`/`FIN`), les
colonnes `QID/DSCP/ECN/PSE/DST` affichent `-`** : `ib2` n'est écrit que par le
chemin d'offload, au commit de l'entrée (voir « Entrées `UNB` » ci-dessous).

| Champ     | v1 (MT7622/29)  | v2 (MT7986)      | Sens                              |
|-----------|-----------------|------------------|-----------------------------------|
| `QID`     | bits [3:0]      | bits [6:0]       | file TX imposée par le port       |
| `PSE_QOS` | bit  [4]        | bit  [8]         | priorité PSE                       |
| `DEST_PORT` | bits [7:5]    | bits [12:9]      | port de sortie                     |
| `DSCP`    | bits [31:24]    | bits [31:24]     | **octet TOS entier** (DSCP+ECN)   |

⚠️ **`ib2[31:24]` porte l'octet TOS entier**, pas le DSCP sur 6 bits. Preuves
source (voir l'en-tête du script pour les `file:line`) : le SDK BSP MediaTek
`mtk_foe_entry_set_dscp()` écrit `FIELD_PREP(MTK_FOE_IB2_DSCP, dscp)` avec un
`dscp` pris sur `match.key->tos` (mask `0xff`) ; `MTK_FOE_IB2_DSCP` =
`GENMASK(31,24)` (`mtk_ppe.h:70`). L'outil affiche donc la valeur brute
(`0x%02x`) **et** la décomposition DSCP 6 bits (`>>2`) / ECN 2 bits (`&3`).

> `999-dscp-01` du fork stocke au contraire `tuple->dscp` = DSCP sur **6 bits**
> (`FIELD_GET(INET_DSCP_MASK,…)`). Un futur consommateur devra re-décaler `<<2`
> pour remplir l'octet TOS attendu par le matériel.

## Sources lues

Par ordre de priorité :

1. `-f FICHIER` (ou `-f -` pour l'entrée standard) ;
2. `/sys/kernel/debug/mtk_ppe/bind` — **buffer agrégé** des deux PPE, patch fork
   `999-3001` (chaque ligne porte un champ `ppe=N`) ;
3. `/sys/kernel/debug/ppe0/bind` — interface **mainline** (pas de champ `ppe=`).

L'interface existe donc **déjà** : `mtk_ppe_debugfs.c` (noyau) expose
`pokeN/{entries,bind}` (`l.184-195`) et `999-3001` ajoute `mtk_ppe/{entries,bind}`
agrégés. Les deux impriment `ib1=%08x ib2=%08x` (`mtk_ppe_debugfs.c:159-164`) —
**aucun patch noyau n'est requis** pour lire `ib2`.

## Usage

```sh
# PPE v2 (MT7986) sur le debugfs agrégé du fork
foedecode

# Forcer la version v1 (MT7622/29) et lire un dump fichier
foedecode -v 1 -f /tmp/ppe0-entries.dump

# Avec le champ ppe= (999-3001) ou sans (mainline) — autodétecté
foedecode -f /sys/kernel/debug/mtk_ppe/entries
```

Sortie : une ligne par entrée, colonnes `PPE STATE TYPE QID DSCP6b ECN PSE DST
FLOW`. La colonne `FLOW` distingue les deux tuples par leurs préfixes :
`orig=<src>:<sport>-><dst>:<dport> new=<…>` (le second n'apparaît que si le
debugfs l'imprime — types HNAPT/DSLITE uniquement).

## Entrées `UNB` (pré-liaison matérielle) — pourquoi QID/DSCP y sont du bruit

Sur le routeur, `entries` peut contenir des dizaines de lignes en état `UNB`
(et **aucune** en `BND`) même **sans** déchargement matériel (`flow_offloading_hw`
absent, flowtable logicielle seule). Source :

- **Ce qui crée ces entrées** : le **PPE matériel**, pas le chemin d'offload.
  `mtk_ppe_start()` arme `MTK_PPE_TB_CFG_AGE_UNBIND` (`mtk_ppe.c:1017`) et
  `MTK_PPE_TB_CFG_SEARCH_MISS = …FORWARD_BUILD` (`mtk_ppe.c:1021-1022`). Sur un
  *search miss*, le PPE **pré-lie** le flux en état `UNB` à partir de l'en-tête
  du paquet. Le logiciel n'écrit jamais `UNB` : `mtk_foe_entry_prepare()` pose
  `MTK_FOE_STATE_BIND` (`mtk_ppe.c:222/233`) et `__mtk_foe_entry_commit()` écrit
  `ib1` tel quel (`mtk_ppe.c:638`) — aucun `MTK_FOE_STATE_UNBIND` n'existe dans
  `mtk_ppe.c`. Ces champs (`UNBIND_TIMESTAMP/PACKETS/PREBIND`) sont définis
  `mtk_ppe.h:16-18` = remplis par le **matériel**.
- **Pourquoi `new=` n'est pas initialisé** : la pré-liaison matérielle ne remplit
  que le tuple **`orig`** (elle ne connaît pas la traduction NAT). Le tuple `new`
  n'est écrit qu'au commit logiciel — `mtk_foe_entry_commit_subflow()` recopie
  `orig`→`new` pour HNAPT (`mtk_ppe.c:728-729`). Or la table n'est mise à zéro
  **qu'une fois**, au démarrage (`mtk_ppe_init_foe_table()`, `mtk_ppe.c:959`) ;
  après quoi les slots sont recyclés (recherche de hash) sans nettoyage du
  `new`. Un slot réutilisé peut donc conserver un `new=` résiduel. Le debugfs
  imprime `new=` sans condition pour HNAPT/DSLITE (`mtk_ppe_debugfs.c:132-144`).
- **Les deux PPE** : MT7986 déclare `ppe_num = 2` (`mtk_eth_soc.c:5539-5548`) ⇒
  des entrées `UNB` peuvent apparaître en `ppe=0` **et** `ppe=1`.
- **L'état `UNB` n'est pas filtré** par le debugfs, qui ne saute que l'état
  `INVALID` (0) : `if (!state) continue;` (`mtk_ppe_debugfs.c:93-98`).

## Prérequis de test

Pour peupler le PPE d'entrées `BND`, l'unité de test doit avoir le **déchargement
matériel actif** (`uci set firewall.@defaults[0].flow_offloading_hw='1'`), sinon
le PPE ne contient **aucune** entrée `BND` et l'outil n'affiche rien. Le PPE
n'est alimenté que par des flux **forwardés** (WAN↔LAN), pas par le trafic local.
