# Réponse à pesa1234 — cap 160-200 Mbit/s à 640/700

> ⛔ **DOCUMENT PÉRIMÉ — NE PAS ENVOYER EN L'ÉTAT.** Deux raisons, toutes deux du
> 10/09 : (1) il est **mal nommé** — ce n'est pas un échange privé avec pesa1234 mais
> une réponse de forum lue par Christophe ; (2) sa conclusion (« division par le nombre
> de files RÉFUTÉE ») est **contredite par la mesure ultérieure** (640 / 4 files = 160).
> À réécrire entièrement avant tout envoi — réécriture **abandonnée sur décision de
> Christophe** (10/09). Le contexte QoS a en outre changé depuis (mqprio puis QosMate
> retirés, shaper custom `cake-shaper` du 15/09 — ⚠️ **lui-même OBSOLÈTE depuis le
> 18/09/2026, remplacé par SQM `piece_of_cake.qos`**).

Statut : **document interne, non suivi, jamais committé** (règle 16).
But : message prêt à envoyer + annexe de preuves. Rien ici n'est un patch.

---

## Message à envoyer

**Objet : cap 160-200 Mbit/s à 640/700 — je n'arrive pas à reproduire, besoin de ton setup exact**

Salut,

J'ai testé tes deux hypothèses sur mon GL-MT6000 (MT7986A, ImmortalWrt `openwrt-6.18`,
kernel 6.18.44, `cake_mq` = backport v7.0). Les deux sont **réfutées** chez moi, et
proprement. Voilà ce que je trouve.

**Montage.** `eth1` (WAN) est en `cake_mq` root, `ethtool -L eth1 tx 4` → 4 enfants, un
par file TX. Trafic généré **depuis le routeur** (`speedtest-go --no-download`), débit
mesuré à la fois par le JSON et par le delta du compteur `tx_bytes` (les deux concordent
à ~1 %).

**1. Pas de division par le nombre de files.** Avec un cap réel `bandwidth 640Mbit rtt 20ms` :
- les 4 enfants affichent `bandwidth 640Mbit` (**pas** 160) ;
- le débit agrégé mesuré = **569 Mbps** (netdev 472.7 sur la fenêtre complète), soit
  569/640 = 0.89, exactement le rendement attendu avec `overhead 38 mpu 84` ;
- `overlimits` non nuls sur les 4 enfants
  (991543 / 245541 / 13584 / 411924 / 320494) → le shaper mord vraiment.

Donc `is_shared` fait bien son travail : l'agrégat des 4 enfants est ≈ le débit configuré,
pas 4 × le débit, et pas débit/4.

**2. Le script SQM ne divise pas non plus.** Dans `Seg_Layer_Cake.qos` (l.176-180 et
l.216-220), les deux sens passent `bandwidth "${UPLINK}kbit"` / `"${DOWNLINK}kbit"` — le
débit plein. `select_cake()` (functions.sh:985-990) ne choisit que `cake` vs `cake_mq`
selon `num_queues > 1` et `USE_MQ`, sans jamais toucher au débit.

**3. Le mono-flux n'est pas un plafond.** `--thread=1` → **566-674 Mbps**, `--thread=2` →
**675-811 Mbps**, multi-flux → **851-890 Mbps**. Pente normale, pas de plafond dur.

**Courbe de référence (non shapée, depuis le routeur)** : 1 flux 566 / 2 flux 675 /
N flux 851-890 Mbps. Avec cap 640 : 569 Mbps.

**Piège que j'ai rencontré, à vérifier chez toi** : un `tc qdisc replace dev eth1 root
cake_mq ...` **sans** `bandwidth` ne supprime PAS un rate existant — `cake_change()`
préserve `q->rate_bps` quand l'attribut `bandwidth` est absent. Donc si tu as « retiré »
le shaping par un `replace`, le cap peut très bien être resté actif. L'unique voie pour
effacer un rate est `del` + `add` frais. Ça pourrait expliquer un cap résiduel.

**Autre piège, celui-là m'a coûté une session** : j'ai un contrôleur maison (`cake-ctrl`,
boucle 10 s) qui lit le `bandwidth` réel du root et réapplique `unlimited` dès qu'il voit
autre chose. Toutes mes mesures de shaping étaient donc nulles jusqu'à ce que je l'arrête.
Si tu as un hotplug/script/daemon qui touche le qdisc root, pareil.

**Ce dont j'ai besoin pour reproduire :**

1. La mesure 160-200 est faite **depuis le routeur** (speedtest/iperf en local) ou **depuis
   un client derrière l'AP** ? Le chemin diffère : le trafic local traverse le qdisc
   `eth1`, le trafic forwardé offloadé (PPE netsys v2) le bypasse complètement.
2. Le `tc -s qdisc show dev eth1` **brut et complet** (root + tous les enfants) **pendant**
   la mesure — c'est la pièce décisive : `bandwidth` affiché par enfant, et les
   `overlimits`.
3. Ta config SQM : `use_mq`, `enable_mq`, `upload`, `download`, `qdisc`, `script`,
   `eqdisc_opts`, `ifb_mq_queues_mode`.
4. `ethtool -L eth1 tx` (nombre de files TX réelles) et `uname -a`.
5. Y a-t-il un script/démon qui réécrit le qdisc root (hotplug, init.d, watchdog) ?
6. La commande exacte et le nombre de flux.

Avec (1), (2) et (3) je devrais pouvoir trancher. Sans le (2) notamment, on ne peut pas
distinguer « le shaper ne mord pas » de « le shaper mord à un débit qu'on n'attendait pas ».

Merci,

---

## Annexe — preuves brutes (ne pas envoyer tel quel)

### A. Cap 640 Mbit, cake-ctrl stoppé (première mesure de shaping valide)

```
child 0..3 : bandwidth 640Mbit          <- PAS 160
netdev     : 768.8 MB / 13.0 s = 472.7 Mbps
ul_speed   : 71.2 MB/s = 569 Mbps
Sent 2002017937 bytes 1332245 pkt (dropped 2523, overlimits 991543 requeues 0)
Sent  539873374 bytes  359098 pkt (dropped  300, overlimits 245541 requeues 0)
Sent  410694884 bytes  273481 pkt (dropped   32, overlimits  13584 requeues 0)
Sent  606361932 bytes  403162 pkt (dropped 1285, overlimits 411924 requeues 0)
Sent  445087747 bytes  296504 pkt (dropped  906, overlimits 320494 requeues 0)
```
Non-dérive vérifiée : 25 s sans `tc` lancé, root toujours `640Mbit`.

### B. Mise à l'échelle par nombre de flux (état unlimited)

| flux | netdev | ul_speed (phase upload) |
|---|---|---|
| 1 (`--thread=1`) | 565.8 Mbps | 674 Mbps |
| 2 (`--thread=2`) | 674.6 Mbps | 811 Mbps |
| N (défaut) | 851-890 Mbps | 800-1000 Mbps |

### C. Le piège cake-ctrl — la preuve directe

Pendant un test, **sans aucune commande `tc` de ma part** :

```
AVANT : qdisc cake 8016: root ... bandwidth 100Mbit   Sent 258 bytes 3 pkt
APRES : qdisc cake_mq 8017: root bandwidth unlimited  Sent 1234855349 bytes 821175 pkt
```

Log correspondant :
```
06:02:52 cake-ctrl: derive : bandwidth reel=, re-applique UNLIMITED (rtt 2ms)
06:04:03 cake-ctrl: derive : bandwidth reel=, re-applique UNLIMITED (rtt 2ms)
```
`bandwidth reel=` **vide** = son `awk` ne matche que `/qdisc cake_mq/` ; un root `cake`
(non-mq) est interprété comme dérive et supprimé.
Source : `/usr/sbin/cake-ctrl.sh` l.80-85 (`apply_unl` = `del` + `add`), l.111-116,
l.59 (`CAP_OPTS="bandwidth 760mbit rtt 20ms"` — le seul cap qu'il sait poser).

### D. Hypothèses réfutées, récapitulatif

| hypothèse | test | résultat |
|---|---|---|
| cake_mq divise par le nb de files | cap 640 réel | enfants 640, agrégat 569 → réfutée |
| le script SQM divise | lecture l.178/218 | débit plein → réfutée |
| `sync_time_ns` désarme le cap | sync à 1000 s | 680 Mbps → réfutée |
| le shaper est inerte | cake-ctrl stoppé | cap tient (569) → réfutée |
| un flux TCP unique plafonne | `--thread=1` | 566-674 Mbps → réfutée |

### E. Topologie eth1 (référence)

```
root 8018 cake_mq bandwidth unlimited rtt 2ms
  :1 216 MB   :2 364 MB   :3 983 MB   :4 445 MB      (mtk_tx_hash, inégal 4.5:1)
clsact : 33 GB (compte tout le trafic, pas un enfant)
ethtool -L eth1 tx 4 (posé par /etc/rc.local:23) ; eth0 tx=16
```
