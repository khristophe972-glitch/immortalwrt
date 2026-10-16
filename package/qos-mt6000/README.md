# qos-mt6000 et luci-app-qos-mt6000

Gestion du qdisc racine `cake_mq` du port WAN du GL-MT6000 : un paquet backend
(`qos-mt6000`) et sa page LuCI (`luci-app-qos-mt6000`, menu Reseau > GL-MT6000 QoS).

## Contenu

| Fichier | Role |
|---|---|
| `package/qos-mt6000/files/usr/lib/qos-mt6000/qos-mt6000.sh` | programme : start, stop, check (dry-run), status |
| `package/qos-mt6000/files/etc/init.d/qos-mt6000` | script de demarrage procd (START=26), recharge sur changement de `/etc/config/qos-mt6000` et sur `ifup` de l'interface declenchante |
| `package/qos-mt6000/files/etc/config/qos-mt6000` | configuration UCI, valeurs par defaut = qdisc de production du 04/10/2026 |
| `package/qos-mt6000/tests/test-qos-mt6000.sh` | 123 tests avec faux tc/uci/logger (`sh tests/test-qos-mt6000.sh`, ou `busybox ash`) |
| `package/luci-app-qos-mt6000/` | page LuCI : menu, ACL rpcd, vue JavaScript |

## Comportement

* `start` valide TOUTES les valeurs UCI avant de construire la commande ; une valeur invalide
  arrete l'operation sans toucher au qdisc (aucune injection possible : chaque valeur est
  comparee a une liste ou a un motif strict).
* Le qdisc est remplace par `tc qdisc replace` (jamais `change`), puis verifie par `tc qdisc show`.
* Les reglages debugfs de cake_mq (`sync_time_ns`, `active_window_ns`, `active_release`,
  `active_hold`, `tin_share`, `tin_hold`, `tin_cap`, `rebalance`, `ewma_shift`,
  `v1_headroom_shift`, `v1_sat_shift`, `v2_spare_pct`) sont ecrits avec `printf` apres le
  qdisc. Une option absente ou vide n'est pas ecrite.
* `rebalance` : `0` partage egal amont (defaut du paquet), `1` v1 (une instance non saturee
  reserve sa marge disponible), `2` v2 (une instance saturee ne descend jamais sous la part
  egale). Hors `0`, le partage est recalcule a chaque synchronisation.
* Reglages fins du partage : `ewma_shift` (1 a 8), `v1_headroom_shift` (1 a 8) et `v1_sat_shift`
  (1 a 8) reglent le lissage et les seuils de `rebalance 1` ; `v2_spare_pct` (0 a 100) regle la
  part rendue aux instances saturees en `rebalance 2`. Sans effet hors du mode concerne.
  `active_hold` et `active_release` reglent l'hysteresis de l'activite ; le noyau ne les borne
  pas, mais le paquet borne `active_hold` : `active_hold` x `sync_time_ns` (le `hold` de
  999-719, ajoute au compteur u64 `last_active`) ne doit pas depasser 1 000 000 000 ns.
* `sync_time_ns` est borne a 1 a 1 000 000 000 ns (1 s). C'est la cadence de resynchronisation
  du diviseur, et ce plafond ferme le produit `u32 x sync_time_ns` des deux autres u32
  (`active_hold`, `tin_hold`) sous 2^64.
* `enabled '0'` : rien n'est modifie. `stop` supprime le qdisc racine (retour au qdisc par defaut).
* Un verrou (`/tmp/qos-mt6000.lock`) evite deux applications simultanees.

## Commandes

    /etc/init.d/qos-mt6000 check     # valide et affiche les commandes, sans rien appliquer
    /etc/init.d/qos-mt6000 reload    # applique
    /etc/init.d/qos-mt6000 status    # configuration, qdisc reel, reglages debugfs
    logread -e qos-mt6000            # journal

## Migration depuis rc.local

Les lignes de `/etc/rc.local` qui posent le qdisc d'eth1 et les reglages
`/sys/kernel/debug/cake_mq/*` font double emploi avec ce paquet et doivent etre
retirees, sinon la derniere a s'executer l'emporte :

    grep -n -E 'tc qdisc replace dev eth1|/sys/kernel/debug/cake_mq/' /etc/rc.local

Sauvegarder `rc.local`, montrer le diff, puis commenter ces lignes (voir la procedure
de deploiement fournie avec le paquet).

## Retour arriere

    /etc/init.d/qos-mt6000 stop
    /etc/init.d/qos-mt6000 disable
    cp -p /root/rc.local.avant-qos-mt6000 /etc/rc.local

## Tests

    sh package/qos-mt6000/tests/test-qos-mt6000.sh
    busybox ash package/qos-mt6000/tests/test-qos-mt6000.sh "busybox ash"
