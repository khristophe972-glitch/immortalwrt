# qos-mt6000 et luci-app-qos-mt6000

Gestion du qdisc racine `cake_mq` du port WAN du GL-MT6000 : un paquet backend
(`qos-mt6000`) et sa page LuCI (`luci-app-qos-mt6000`, menu Reseau > GL-MT6000 QoS).

## Contenu

| Fichier | Role |
|---|---|
| `package/qos-mt6000/files/usr/lib/qos-mt6000/qos-mt6000.sh` | programme : start, stop, check (dry-run), status |
| `package/qos-mt6000/files/etc/init.d/qos-mt6000` | script de demarrage procd (START=26), recharge sur changement de `/etc/config/qos-mt6000` et sur `ifup` de l'interface declenchante |
| `package/qos-mt6000/files/etc/config/qos-mt6000` | configuration UCI, valeurs par defaut = qdisc de production du 04/10/2026 |
| `package/qos-mt6000/tests/test-qos-mt6000.sh` | 77 tests avec faux tc/uci/logger (`sh tests/test-qos-mt6000.sh`, ou `busybox ash`) |
| `package/luci-app-qos-mt6000/` | page LuCI : menu, ACL rpcd, vue JavaScript |

## Comportement

* `start` valide TOUTES les valeurs UCI avant de construire la commande ; une valeur invalide
  arrete l'operation sans toucher au qdisc (aucune injection possible : chaque valeur est
  comparee a une liste ou a un motif strict).
* Le qdisc est remplace par `tc qdisc replace` (jamais `change`), puis verifie par `tc qdisc show`.
* Les reglages debugfs de cake_mq (`sync_time_ns`, `active_window_ns`, `active_release`,
  `tin_share`, `tin_hold`, `tin_cap`) sont ecrits avec `printf` apres le qdisc. Une option
  absente ou vide n'est pas ecrite.
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
