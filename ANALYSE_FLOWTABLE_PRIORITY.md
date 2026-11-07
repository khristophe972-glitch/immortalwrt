# ANALYSE — flowtable priority (Bairaktaris) + bridge-fastpath (Woudstra)

Fichier NON commité (règle 16). Mission lecture seule 07/09/2026 — analyse de portabilité
de 2 séries netfilter vers le fork ImmortalWrt GL-MT6000 (MT7986A, kernel 6.18.44,
branche openwrt-6.18, tip f567cbe868).

Sources : /tmp/lore-flowtable-priority/ (mbox patchwork + commentaires de review).
Datapath de référence (production) : flow_offloading_hw=1 → PPE offloade ~100% du
forwardé ; flowtable `inet fw4` ft (hook ingress priority filter, flags offload) ;
Option B désactivée (sets vides) ; downlink DSCP coupé natif (ingress='0').

> ⚠️ **MISE À JOUR 15/09/2026 — verdict CONFIRMÉ, raisonnement PÉRIMÉ.** Cette analyse
> date du **07/09** et repose sur deux prémisses **mortes** : (1) « bypass PPE ~100 % » —
> faux depuis le **SW offload durable du 10/09** (le forwardé traverse bien le SW
> fastpath) ; (2) « `skb->priority` = 0 sur tous nos flux » — faux depuis la bascule
> **QosMate du 12/09** (`meta priority set …` pose bien `skb->priority`). La conclusion
> « ne pas porter » **tient quand même**, mais pour une **autre raison**, établie le
> 15/09 en lecture seule sur `sch_cake.c` : `cake_select_tin()` (**l.1682/1684**) ne lit
> `skb->priority` que si `TC_H_MAJ(priority) == sch->handle` **et**
> `TC_H_MIN(priority) <= q->tin_cnt` — or la `priomap` pose des minors **11-15** face à
> `tin_cnt = 8` en `diffserv8` ⇒ cake classe par **DSCP** (champ que le SW offload ne
> perd jamais) ⇒ la série A reste **sans consommateur**.
> Le patch **709 a été RETIRÉ du dépôt le 15/09/2026** (fichier déplacé dans
> `~/retrait-709-20260915/`, sha256 `69cf174b…`), et **QosMate a lui-même été
> désinstallé le 15/09** (aujourd'hui : **SQM `piece_of_cake.qos`**, `cake_mq`
> root `bandwidth 900Mbit` — ⚠️ **MAJ 18/09/2026** : le `cake-shaper` cité dans les
> versions antérieures de cette note est **OBSOLÈTE**). Détail : `CLAUDE.md` (tableau Patchs
> custom) et `.claude/docs/chantiers-clos.md` (veille Bairaktaris, close le 15/09).

---

## Série A — « netfilter: flowtable: carry a priority into the offload »
Julius Bairaktaris, nf-next v2 (base commit 91ec2035), 01-02/09/2026.

### Contenu
Ajoute `u32 priority` à `struct flow_offload` ; `nft_flow_offload_eval()` capture
`flow->priority = pkt->skb->priority` au flow add ; SW fastpath (`nf_flow_table_ip.c`,
v4+v6) re-pose `skb->priority = flow->priority` après `ip_decrease_ttl` ; HW offload
(`nf_flow_table_offload.c` `nf_flow_rule_route_common()`) émet une action
`FLOW_ACTION_PRIORITY` ; mtk/airoha = `case FLOW_ACTION_PRIORITY: break;` (ignore).
Consommateur cible = driver OpenWrt DSA IPQ8074 (PPE Qualcomm). Série = 7 fichiers
+28/−1 + selftest. `priority == 0` → inerte.

### Applicabilité 6.18.44
- Fichiers tous présents. Zones non modifiées par nos patches locaux (vérifié) :
  `nf_flow_table_ip.c` intact (aucun patch local), fenêtre `flags/type/timeout` de
  `struct flow_offload` intacte, switch REDIRECT/CSUM de `mtk_ppe_offload.c` intact
  (nos hunks mtk à 10/193/280/472/541). `nft_flow_offload.c` eval décalé par 675-10
  (~+390 lignes) mais fenêtre `flow_offload_ct_tcp`/`NF_FLOW_HW_BIDIRECTIONAL` intacte
  → gros offset toléré.
- ⚠️ Dépendance compile non vérifiable localement : `flow_action_entry_next()` appelé
  dans le hunk offload.c est absent des headers locaux (Ubuntu 7.0) et d'aucun patch de
  l'arbre → à confirmer sur le source kernel 6.18.44 au build. NB : `FLOW_ACTION_PRIORITY`
  absent de tous les patches OpenWrt de l'arbre (cohérent avec un état pre-série).
- Absente de 6.18.44 (série nf-next post-6.18).

### Gain réel GL-MT6000 = ZÉRO
1. Notre forwardé est bypass PPE ~100% → ne traverse JAMAIS le SW fastpath
   (`nf_flow_table_ip.c`) où la priorité serait ré-appliquée.
2. `skb->priority` = 0 sur tous nos flux (QoS = DSCP diffserv8 via qosify, PAS
   priority ; DSCP ≠ skb->priority).
3. `mtk_ppe_offload.c` et `airoha` = `case FLOW_ACTION_PRIORITY: break;` (ignore) ;
   FOE NETSYSv2 MT7986 n'a ni champ DSCP ni files prioritaires (constat 29/08).
4. Rien ne consomme la valeur portée.

### Risque
Faible (champ u32 interne, rebuild cohérent) mais **aucun intérêt fonctionnel** MT7986.
Le seul scénario d'utilité = driver de HW offload classant par priorité (type IPQ8074),
absent de notre datapath.

### VERDICT Série A : **NE PAS PORTER** (inerte, risque compile non lévé)

---

## Série B — « bridge-fastpath » Eric Woudstra v12 (7 patches, 07/07/2026)

### Constat central
Notre série locale `pending-6.18/675-01→13` (port Bohdan Chubuk, provenance
`Based on commit … from ericwoud/bpir-nftflow-nf-next-20260512` + Signed-off-by
Woudstra, vérifiée 11/13) EST la série de Woudstra : **le fork est un sur-ensemble de v12**.

### Identité v12 ↔ 675 (croisement binaire)
| v12 | 675 | note |
|---|---|---|
| 1/7 (fill path port-à-port) | 675-05 | identique |
| 2/7 (dev_fill_bridge_path) | 675-06 + 675-13 | 675-13 retire en plus le WARN_ON |
| 3/7 (nf_flow_rule_bridge) | 675-07 | identique |
| 4/7 (flowtable_bridge) | 675-08 | identique |
| 6/7 (NFPROTO_BRIDGE validate) | 675-09 | identique |
| 7/7 (bridgeflow to eval) | 675-10 + 675-12 + 675-03 | 675-12 hw_ifidx (structural MT7986), 675-03 MTK_WDMA wifi |
| 5/7 (garde thoff==0) | **ABSENT** | `grep thoff` = 0 dans les 675 |
| retrait `memset(tuple->encap)` | conservé par 675-10 | cosmétique neutre |

Style retenu : branch `bool routing` dans `nft_flow_offload_eval` (= celui de v12),
pas le boilerplate `_unsupp` préféré par Pablo pour la review upstream.

### Déclaration Woudstra (MT7986A)
hw_offload bridge fonctionnel ~2 ans : BPi-R3 (lan1 MT7531 ↔ eth1) et R3-mini
(eth0 ↔ eth1), driver mtk **in-tree SANS changement** (« No patch needed »). Principe :
« If a software fastpath is setup correctly, the hardware fastpath is also functional. »
Selftest bridge_fastpath.sh (notre 675-11) = couverture vlan, q-in-q, pppoe, pppoe-in-q,
dsa-port in_vlan_ingress.

### Delta manquant (optionnel)
Garde `thoff==0` (v12 5/7) : hunk unique (decl `int ret, thoff;` + capture
`thoff = nft_thoff(pkt); if (thoff == 0) goto out;` avant le `skb_header_pointer` TCP).
S'applique proprement (zone intacte chez nous), mais ne durcit que le chemin **bridge**
latent — notre flowtable fw4 est `inet` (forward routé), thoff jamais 0 → code mort.

### VERDICT Série B : **DÉJÀ COUVERT** par 675-01→13 + 795-*. Aucun portage obligatoire.
Hypothèse de reprise (SI symptôme bridge-fastpath sur br-lan un jour) : porter la garde
seule en `675-14`. Le memset ne pas porter.

---

## VERDICT GLOBAL
**AUCUN PORTER.** Série A inerte sur MT7986 (skb->priority=0, bypass PPE, driver ignore).
Série B intégralement présente dans le fork (sur-ensemble + correctifs MT7986 déjà actifs).
Les 2 séries n'apportent rien à l'état verrouillé actuel (Option B down, downlink coupé
natif, upload offloadé PPE). Aucun changement d'arbre, aucun commit, aucune CI.

## Commandes de vérification (si reprise future, sur arbre kernel préparé — pas ici)
```
# Série A : dépendance compile
cd build_dir/target-*/linux-*/linux-6.18.44
grep -n flow_action_entry_next include/net/flow_offload.h
git apply --check /tmp/checkA/patchA-01.patch
# Série B : garde thoff (extraire le patch 5/7 du mbox v12)
git apply --check <patch-5-7>
```
Sources hunks : /tmp/lore-flowtable-priority/bairaktaris_priority_flowtable.mbox,
/tmp/lore-flowtable-priority/woudstra_bridge_fastpath_v12.mbox.
