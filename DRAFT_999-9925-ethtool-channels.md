# DRAFT — 999-9925 ethtool get/set_channels pour mtk_eth_soc (MT7986A)

Fichier NON commité (règle 16). Réflexion + ébauche patch 08/09/2026.
Repo ~/iwrt-2512, branche openwrt-6.18 (tip f567cbe868).
**MAJ 08/09 (post-flash r40982) : correction du bug E2 découvert LIVE.**

## Objet
`ethtool -l eth1` / `ethtool -L eth1 tx 4` → « Not supported » sur le
GL-MT6000 (mtk_eth_soc, MT7986A, kernel 6.18.44). Constat 08/09 : **aucune
version d'ethtool ne peut corriger** — l'op kernel `dev->ethtool_ops->
get_channels/set_channels` manque → -EOPNOTSUPP en kernel pour les chemins
ioctl ET netlink, avant toute logique userspace. Fix = kernel-side.

## Historique
1. **Version initiale** (commit a755cb406f) : `mtk_set_channels()` rejetait
   `if (channels->rx_count || channels->combined_count) return -EOPNOTSUPP;`.
   `.get_channels` seul suffisait à faire marcher `ethtool -l` ; `-L` requiert
   `.set_channels`. Poussé → CI SUCCÈS → flashé r40982.
2. **Constat LIVE post-flash (E2)** : `ethtool -L eth1 tx 4` → **« Not
   supported »** malgré `.set_channels` présent. `ethtool -l` (get) OK.
3. **Cause racine** : les DEUX setters (ioctl ET netlink) passent à
   `set_channels()` le `rx_count` COURANT (4) pour une requête TX-only →
   le test `rx_count || combined` est VRAI à chaque fois → -EOPNOTSUPP
   systématique. Un `rx_count=0` n'atteint jamais le driver (voir §contrat).
4. **Correction 08/09** : discriminateur `rx_count != real_num_rx_queues`
   (seul un vrai changement RX est rejeté) + garde no-op si tx inchangé.
   **Patch corrigé ÉCRIT dans patches-6.18 mais NON committé, NON poussé.**

## §Preuves contrat (pourquoi rx_count courant arrive toujours)
- **Netlink — net/ethtool/channels.c** `ethnl_set_channels` (v6.18.44) :
  pré-remplit le canevas via `get_channels()` (l.120), `ethnl_update_u32` ne
  touche que les attrs PRÉSENTS (l.124-131) → si seule `tx_count` est passée,
  `rx_count` reste la valeur lue par get = courante. Appelle le driver
  `set_channels` l.181 avec rx_count=courant.
- **Ioctl — net/ethtool/ioctl.c** `ethtool_set_channels` (l.2247) : copie de
  l'userspace MAIS l.2277 exige `!combined_count && (!rx_count || !tx_count)`
  (sinon -EINVAL) → userspace ethtool DOIT fournir le rx_count courant pour
  une requête TX-only → le driver reçoit rx_count=courant.
- **Conclusion** : le seul test correct du « RX veut bouger ? » est
  `rx_count != dev->real_num_rx_queues`. Un rx_count=0 ne parvient jamais au
  driver par l'ioctl (rejeté l.2277) ; le comparer serait donc aussi faux.

## La fonction corrigée (`mtk_set_channels`)
```c
static int mtk_set_channels(struct net_device *dev,
			    struct ethtool_channels *channels)
{
	/* RX is hardware-fixed (RSS/LRO).  Both the ioctl and netlink setters
	 * carry the CURRENT rx_count when only TX is requested, so only a real
	 * RX change (count differing from the live one) is rejected below.
	 */
	if (channels->combined_count ||
	    channels->rx_count != dev->real_num_rx_queues)
		return -EOPNOTSUPP;

	if (!channels->tx_count || channels->tx_count > dev->num_tx_queues)
		return -EINVAL;

	if (channels->tx_count == dev->real_num_tx_queues)
		return 0;

	return netif_set_real_num_tx_queues(dev, channels->tx_count);
}
```
Garde no-op ajoutée : `-L eth1 tx 16` (requête identique à l'état courant) ne
fait pas de reset des files 12-15 inutile.

## §Validation de la correction (08/09, avant écriture dans l'arbre)
- **Fichier cible** : `target/linux/mediatek/patches-6.18/999-9925-...patch`
  (enveloppe OpenWrt, format-patch style, 31 insertions).
- **Proxy pristine** : mtk_eth_soc.c v6.18.44 brut (download fichier unique,
  gregkh/linux). Application GNU patch → **byte-identique** à la cible générée
  (`cmp` vs /tmp/mtk_new.c). Hunk 2 « fuzz 2 » à 4800.
- **Proxy arbre réel (sim_real)** : pristine + membres RSS 999-2711
  (`.get_rxfh_key_size`… insérés entre `.set_rxnfc` et `.get_eee`) + fonction
  remplissage avant le struct. Résultat : les membres `.get_channels`/
  `.set_channels` se posent **entre `.set_rxnfc` et `.get_rxfh_key_size`** =
  position correcte que produira l'arbre réel. (offset 5 = ligne de remplissage
  simulée, attendu.)
- **Fuzz 2 = bénin, PAS une fragilité** : l'ANCIEN patch (a755cb406f, prouvé
  en CI #34182661833) a le MÊME comportement (« Hunk #2 succeeded at 4793 with
  fuzz 2 » sur pristine). Structure de hunk identique (`-4758,3` insertion pure
  + hunk2 sans contexte de queue) → mécanique GNU patch identique. L'ancrage
  hunk2 ne porte que `.get_rxnfc`/`.set_rxnfc` (2 ctx, PAS de contexte aval) :
  les membres RSS 999-2711 sont APRÈS `.set_rxnfc` → ne décalent pas la
  position relative struct→`.set_rxnfc` → placement positionnel correct.
- **Format restauré** : les 2 lignes de contexte terminales de hunk1
  (`.get_link_ksettings`/`.set_link_ksettings`) gardent l'espace de tête
  (format contexte conforme, identique au fichier committé).

## Point d'attention conservé (valide pour la version corrigée)
`-L eth1 tx 4` IMMÉDIAT et EFFECTIF sans recréer cake_mq : `mq_attach()` ne
hash que les files `< real_num_tx_queues`, `netif_set_real_num_tx_queues()`
câble `mq_change_real_num_tx()` (root cake_mq, `.change_real_num_tx` 700-03),
`qdisc_reset_all_tx_gt()` vide les files 4-15, le shaper partagé suit
(`cake_mq_queue_count()` = real_num). Nuance : les 16 instances cake restent
allouées (12 inertes, coût nul).

## Non fait / garde-fous
- Patch corrigé ÉCRIT dans patches-6.18, **NON committé, NON poussé, aucune
  CI** (en attente validation Christophe).
- VRAI DRY-RUN sur l'arbre kernel complet patché = uniquement possible en CI
  (règle 9 : pas de build local).
- Routeur r40982 non touché depuis le constat E2 (aucun -L appliqué en dur,
  aucun reboot).
- Commit message prévu : décrire le fix du contrat set_channels (ioctl+netlink
  portent rx_count courant), Signed-off-by Christophe.
