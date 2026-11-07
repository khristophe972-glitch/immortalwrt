# ÉTAT — Latence uplink WiFi GL-MT6000 — 10/09/2026

Fichier NON commité (règle 16). Point de reprise de session.

---

> ## ⚠️ CORRIGÉ LE 11/09/2026 — LIRE AVANT TOUT
> Ce document a été écrit **sous le régime HW offload**, abandonné le **10/09**
> au profit du **SW offload durable** (règle 6 : `flow_offloading='1'`, option
> `flow_offloading_hw` ABSENTE, vérifié live le 11/09). Deux de ses affirmations
> structurantes en découlent et sont **FAUSSES**, une troisième repose sur un
> raisonnement invalide et était périmée. Le texte d'origine est **conservé**
> pour traçabilité ; chaque correction est marquée 🔴 ou ❌.
>
> ## ⛔ MAJ 15/09/2026 — le volet QoS de ce document est MORT
> Les mentions de **`cake_mq` « root actuel »** (l. 182) et de mesures via
> `tc -s class show dev eth1` (l. 189) sont **périmées** : qosify a été retiré le
> 12/09, **QosMate** l'a remplacé puis a été **désinstallé le 15/09/2026**, remplacé
> par le script custom **`/etc/init.d/cake-shaper`** (`cake_mq` root **egress `eth1`
> seul**, unlimited ; **pas d'`ifb-eth1`** ⇒ descente non shappée) — ⚠️ **MAJ
> 18/09/2026 : `cake-shaper` est OBSOLÈTE, le shaper est désormais SQM
> `piece_of_cake.qos` (`cake_mq` root, `bandwidth 900Mbit`)**.
> État courant : `CLAUDE.md` §ÉTAT QoS. Le reste (mécanique `mtk_tx_hash`, verrous
> `ndo_setup_tc`/`CONFIG_NET_SCH_MQPRIO`, upload WiFi traversant eth1) reste valable.

---

## Problème
Sous charge, latence de l'uplink WiFi -> 776 ms. Mesuré en session antérieure,
**jamais reproduit ni re-mesuré depuis**. C'est le trou de tout le travail.

🔴 **11/09** : il est désormais **re-testable** — un levier existe (voir
§« Résultat structurel »), là où ce document concluait qu'aucun n'existait.

## Résultat structurel — ❌ FAUX (corrigé le 11/09)

**Texte d'origine :**
> L'upload WiFi est **offloadé au PPE**. Il ne traverse donc ni QDMA, ni AQL,
> ni cake, ni tc. Vérifié sur le silicium (arbre kernel réel + mesures routeur),
> pas supposé.

**Pourquoi c'est faux.** Vrai **uniquement en HW offload**. En **SW offload**
(régime durable depuis le 10/09), il n'existe **aucun chemin de bypass** : un
paquet forwardé retraverse la pile Linux, donc le qdisc d'eth1 **puis** les
files QDMA. L'affirmation est structurellement impossible dans le régime
actuel — indépendamment de toute mesure.

**Mesure du 11/09 (et son croisement).** Client WiFi = Ubuntu `192.168.8.218`,
MAC `f8:ac:65:25:46:44`, confirmé station de `phy1-ap0` (VHT-NSS 2, 160 MHz).
Speedtest Ookla : **upload 709,62 Mbps / 616,5 Mo**.

- les 16 enfants `cake_mq` d'eth1 ont encaissé **+672 Mo** sur la fenêtre
  (= 616,5 Mo d'upload + ~43 Mo d'ACK du download + surdébit — ajustement
  quasi exact) ;
- croisement : `lan4` (1 Gbit UP) = **14 Mo cumulés depuis le boot**, `lan2` =
  100 Mbit/s → le client ne pouvait pas être câblé.

Répartition : **q3 283,7 Mo + q6 190,1 Mo + q14 189,3 Mo = 98,6 %** du delta.

🔴 **11/09 — deux erreurs de lecture corrigées ici même :**
- « tins DSCP peuplés (l'enfant `800b:10` porte 234,9 Mo en Tin 2 = AF41) » → **FAUX**.
  `234910233` est un compteur **CUMULÉ depuis la création du qdisc** (donc depuis le
  boot), ce n'est **pas** une part du delta d'upload. Le delta réel de la file HW 15
  sur la fenêtre de mesure était de **34 917 octets**. Aucune conclusion DSCP ne peut
  être tirée de ces compteurs.
- « q3/q6/q14 = 98,6 % » ne doit **PAS** être lu comme structurel : la 2e mesure
  (11/09 bis) donne un **ensemble disjoint** (files HW 1, 2, 10, 11, 13, 15 = 99,7 %,
  dont la file 3 à **0,02 %**). L'attribution est le hash 4-tuple **par flux**
  (`reciprocal_scale(skb_get_hash, 16)` via `mtk_tx_hash`) → **aléatoire et non
  reproductible** d'une session à l'autre. Aucune file n'est « chaude » par nature.

**Portée réelle de `qdma_tx_rate` et de `cake_mq` sur WiFi :**

| Jambe | Chemin | `qdma_tx_rate` / `cake_mq` |
|---|---|---|
| **UPLOAD** client → Internet | client → AP → routeur → **eth1 TX** → WAN | **GOUVERNENT** |
| **DOWNLOAD** Internet → client | WAN → eth1 **RX** → routeur → mt76 → client | **SANS PRISE** (mt76/AQL) |

⚠️ **Question ouverte** : la répartition q3/q6/q14 n'est pas expliquée par
`mtk_tx_hash` tel que je le lis (voir §« Faits d'architecture »). Une **2e
mesure serrée** est nécessaire (règle 17) avant d'en tirer quoi que ce soit sur
les files.

## Éliminé définitivement

- ❌ « **AQL / fq / cake / tc — inertes sur le trafic WiFi offloadé** » →
  🔴 **FAUX** : `cake_mq` voit et shape l'**upload** WiFi (mesure ci-dessus).
  Reste vrai pour la jambe **download** uniquement (mt76/AQL).
- ❌ « **`qdma_tx_rate` (patch 999-9926, committé) et `ethtool -L` — leviers
  soft-path uniquement** » → 🔴 **trompeur** : ils gouvernent la jambe
  **upload** d'un client WiFi, qui EST du soft-path dès lors qu'il est forwardé.
  Sans prise sur le download, en revanche.
- Régulation matérielle PPE/PSE — **inexistante** : aucun registre de débit dans
  `mtk_ppe_regs.h` ; le FOE aiguille PSE->GDM via DEST_PORT_V2 sans passer par
  `qtx_sch`. ✅ inchangé
- Multicast — < 0,6 % d'airtime (276 pkt / 100,6 s), innocent. ✅
- mt76 / mac80211 — audité sain (voir RESUME.md), 0 défaut actif. ✅ ⚠️ mais
  c'est désormais **le seul levier du DOWNLOAD** : à rouvrir en priorité si le
  symptôme s'avère être en download.
- pesa / padavan — 0 levier exploitable pour la latence. ✅

## Fait notable, non résolu
Upload WiFi client ~775 Mbps ~= plafond WAN mesuré (~800 Mbps). Si le goulot est
le WAN et non l'air, la question de la file se déplace. À creuser.

🔴 **11/09** : cohérent avec la mesure — l'upload WiFi traverse bien le WAN, sous
le cap `cake_mq` de **880 Mbit**. 775 < 880 → **le cap ne mord pas** ; le goulot
est en aval (ligne Free, ~900 Mbps up).

## Mesures du 10/09 (routeur, lecture seule) — résultats conservés
Baseline soft-path WAN (speedtest-go --no-download, routeur -> internet) :
- JSON ul_speed = 99 972 456 **octets/s** = 799,8 Mbps
  Unite tranchee par compteur : l'hypothese bits/s (99,97 Mbps) est incompatible
  avec les 1 116 112 258 octets vus par eth1.
- compteur eth1 : 1 116 112 258 o / 13,04 s = 684,7 Mbps (run complet),
  ~888 Mbps sur la phase upload seule. Paquets 1497 o (MSS plein).
- ping 2,39 ms, jitter 0,32 ms (serveur Marseille, 661 km)

Controle positif du cap qdma_tx_rate : 16 files a 2 Mbps via debugfs ->
soft-path WAN tombe de ~800 a **3,33 Mbps** (divise par 206).
Dry-run debugfs valide 6/6 (no-op rc=0 ; idx>=16 rejete ; valeur invalide rejetee ;
blanc seul = no-op, fix 5a67f8f871 verifie live).
Routeur rendu propre : 16/16 files remises a MAX_RATE_EN=0, WAN OK.

🔴 **11/09 — interprétation corrigée.** La conclusion d'origine (« donc l'absence
d'effet sur l'offload est bien un bypass structurel ») était un raccourci. Ce
contrôle positif prouve **une seule chose** : la chaîne `debugfs →
mtk_qdma_set_queue_max_rate() → registre qtx_sch` **fonctionne**. Il portait sur
du trafic **généré par le routeur lui-même**, qui emprunte le soft-path par
construction — il ne dit **rien** du sort d'un flux *forwardé*, qui est la
question posée.

## Faits d'architecture — 🔴 corrigé le 11/09

- MT7986 : **un seul bloc QDMA** (une seule entrée reg_map, base 0x4400),
  16 files TX, `MTK_QTX_PER_PAGE` = 16. ✅
- ❌ « eth1 (WAN) : `ethtool -L eth1 tx 4` posé par **/etc/rc.local:23** =>
  mtk_tx_hash répartit le soft-path WAN sur les files **0-3** (eth0 : 16 files) »
  → 🔴 **PÉRIMÉ**. La référence **entérinée le 11/09** est
  **`ethtool -L eth1 tx 16`** (16 enfants cake_mq / 16 files ; `tx 1` cassait le
  mapping → uplink actif médian **+31 ms**). Vérifié live :
  `ethtool -l eth1` → max TX 16 / current TX 16 ; 16 kobjects `queues/tx-*`.
- ❌ « `dsa_queue_base` : eth0 -> 3 (5 ports = files 3-7), **eth1 -> 8** » →
  🔴 **FAUX**. eth1 = **gmac1 = WAN, jamais conduit DSA** : son `dsa_queue_base`
  reste à la valeur d'init (`MTK_MAX_DEVS` = 3, patch 795-05) et **ne sert
  jamais** — la branche DSA de `mtk_select_queue` exige `netdev_uses_dsa(dev)`.
  Seul **eth0** est conduit (MT7531 `port@6`) → files **3-7**.
- ❌ « `mtk_set_queue_speed()` : seul autre ecrivain de qtx_sch, appele
  uniquement pour les ports DSA. **Jamais appele sur ce boot** (aucune ligne
  dmesg) » → 🔴 **FAUX — et le raisonnement était invalide** : la fonction
  n'imprime **rien**, l'absence de ligne dmesg ne prouve donc rien.
  Ce board a un switch **MT7531 (DSA)** ; `mtk_device_event()` est le notifier
  `NETDEV_CHANGE` des ports DSA (`mtk_eth_soc.c` : `dsa_user_dev_check` +
  `__ethtool_get_link_ksettings` + rejet si speed == 0 ou -1 →
  `mtk_set_queue_speed(eth, dp->index + 3, speed)`). lan2 (100) et lan4 (1000)
  sont UP → **le notifier s'exécute au runtime**.
  ➜ **Corollaire — CHAÎNE SOURCE CONFIRMÉE le 11/09** : les files **3 à 7** (lan1-5 →
  `dp->index` 0-4 → `dp->index + 3`) portent un MAX_RATE au débit du port
  (**file 3 = 100 Mbps** car lan2 est en 100 Mbit/s ; file 5 = 1000 Mbps pour lan4).
  La levée exige `mac->speed <= s.base.speed` avec `mac->speed` = eth0 = **2500** →
  **jamais levée**. Et `mtk_set_queue_speed()` écrit le **mot entier** (pas de
  read-modify-write) → un cap posé par `qdma_tx_rate` sur les files 3-7 est
  **écrasé au premier flap de lien** lan1-lan5.
  ⚠️ **Tension quantifiée, NON résolue** (elle ne se tranche pas par une 2e mesure) :
  la mesure du 11/09 attribue à la file 3 **283,7 Mo** sur une fenêtre d'upload où le
  total était de 663 Mo à ~750 Mbps ⇒ **~320 Mbps sur la file 3**, soit **3× le cap
  prétendu**. Contradiction réelle — sauf si le rate-limiting QTX **ne remonte pas de
  backpressure** vers l'anneau TX DMA (le compteur de classe, côté CPU, verrait alors
  le plein débit pendant que le fil est plafonné). Question de **source**, pas de
  mesure : à trancher en lisant si le QTX MAX_RATE stalle le ring ou s'il drope en
  aval. Le contrôle positif du 10/09 (16 files à 2 Mbps → WAN 800 → 3,33 Mbps) prouve
  que le cap **mord** sur le soft-path, donc la tension reste entière.
- 🔴 **Ajout 11/09** : `mtk_rx_put_buff()` (chemin RX) appelle
  `skb_record_rx_queue(skb, 0)` — **valeur codée en dur 0** (vanilla 6.18.44).
  Donc dans `mtk_tx_hash` (`rcount = real_num_rx_queues` = 4 > 1 **et**
  `skb_rx_queue_recorded`) un paquet **reçu sur eth1** part sur la **file 0**.
  Un paquet reçu d'**ailleurs** (WiFi) n'a pas de rx queue enregistrée → branche
  `reciprocal_scale(skb_get_hash(skb), 16)` → n'importe laquelle des 16.
  **Explication candidate de la répartition q3/q6/q14** (le trafic mesuré venait
  du WiFi, pas d'eth1) — à confirmer par la 2e mesure.
- 🔴 **Ajout 11/09 — le levier classe→file est FERMÉ (3 verrous).** Seul chemin
  non-hashé de `mtk_tx_hash` = `if (dev->num_tc)` → `tc_to_txq` via `skb->priority`
  (donc DSCP). Or :
  1. `dev->num_tc` ne se pose que par **mqprio** → `ndo_setup_tc` type
     `TC_SETUP_QDISC_MQPRIO` ;
  2. `mtk_eth_setup_tc` est défini dans **`mtk_ppe_offload.c:641`** (PAS dans
     `mtk_eth_soc.c` — seul le `.ndo_setup_tc` y figure, l.4764) et n'accepte QUE
     `TC_SETUP_BLOCK` + `TC_SETUP_FT` → `-EOPNOTSUPP` pour tout le reste ;
  3. `CONFIG_NET_SCH_MQPRIO is not set` (`target/linux/generic/config-6.18`) → le
     qdisc mqprio n'est même pas compilé.
  ⇒ Sur eth1 le DSCP **ne choisit aucune file**. Le seul sélecteur est
  `reciprocal_scale(skb_get_hash, 16)`. Et mqprio devrait être le qdisc **root**, ce
  qui entrerait en collision frontale avec `cake_mq` (root actuel) — 3e verrou
  rédhibitoire indépendant des deux premiers.
- `mtk_ppe_init(eth, ...)` pour les 2 PPE => ppe0 et ppe1 pointent le MEME
  `struct mtk_eth` : les deux noeuds debugfs ecrivent les memes registres. ✅

## Actions en attente (aucune engagée)

- **0 (NOUVEAU 11/09)** — **2e mesure serrée de l'upload WiFi** (zéro écriture
  routeur) : compteurs `tc -s class show dev eth1` juste avant / juste après un
  speedtest upload WiFi. But : (a) re-confirmer le volume — règle 17, 2 mesures
  indépendantes ; (b) trancher la répartition q3/q6/q14 et le point « files 3/5
  capées par les événements de lien DSA ».
- **A** — ❌ « tester la latence sous charge, offload ON vs OFF. Seul chemin
  connu pouvant bouger le symptome. L'exclusion de flux existe deja dans le fork
  (Option B / qosify-flow-exclude), **actuellement OFF** » → 🔴 **CADUC** :
  l'offload est fixé en **SW durable** (règle 6, décision explicite) et sous SW
  offload l'Option B n'a plus d'objet ici. Le chemin à tester est désormais :
  **reproduire les 776 ms d'uplink WiFi** et observer `cake_mq` — `rtt 2ms`,
  `ack-filter`, `triple-isolate` et le cap 880 Mbit sont **déjà en place**.
- **1** — `git fetch` de la branche **r4.9.3** de pesa1234 puis
  `git apply --check` des 3 PERTINENT (9999-23 pktid, 9999-24 publish-wcid,
  9999-19 sta-poll) contre notre pin, diffs a montrer avant tout commit.
  ATTENTION : ce sont des fixes de correction (crash/race), PAS de latence. ✅
- **2** — cloner **`mt7988-6.6-mt76`** de padavanonly : seule zone jamais
  ouverte. ✅

## Materiel de reference — 🔴 TOUT A DISPARU (constaté le 11/09)

`/var/tmp` est **VIDE** sur le routeur (tmpfs → perdu). Les chemins ci-dessous
sont conservés comme **liste de ce qu'il faut re-cloner**, pas comme ressources
disponibles.

- `/var/tmp/refs/pesa-r492` — pesa1234/openwrt, `next-r4.9.2.rss.mtk`,
  103 patches mt76, pin 59676919ea (le notre)
  ATTENTION : RESUME_PESA.md decrit la branche **r4.9.3**, le clone etait **r4.9.2**
  => 9999-19/23/24 n'y etaient PAS (le clone s'arrete a 9999-18).
- `/var/tmp/refs/padavan-mt798x` — padavanonly, branche `mt798x-mt799x-6.6-mtwifi`
  = **driver proprietaire MediaTek** (package/mtk/drivers/mt_wifi7, iwpriv),
  **0 patch mt76**, clone superficiel. Rien de portable vers notre fork mt76.
  ATTENTION : la branche `mt7988-6.6-mt76` existe cote remote, jamais clonee.
- `/var/tmp/refs/mt76-ref` — upstream mt76 @ pin (fichiers extraits, pas un depot)
- `/var/tmp/k18b/linux-6.18.44` — arbre kernel reel (584 patches / 0 echec /
  0 .rej ; 11 objets mtk_eth compiles 0 warning)
  🔴 **11/09 : absent**. Les vérifications de code de ce document ont été
  refaites le 11/09 depuis une copie locale de `mtk_eth_soc.c` **vanilla 6.18.44**
  (`/tmp/mtk_eth_soc.c`) — cohérente, mais **sans** les patches du fork
  (795-04/05/06, 999-9915, 999-9925, 999-2711) : pour tout ce qui touche ces
  patches, se référer aux sources dans `target/linux/`.

## Etat du depot
🔴 **11/09** : HEAD = origin/openwrt-6.18 = **`2758e25826`** (« qosify: make
ingress ifb multiqueue so cake_mq attaches » — patch 003, non flashé).
Rien a committer, rien a pousser. Aucune modification de fichier suivi.
**19 non-suivis** a la racine (regle 16).
