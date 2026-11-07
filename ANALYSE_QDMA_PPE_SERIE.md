# ANALYSE_QDMA_PPE_SERIE.md — Série QDMA TX + PPE hardware-offload (mtk_eth_soc)

Doc d'analyse **lecture seule** (0 modif, 0 compile), produit le 09/09/2026 sur le
fork ImmortalWrt (branche openwrt-6.18), cible GL.iNet GL-MT6000 (MT7986A,
Filogic 830, netsys v2, `version=2`, kernel 6.18.44).

Objet : **toute la série QDMA + PPE hardware-offload pour mtk_eth_soc** — les
2 familles de patches (QDMA/TX path + PPE offload) + le socle WED/WDMA — jusqu'à
la question posée par le chantier MAX_RATE (999-2728) : « jusqu'où le matériel
peut-il mordre malgré cake ? ».

Method : reconstruction des sources à l'état flashé (vanilla 6.18.44 +
patches appliqués en ordre) puis lecture du chemin de données. Fichier
**untracked, JAMAIS commité** (règle 16, précédent ANALYSE_FLOWTABLE_PRIORITY.md).

---

## Phase 1 — Classification (25 patches, lecture seule)

| patch | statut | fichiers + fonctions | actif MT7986A ? |
|---|---|---|---|
| **795-04** per-SoC QDMA queue count + paging | pending Golle (4/9) | mtk_eth_soc.c/.h : `MTK_QDMA_NUM_QUEUES`→`num_tx_queues`, `qdma.page` + `MTK_QTX_PER_PAGE` | **oui — fondation** |
| **795-05** per-conduit DSA queue map | pending Golle (5/9) | `mtk_update_dsa_queue_map()`, `dsa_queue_base`/`dsa_port_rank[]` | **oui** (gmac0, conduit unique) |
| **795-06** use DSA queue map in TX | pending Golle (6/9) | `mtk_set_queue_speed()` avec `queue`, TX path lit la map | **oui** — route lan2-5 → qid global |
| **999-2728** SPEED_2500 MAX_RATE | **vendor fork** (MediaTek) | case SPEED_2500 MAN25/EXP5/WEIGHT10 | **partiel** — jamais écrit en prod (gate, mission 1) |
| **999-9915** non-DSA TX queue selection | **vendor fork** (custom) | `mtk_tx_hash()` (num_tc/XPS) | **oui** — chemin eth1 WAN → cake_mq 16 files |
| **795-01** rhashtable_lookup_fast | pending Golle (1/9) | mtk_ppe_offload.c : 3 sites | **oui** (perf lookup) |
| **795-02** tport_idx netsys_v3 | pending (Bo-Cun Chen) | mtk_ppe.c/.h : `MTK_FOE_TPORT_IDX` | **non** — garde v3 = MT7988 |
| **795-03** output device avant bind | pending Golle (3/9) | mtk_ppe_offload.c : `mtk_flow_set_output_device()` | **oui** (ordre bind→WED) |
| **795-07** DSA queue map in flow offload | pending Golle (7/9) | mtk_ppe_offload.c : lit la map côté offload | **oui** — flux PPE héritent la map |
| **795-09** offload MxL862xx | pending Golle (hors série) | `mtk_flow_get_dsa_port()`, tag_8021q | **non** — autre board, compilé inerte |
| **795-08** tag_mxl862xx queue_mapping | pending Golle (8/9) | net/dsa/tag_mxl862xx_8021q.c | **non** — tagger absent (MT7531) |
| **736-03** improve tracking FOE | backport Fietkau | `mtk_flow_entry_match_len()` | **oui** |
| **736-04** fix accounting L2 | backport Fietkau | `mtk_ppe_acct_data()`, poll_atomic | **oui** |
| **736-05** zero-init FOE table | backport Fietkau | memset FOE + MIB | **oui** |
| **700** flowtable netdev events | backport Neira 2018 | net/netfilter | **oui** (flow_offloading_hw=1) |
| **701** ignore EOPNOTSUPP | backport Fietkau | nf_tables_api.c | **oui** |
| **940/942/943/944** WED regions/dts | backport Bianconi | mtk_wed.c + dts (cpuboot/ILM/DLM) | **oui** (runtime WED mainline) |
| **999-9911→9918** fixes WED fork | **vendor fork** | mtk_wed.c : double-free, SER wed1, reinsert, wdma init, busy-poll, WDMA reset | **oui** (runtime) |

**Lecture transverse (formulée en phase 1, à corriger en synthèse — voir §4)** :
la série Golle `795-01→795-09` (795-09 hors numérotation) a une dépendance
stricte — 795-04 (paging + `num_tx_queues`) précède tout, 795-05 pose la map,
**795-06 (TX CPU) et 795-07 (offload PPE) la consomment**. Hypothèse initiale :
CPU TX et flux PPE offloadés s'alignent sur le même espace paginé et touchent
tous les registres `qtx_sch` que 999-2728 configure. **Les phases 2-3
départagent cette hypothèse : convergence des NUMÉROS (map partagée) mais plans
de registres SÉPARÉS (voir la découverte clé §4).**

Bordure examinée non retenue (pas dans le périmètre) : 732-00/01/03/04/05, 702,
737-08/09, 738-01/02 (ring DMA / NAPI / probe).

---

## Phase 2 — Marche QDMA TX de bout en bout (MT7986A)

Établie sur `/tmp/finalstack` (driver `mtk_eth_soc.c` reconstruit = état flashé,
6845 lignes).

### 1. Allocation : 2 netdevs, chacun 16 files logiques
`mtk_add_mac()` l.5851-5857 : `MTK_QDMA` → `txqs = soc->num_tx_queues` = **16**
(mt7986_data l.6712) ; `MTK_RSS` → `rxqs = soc->rss_num` = **4** (l.6714). Les 2
macs (eth0 conduit DSA, eth1 WAN direct) reçoivent chacun
`alloc_etherdev_mqs(…, 16, 4)` → **16 files TX logiques par netdev**, partageant
**un seul ring DMA physique** (`&eth->tx_ring`, « 2 netdevs on 1 physical DMA
ring »).

### 2. Sélection de file : `mtk_select_queue()` (ndo l.5793)
Trois voies (l.5667-5689) :
- **DSA (eth0)** : `dp_idx = skb_get_queue_mapping(skb)` (posé par le tagger DSA
  = `dp->index`) → `queue = mac->dsa_queue_base + mac->dsa_port_rank[dp_idx]`
  (l.5674-5678). Patch 795-06.
- **non-DSA multi-file (eth1)** : `queue = mtk_tx_hash(dev, skb, sb_dev)`
  (l.5680, patch 999-9915).
- **mono-file** : `queue = mac->id`.
- Borne : `if (queue >= dev->num_tx_queues) queue = 0` (l.5685).

### 3. `mtk_tx_hash()` (999-9915) — le chemin eth1 WAN
l.5630-5665 : `qcount = dev->real_num_tx_queues` (**16**, ou réduit si
`ethtool -L`). Si RSS actif (`rcount = real_num_rx_queues` = 4 > 1) et rx_queue
enregistrée → hash = queue RX ; sinon `reciprocal_scale(skb_get_hash, qcount)`
(l.5664). `num_tc` (qdisc multi-TC) → offset/count par TC (l.5640-5648). Pas de
TC actif sur eth1 → **spread 0-15 par hash**.

### 4. Émission : `mtk_start_xmit` → `mtk_tx_map`
l.1869 : `queue = skb_get_queue_mapping(skb)` (**re-lit la même valeur** posée
par select_queue ; l'enqueue cake_mq n'a pas bougé l'index) → `.qid = queue`
(l.1888). Encodeur v2 (MT7986) l.1735 : `QID_BITS_V2(info->qid)` dans txd4. Le
**qid du descripteur = l'index de file 0-15**.

### 5. Hardware : le qid → registre qtx_sch paginé
`mtk_set_queue_speed()` l.997-1001 : `page = idx / 16` → registre `qdma.page`
(0x45f0), `ofs = 0x10 * (idx % 16)` → `qtx_sch` (0x4404). MT7986 : 16 queues = 1
page.

**Défaut init** (mtk_tx_alloc l.3085-3107, pas via mtk_set_queue_speed) : pour
chaque queue `i` → **0x38140000** = `MIN_RATE_EN | LEAKY_BUCKET_SIZE | MAN1 EXP4`
(plancher 10 Mbps, **pas de MAX_RATE_EN**). tx_sch_rate l.3109-3111 = MAX_WFQ
(weighted fair queueing global).

**MAX_RATE** (mtk_set_queue_speed l.894-1003) : écrit **uniquement** via
`mtk_device_event` (notifier NETDEV_CHANGE, l.4162) sur les user ports DSA —
valeurs de la branche else (MT7986, non-MT7621, non-V3) : SPEED_10→MAN1 EXP4,
100→MAN1 EXP5, 1000→MAN1 EXP6, **2500→MAN25 EXP5** (999-2728).

### Synthèse phase 2
- **eth1 (WAN, non-DSA)** : sélection 0-15 par hash/XPS → cake_mq suit (un cake
  enfant par file, 16 ou réduit via `ethtool -L`) → qid 0-15. **Aucun MAX_RATE
  posé sur ces files par eth1 lui-même** (jamais de NETDEV_CHANGE DSA).
- **eth0 (conduit DSA)** : files = base (`MTK_MAX_DEVS` = 3) + rang → lan2-5
  (1G) → MAX_RATE=1000 posé au link-up ; lan1 (2.5G, égal conduit) → neutralisé
  par la gate.
- **Chevauchement** : les files 3-6 (MAX_RATE=1000 posés par les LAN 1G)
  **appartiennent aussi à l'espace 0-15 d'eth1** → un upload eth1 haché sur ces
  files croise un registre à 1000 Mbps (non contraignant < plafond 900 ligne).
  Sous `ethtool -L eth1 tx 4`, eth1 = q0-3 → chevauche q3 (lan2).

---

## Phase 3 — Marche PPE offload (egress)

### Reconstruction (méthode mission 1)
5 fichiers PPE reconstruits à l'état flashé : mtk_ppe.c, mtk_ppe.h,
mtk_ppe_regs.h, mtk_ppe_offload.c, mtk_ppe_debugfs.c — vanilla v6.18.44
(raw.githubusercontent.com/gregkh/linux/v6.18.44) + **8 patches appliqués en
ordre numérique** (736-03/04/05, 795-01/02/03/07/09), tous OK, dans
`/tmp/ppe_stack/work/`. Les patches 700/701 (net/netfilter) sont le drain vers
le flowtable — référencés conceptuellement, non reconstruits (hors fichiers PPE).

### La marche d'un flux offloadé (état flashé)

1. **Naissance** : règle nft `flow add @ft` → `ndo_setup_tc` →
   `mtk_flow_offload_replace()` (mtk_ppe_offload.c). Choix du `ppe_index` par
   conduit ; un port DSA est substitué à son conduit avant le check idev (hunk
   795-09, correctif générique **actif** — sans lui les flux depuis un user port
   DSA se binderaient sur le mauvais PPE engine et ne matcheraient jamais).
2. **Bind** (ordre flashé) : `mtk_foe_entry_prepare()` (IB1 STATE_BIND +
   IB2 DEST_PORT placeholder + PORT_AG_V2=0xf) → tuplets v4/v6 + mangles NAT →
   **`mtk_flow_set_output_device()` AVANT le VLAN/PPPoE** (795-03) →
   `mtk_wed_flow_add()` si sortie WiFi → `mtk_foe_entry_commit()`.
3. **Champs de sortie** (FOE IB2 v2, MT7986) :
   - `DEST_PORT_V2` (bits 12-9) = port PSE de destination. Enum `mtk_pse_port` :
     ADMA=0, **GDM1=1, GDM2=2**, PPE0=3, PPE1=4, **QDMA_TX=5** (=chemin CPU),
     QDMA_RX=6, DROP=7, **WDMA0=8, WDMA1=9**, TDMA=10, NONE=11, PPE2=12,
     WDMA2=13, EIP197=14, GDM3=15.
   - `QID_V2` (bits 6-0) + `PSE_QOS_V2` (bit 8) = file de sortie **côté PSE**.
   - `PORT_AG_V2` (bits 23-20) = agrégation (0xf).
4. **Choix egress** (`mtk_flow_set_output_device`, mtk_ppe_offload.c l.218-285) :
   - **WiFi** (netdev WiFi reconnu via `mtk_flow_get_wdma_info`) : `set_wdma`
     (RX_IDX + winfo wcid/bss) → pse_port = WDMA0/1/2 + `wed_index` →
     **`mtk_wed_flow_add()`** → paquet vers le chip radio via les rings WDMA.
   - **Câblé** : substitution DSA → dev = conduit. pse_port = GDM1/2/3 selon
     `dev == eth->netdev[0/1/2]`. Si DSA : `set_dsa` (etype BIT(dp), tag MTK
     natif, push_vid=0) ou `set_vlan(push_vid)` (tag_8021q MxL862xx) ;
     **queue = `dsa_queue_base + dsa_port_rank[dp]`** (795-07). Sinon (GDM
     direct, ex. WAN) : **queue = `pse_port - 1`**.
   - Toujours : `set_queue(queue)` + `set_pse_port(pse_port)`.
5. **Commit** : `mtk_foe_entry_commit()` insère l'entrée candidate dans
   `foe_flow[hash/offset]` ; le hardware apprend son hash réel, le driver
   committe à ce hash (`mtk_ppe_check_skb`/IRQ → `__mtk_foe_entry_commit`).
6. **Comptabilité** (736-03/04) : table logicielle `acct_table` cumulée (MIB lu
   incrémentalement, poll atomic), stats par entrée FOE, idle par timestamp.

### Réponse à la question centrale (flux offloadés vs cake / MAX_RATE)
**Le trafic offloadé n'emprunte JAMAIS l'espace que cake_mq et 999-2728
gouvernent**, par construction :
- Le CPU (et donc le qdisc eth1) n'est pas dans le chemin — les deux branches
  egress sont **GDM/WDMA, jamais `PSE_QDMA_TX_PORT`(=5)**. Cake ne voit que le
  pré-offload (confirme structurellement les ~99,9 % de bypass mesurés).
- Le `QID` du FOE est un champ **du plan PSE**, pas un index de ring TX QDMA, et
  **aucun registre qtx_sch n'est écrit pour le chemin offloadé** (les seuls
  écrits qtx_sch = défaut 0x38140000 à l'alloc + MAX_RATE via
  `mtk_set_queue_speed`, chemin TX CPU). Le shaping logiciel (cake, MAX_RATE
  1000 des files partagées 3-6) ne gouverne que le soft-path.
- Nuance downlink LAN : les flux offloadés vers un port LAN portent un QID
  **numériquement** 3-7 (base+rank, 795-07) qui coïncide avec les files capées
  — mais ce champ vit dans le FOE (plan PSE), hors espace qtx_sch ; le driver
  n'y applique aucun MAX_RATE, et la limite physique 1G du port domine de toute
  façon.

### Attribution des 8 patches reconstruits (v2 = notre board)
| Patch | Rôle dans la marche | Actif sur notre HW |
|---|---|---|
| 795-01 | `rhashtable_lookup_fast` (4 sites) | ✅ micro-opt |
| 795-02 | tport_idx (v3, injection tport) | ❌ gated `netsys_v3` |
| 795-03 | output device avant VLAN/PPPoE | ⚪ **no-op déclaré** (tag MTK + WED) |
| 795-07 | DSA queue map dans le FOE (sorties LAN) | ✅ change le QID posé, sans effet shaping |
| 795-09 | MxL862xx (push_vid) + substitution conduit→ppe_idx | ⚪ MxL862xx inactif ; ✅ hunk ppe_idx actif |
| 736-03 | comptabilité FOE (acct_table, sweep MIB) | ✅ sans effet egress |
| 736-04 | stats par entrée + idle (fix L2) | ✅ sans effet egress |
| 736-05 | zero-init table FOE + MIB | ✅ sans effet egress |

---

## Phase 4 — Synthèse : les 2 plans séparés du silicium

### Découverte clé (correction de la lecture transverse de la phase 1)
La phase 1 supposait que CPU TX et flux PPE offloadés « s'alignent sur le même
espace paginé `num_tx_queues` → tous touchent les registres `qtx_sch` que
999-2728 configure ». **Les marches 2-3 réfutent la seconde moitié** :

- Le chemin **TX CPU** traverse les rings QDMA (files 0-15 eth1 / base+rang pour
  eth0) → le qid du descripteur pointe le registre **qtx_sch paginé** (là où le
  défaut 0x38140000 et les MAX_RATE de 999-2728 s'appliquent) → PSE → GDM.
  **C'est ici que cake_mq s'attache** (un cake enfant par file) et que les
  MAX_RATE mordent.
- Le chemin **offloadé (PPE)** ne monte pas au CPU. Le FOE (DEST_PORT_V2 +
  QID_V2/PSE_QOS + PORT_AG) aiguille **PSE → GDM/WDMA** directement. QID_V2 est
  un champ PSE, pas un index de file QDMA ; aucun registre qtx_sch n'est écrit
  pour ce chemin.

**Conclusion** : les deux familles convergent sur les **numéros de map**
(795-05/06/07 partagent `dsa_queue_base`/`dsa_port_rank`) mais vivent dans des
**plans de registres séparés** (QDMA/qtx_sch vs PSE/FOE). Le matériel « ne mord
pas » : ni cake ni MAX_RATE ne gouvernent le trafic offloadé — seule la
classification QDMA de sortie du PSE (champ PSE_QOS/QID, non exposé par le
driver) pourrait le faire, et rien dans le driver n'y touche.

### Implications pratiques (GL-MT6000, production)
1. **Bypass cake ~100 % offloadé = structurel, pas contournable par la série
   QDMA/PPE**. Toute tentative de « shaper le flux offloadé » via qtx_sch /
   files cake est vaine : le paquet offloadé ne traverse aucun de ces deux
   mécanismes. (Recoupement : verdict 02/09 + discriminateur 06/09 — le bloat
   est le réseau Free, le routeur ne peut rien y corriger par le soft-path.)
2. **Option B (exclusion des flux vidéo de l'offload) = la seule voie pour
   qu'un flux redevienne gouvernable** (soft-path → traverse eth1 qdisc/cake et
   peut être marqué). Cohérent avec le chantier DSCP : hors offload, le flux est
   marquable/façonnable ; offloadé, il est invisible. Le coût = CPU + latence
   soft-path.
3. **MAX_RATE (999-2728) ne contraint que le soft-path TX CPU** : upload eth1
   non-offloadé haché sur les files 3-6 partagées avec les LAN 1G (cap 1000,
   jamais atteint sous le plafond ligne 900), et soft-path eth0 vers ports LAN.
   Jamais les flux offloadés (uplink WAN : QID FOE = pse_port-1 = 1 ; downlink
   LAN : QID 3-7 hors espace qtx_sch).
4. **Le plancher MIN_RATE (défaut 0x38140000, MAN1 EXP4 = 10 Mbps) s'applique
   aux files TX CPU** — c'est le « floor » du scheduler QDMA, distinct du MAX.
   Aucun impact observé (jamais de starvation CPU→PSE mesurée).
5. **Vue WED/WDMA** (3e famille, classification seulement) : socle mainline
   (940-944, backport Bianconi regions/dts) + fixes fork runtime (999-9911→9918)
   — la sortie WiFi du PPE passe par WDMA/WED (branch de `set_output_device`),
   jamais par le QDMA CPU : même propriété de bypass, côté radio. Recoupement
   CLAUDE.md : WED mainline actif (PPE→mt7915 via `ndo_fill_forward_path`), le
   CPU ne passe jamais par les rings WED.

---

## Références / ancres (état flashé reconstruit)

- Driver : `/tmp/finalstack/drivers/net/ethernet/mediatek/mtk_eth_soc.c`
  (6845 l.) + `/tmp/mtk_eth_soc.h` (vanilla v6.18.44).
- PPE : `/tmp/ppe_stack/work/drivers/net/ethernet/mediatek/` (mtk_ppe.c/.h,
  mtk_ppe_regs.h, mtk_ppe_offload.c, mtk_ppe_debugfs.c).
- DTS : `target/linux/mediatek/dts/mt7986a-glinet-gl-mt6000.dts` — gmac0 =
  conduit MT7531 (LAN), gmac1 = WAN 2.5G (`label-mac-device`).
- Patches : `target/linux/generic/pending-6.18/{736-03,736-04,736-05,795-01..09,
  940,942,943,944}` ; `target/linux/mediatek/patches-6.18/{999-2728,999-9911..18,
  999-9915}` ; 700/701 = net/netfilter.
- Firmware flashé : r40983-80a382e273 (08/09) — kernel 6.18.44.
