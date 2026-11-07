# RESUME_PESA — Veille multi-repo pesa1234/openwrt

- Date : 04/09/2026 · Méthode : analyse seule (zéro modification, zéro commit)
- Branche inspectée : **pesa1234/openwrt `next-r4.9.3.rss.mtk`** (la plus récente de la série r4.9.x)
- Référence fork : branche openwrt-6.18, GL-MT6000 MT7986A, kernel 6.18.44, mt76 pin 59676919ea (r40965-f962474556)
- ⚠️ Base de comparaison : **29 patches mt76 réels sur disque** (le prompt « 16 post-audit » omettait 0020, 0028→0037, 6013, 9999-17 — tous présents)
- Exécution : consult @orchestrator avant chaque axe + agents dédiés (patch-reviewer / parallel-check / fork) ; validation Christophe entre chaque axe
- Livrable : ~/iwrt-2512/RESUME_PESA.md — **untracked, jamais commité (règle 16)**

## Récapitulatif des 3 axes

| Axe | Chemin | Nouveaux | Retenus | Verdict |
|---|---|---|---|---|
| 1 — mt76 | package/kernel/mt76/patches | 93 / 107 distants | **3 PERTINENT + 5 à trancher** | Masse INERTE vendor / DÉJÀ_UPSTREAM / WED |
| 2 — mediatek + kernel | patches-6.18 + backport + pending | 27 + 22 + 26 | **0** | 2 doublons locaux, 73 INERTE (autres plateformes) |
| 3 — packages réseau | firewall4/qosify/nftables/dnsmasq | 0 | **0** | 6 fichiers byte-identiques, fw4/qosify sans patches/ chez pesa |

## AXE 1 — mt76 : liste PRIORITAIRE (intérêt décroissant)

### PERTINENT — candidats nets à porter

| Patch | Composant | Pourquoi | Risque |
|---|---|---|---|
| 9999-23 wcid-pktid corruption cleanup | mt76 core (mac80211.c) | Sérialise le teardown pktid (status_lock) vs `mt76_tx_status_skb_add()` — race core réelle | Low |
| 0005 null-ptr auth→bw-notif | mt7915/main.c (rc_work) | Guard `!msta->vif` : STA en Auth qui envoie « notify bw change » → crash. Famille de nos fixes crash | Low |
| 9999-24 publish wcid avant MCU add | mt7915/main.c (init_vif) | `rcu_assign_pointer(wcid[idx])` AVANT les cmd MCU — nexus direct avec notre 0028 (STA_REC/VoW) | Low |

### À trancher (5)

| Patch | Composant | Point de recoupement |
|---|---|---|
| 0018 QoS map SW/HW cohérent | mt7915/mcu (cmd MTK SET_QOS_MAP 0xb4) | **Chantier DSCP/qos_map_set** — cmd FW MTK non testée sur mt7986, expertiser avant tout |
| 9999-19 move sta_poll hors tx_free | mt7915/mac.c:853 | Touche le sta_poll de nos 9999-05/0036 — trade contention TX-free vs cadence airtime |
| 9603 garde format MPDU TXS | mt76_connac_mac.c | Complémentaire de la garde `PPDU_FMT` du master — à DRY-RUN |
| 0012 sanity wcid mt76_tx | mt76 core (tx.c) | Guard `tx_pending` non init |
| 0014 power-on seq | mt7915/soc.c+regs.h (ADIE CK1) | mt7986 SoC, complément ADIE XO_08 — pertinent seulement si souci power/ULP |

## AXE 1 — mt76 : ÉCARTÉS (raison en 1 mot)

| Groupe | Raison |
|---|---|
| 0000_001/002/003 (Felix Fietkau, efuse/calibration) | upstream (déjà dans notre pin) |
| 0000_004 (radar background) | périmé (signature master divergée) |
| 1000-1056 (52 : testmode/CSI/air-monitor/cert/muru/iBF/SCS/lpi) | vendor (infra MTK absente du mainline) |
| Airtime MTK : 9510/9512, 9999-03/07/09-16, 2005 | vendor (notre fairness passe par sta_poll, déjà durci) |
| EDCCA : 9999-04, 1015 | inerte (0 trace edcca dans notre master) |
| 0025, 2019, 0021, 9903 | divergence (le master a déjà l'équivalent ou l'inverse) |
| 0002/0003/0004/0006/0009/0010/0011/0019/0022 | cosmétique/debug (ACS inutilisé, canaux fixes) |
| 9999-08 (vow band) | couvert (déjà en init_vif chez nous) |
| 9999-20/21/22 (harden CSI/testmode/iBF) | vendor |
| 9999-01, 9704, 101 | cosmétique/build |
| 2000, 2011 | WED (abandonné, non analysés) |
| 1051 (set_channel par assoc), 1052 (memset vif reuse) | risqué/lourd ou cosmétique |
| 2016 refine TWT | différé (à confronter à nos 6013/6014 au bump mt76) |

Note : aucun doublon exact de nos patches locaux ; 9999-19 ≈ notre 9999-05 (mais ≠), 9999-08 couvert structurellement.

## AXE 2 — mediatek + kernel générique : ÉCARTÉS (raison en 1 mot)

### 2a — mediatek/patches-6.18 (27)

| Patch | Raison |
|---|---|
| 999-9920 (SGMII syscfg0 mask) | doublon (= notre 999-9924, hunks identiques vérifiés) |
| 999-9921 (spurious MDIO timeout) | doublon (= notre 999-9923, hunks identiques vérifiés) |
| 999-9922 (WDMA TX hang) | wed (garde `mtk_wed_is_v2`, chemin non exécuté) |
| 999-9902 (disable RSS), 999-9909 (jumboframe) | autre (mt7981 uniquement) |
| 999-2701 (RSS irqs) | autre (DTS mt7981b) |
| 010 (Dosilicon DS35) | autre (GL-MT6000 = eMMC, pas spi-nand) |
| 046-049, 063, 094-097, 192, 967/968/973 | autre (BPI-R4 Pro mt7988a exclusif) |
| 805, 969/970/974/976/977 (PHY as21xxx) | autre (PHY cartes mt7988 ; GL-MT6000 = C45 générique) |
| 975 (mtk-usxgmii link-up reset) | autre (PCS USXGMII mt7988/mt8195) |

Verdict : AUCUN patch mtk_eth_soc/PPE/DTS nouveau pertinent MT7986A. Le seul ciblant MT7986 (999-9922) est WED-gated. Confirmation positive : nos portés 22-23/08 (999-9923/9924) sont strictement identiques au contenu pesa actuel.

### 2b — backport-6.18 (22) + pending-6.18 (26)

Backport : ksmbd (500-504), motorcomm/stmmac YT6801 (705), mvneta + mxl862xx (781, 945), PHY realtek RTL8221B (799), hwmon adt7470 (851), USB xhci (894) — **tous INERTE** (drivers d'autres plateformes/hardware).
Pending : 26 **INERTE** — DSA/PHY autres puces (mxl862xx ×18, as21xxx ×5, RTL8261N, marvell EPROBE_DEFER). Rien cake/fq/tc/BPF/XDP/flowtable/conntrack/bridge.

## AXE 3 — packages réseau : aucun écart

- **firewall4 / qosify** : les packages existent chez pesa (`package/network/config/`) mais **sans dossier `patches/`** → aucun patch custom pesa à comparer. Nos 3 patches firewall4 (001-fullcone, 002-bridge-flowtable, 003-Option B) + qosify 001-multiqueue = ajouts propres au fork.
- **nftables (4)** + **dnsmasq (2)** : les 6 fichiers même-nom sont **byte-identiques** (sha256 égaux).
- **200-ubus_dns.patch** : identique → souscription dnsmasq ubus (dont dépend qosify-dyn) inchangée. **Non-RISQUÉ** : aucun nftset/netlink (pas de risque de fuite mémoire 132 MB).

## Prochaine étape

Décision de portage sur les 3 PERTINENT (9999-23, 0005, 9999-24) puis les 5 à trancher (0018, 9999-19, 9603, 0012, 0014). Chaque patch porté suivra le protocole habituel : DRY-RUN `git apply --check` + diff complet avant commit (règles 1-2), jamais push/CI sans demande (règle 3), attention aux zones partagées (0028, 9999-05/0036, DSCP).
