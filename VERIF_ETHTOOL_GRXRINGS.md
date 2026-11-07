# VÉRIFICATION — patch Vladimir Oltean (get_num_rings / ETHTOOL_GRXRINGS) dans ethtool 6.11 et 7.1

Fichier NON commité (règle 16). Vérifications lecture seule 08/09/2026.
Repo ~/iwrt-2512, branche openwrt-6.18 (tip f567cbe868).

Objet : confirmer la présence, dans les tarballs ethtool portés par le fork, du commit
upstream *« netlink: rss: retrieve ring count using ETHTOOL_GRXRINGS ioctl »*
(Vladimir Oltean <vladimir.oltean@nxp.com>, soumis sept. 2024, acké Jakub Kicinski).

## Statut ethtool dans le fork
- Tip `f567cbe868` : `PKG_VERSION:=7.1` (bump `672400f361` 20/07/2026). PAS de 6.11 actuel.
- Firmware flashé r40980 (`494705f7c5`) : **7.1** aussi (le bump 7.1 est ancêtre du commit).
- 6.11 = seulement dans l'historique git (`9454331b7f` 19/01/2025 « ethtool: update to 6.11 »),
  depuis longtemps dépassé (6.11 → 6.14 → 6.15 → 6.19 → 7.0 → 7.1).

## Vérification 1 — ethtool 7.1 (fork actuel) : PATCH PRÉSENT ✅
- Tarball téléchargé : https://cdn.kernel.org/pub/software/network/ethtool/ethtool-7.1.tar.xz
- sha256 = `4d78c26edc0255bc92f4b995b5fd66108d75ff966ed4694f6025a6d370bc2496`
  **= PKG_HASH du Makefile actuel** → archive authentique.
- `netlink/rss.c` : l.71 commentaire « There is no netlink equivalent for ETHTOOL_GRXRINGS »,
  l.72 `static int get_num_rings(struct cb_args *args)`, l.77 `.cmd = ETHTOOL_GRXRINGS`,
  l.152 `ret = get_num_rings(args);`.

## Vérification 2 — ethtool 6.11 (version historique du fork) : PATCH PRÉSENT ✅
- Tarball téléchargé : https://cdn.kernel.org/pub/software/network/ethtool/ethtool-6.11.tar.xz
- sha256 = `8d91f5c72ae3f25b7e88d4781279dcb320f71e30058914370b1c574c96b31202`
  **= PKG_HASH du commit historique 9454331b7f** (2 sources indépendantes concordantes, règle 17)
  → archive authentique.
- `netlink/rss.c` : l.64 commentaire idem, l.65 `static int get_num_rings(struct cb_args *args)`,
  l.70 `.cmd = ETHTOOL_GRXRINGS`, l.145 `ret = get_num_rings(args);`.

## Identité du code entre versions
`diff` du bloc « There is no netlink equivalent … }` entre ethtool-6.11 et ethtool-7.1
= **vide** → bloc `get_num_rings` byte-identique.

## Vérification 3 — routeur r40980 (runtime, 08/09/2026) : patch ACTIF et EXERCÉ ✅
- Paquet installé : **`ethtool-full`** 7.1 (variante full = `--enable-netlink`) + `libmnl0`.
  Le Makefile du fork : variante par défaut `ethtool` (tiny) = `--disable-netlink` → le
  patch Oltean (netlink/rss.c) serait compilé HORS ; seule `ethtool-full` l'inclut.
- Binaire `/usr/sbin/ethtool` : 751 035 octets, 27 marqueurs « netlink » (strings),
  chaînes « netlink error »/« New RSS context » présentes → netlink compilé.
- **Preuve d'exécution** : `ethtool -x eth1` (RSS netlink, lecture seule) →
  « RX flow hash indirection table for eth1 **with 4 RX ring(s)** » : la requête
  RSS_GET netlink réussit contre mtk_eth_soc ET le ring count (4) est obtenu par le
  fallback ioctl `ETHTOOL_GRXRINGS` de `get_num_rings()` = chemin exact du patch → actif.
- NB : `ethtool -l eth1` (CHANNELS) = « netlink error: Not supported » → driver sans
  channels netlink, indépendant du patch Oltean.

## Conclusion
Patch intégré upstream **dès ethtool 6.11** (tarball daté oct. 2024, cohérent avec le
soumission sept. 2024), conservé jusqu'à 7.1. Les deux versions portées par le fork
(6.11 historique, 7.1 actuel/flashé r40980) contiennent le patch. Aucun patch custom
dans `package/network/utils/ethtool/` (dossier `patches/` absent). Sur le routeur,
**`ethtool-full` 7.1** = la variante qui compile netlink/rss.c → patch actif en pratique
(démontré par `ethtool -x eth1`).

## Sources locales
- /tmp/ethtool-check/ethtool-6.11.tar.xz + ethtool-7.1.tar.xz (hashs vérifiés ci-dessus)
- netlink/rss.c extraits : /tmp/ethtool-check/ethtool-{6.11,7.1}/netlink/rss.c
