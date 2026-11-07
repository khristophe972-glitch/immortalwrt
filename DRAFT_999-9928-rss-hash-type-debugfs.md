# DRAFT — 999-9928 : exposer le type de hachage RSS (debugfs)

**Statut : DRAFT. Non appliqué dans `target/linux/mediatek/patches-6.18/`.**
Aucune compilation locale (règle 9), aucune CI, aucun push (règle 3) avant ta validation.

## But

Le moteur RSS du MT7986 est armé (patch `999-2711`, autrice Mason Chang) avec un
**type de hachage écrit en dur** dans deux champs de 3 bits de `PDMA_RSS_GLO_CFG` :

| champ | bits | macro | valeur |
|---|---|---|---|
| IPv4 hash type | 14:12 | `MTK_RSS_IPV4_STATIC_HASH` | `0x7` |
| IPv6 hash type | 10:8  | `MTK_RSS_IPV6_STATIC_HASH` | `0x7` |

Ces deux valeurs `0x7` ne sont **documentées nulle part** dans l'arbre : `grep -rn
STATIC_HASH` ne renvoie que les deux définitions (`mtk_eth_soc.h`) et les deux
usages (`mtk_rss_init()`). Le patch de définitions de registres
(`add-rss-lro-reg`) ne donne que l'offset. Côté userspace, `mtk_get/set_rxfh()`
n'exposent que clé + table d'indirection, et `set_rxfh()` rejette toute fonction
de hachage autre que `ETH_RSS_HASH_TOP`.

Observation terrain : le hachage semble **IP-niveau** (4 flux iperf3 parallèles
vers la même IP distante se sont concentrés à 95 pourcent sur le ring 0). On veut
savoir si une autre valeur des 3 bits donne un hachage 5-tuple.

## Ce que fait ce patch

Expose les deux champs via un nœud debugfs lecture/écriture, pour sonder le
matériel **sans reconstruire le noyau à chaque valeur** :

- `read`  : lit `RSS_GLO_CFG` et décode les deux champs.
- `write` : stage la nouvelle valeur puis la **latche avec le même pulse
  `CFG_REQ` que `mtk_rss_init()`** ; `RSS_EN` et tous les autres bits sont
  préservés par le read-modify-write.
- Valeur par défaut = valeur courante ; **aucun changement tant qu'on n'écrit
  pas** → aucune régression de boot possible.
- **Non persistant** : `mtk_rss_init()` reprogramme `0x7` à chaque boot. Un
  reboot = retour à l'état actuel. Pas de fichier de rollback nécessaire.

Chemin : `/sys/kernel/debug/mtk_rss/hash_type`

## Emplacement prévu

    target/linux/mediatek/patches-6.18/999-9928-net-ethernet-mtk_eth_soc-add-rss-hash-type-debugfs.patch

(trie après `999-9927`, avant `999-9999` ; le fichier `999-9926` du fork ajoute
déjà un knob debugfs — même style.)

## Le patch complet

```diff
From: khristophe972-glitch <khristophe972@gmail.com>
Subject: [PATCH] net: ethernet: mtk_eth_soc: expose RSS hash type through debugfs

mtk_rss_init() programs the hash type of the MTK RSS engine by setting two
3-bit fields of PDMA_RSS_GLO_CFG to a hardcoded value (the
MTK_RSS_IPV4/IPV6_STATIC_HASH defines, both 0x7).  The meaning of that
value is not documented anywhere in the tree: the only occurrences are the
two defines in mtk_eth_soc.h and their two uses in mtk_rss_init(), and the
register-definition patch that added them (add-rss-lro-reg) only provides
the offset.  ethtool cannot reach the field either: mtk_get/set_rxfh()
expose the hash key and the indirection table only, and set_rxfh() rejects
any hash function other than ETH_RSS_HASH_TOP.

Expose the two fields as a read/write debugfs node so that the hardware can
be probed empirically without rebuilding the kernel for each value.  The
write path stages the new value and then latches it with the same CFG_REQ
pulse mtk_rss_init() uses, so the field is committed exactly the way the
driver commits it at init; RSS_EN and every other bit are preserved by the
read-modify-write.  The node defaults to the current value and changes
nothing unless it is written, so it cannot regress a boot.

Read:  cat /sys/kernel/debug/mtk_rss/hash_type
Write: echo "<ipv4> <ipv6>" > /sys/kernel/debug/mtk_rss/hash_type
       (both fields 0..7; 7 is the value mtk_rss_init() programs today)

Signed-off-by: khristophe972-glitch <khristophe972@gmail.com>
---
 drivers/net/ethernet/mediatek/mtk_eth_soc.c | 106 +++++++++++++++++++++
 drivers/net/ethernet/mediatek/mtk_eth_soc.h |   2 +
 2 files changed, 108 insertions(+)

--- a/drivers/net/ethernet/mediatek/mtk_eth_soc.h
+++ b/drivers/net/ethernet/mediatek/mtk_eth_soc.h
@@ -226,6 +226,8 @@
 #define MTK_RSS_CFG_REQ			BIT(2)
 #define MTK_RSS_IPV6_STATIC_HASH	(0x7 << 8)
 #define MTK_RSS_IPV4_STATIC_HASH	(0x7 << 12)
+#define MTK_RSS_IPV6_HASH_TYPE		GENMASK(10, 8)
+#define MTK_RSS_IPV4_HASH_TYPE		GENMASK(14, 12)
 #define MTK_RSS_HASH_KEY_DW(x)		(reg_map->pdma.rss_glo_cfg + 0x20 +	\
 					 ((x) * 0x4))
 #define MTK_RSS_INDR_TABLE_DW(x)	(reg_map->pdma.rss_glo_cfg + 0x50 +	\
--- a/drivers/net/ethernet/mediatek/mtk_eth_soc.c
+++ b/drivers/net/ethernet/mediatek/mtk_eth_soc.c
@@ -28,6 +28,9 @@
 #include <linux/phy/phy.h>
 #include <linux/jhash.h>
 #include <linux/bitfield.h>
+#include <linux/debugfs.h>
+#include <linux/string.h>
+#include <linux/uaccess.h>
 #include <linux/ethtool.h>
 #include <linux/netlink.h>
 #include <net/dsa.h>
@@ -3768,6 +3771,107 @@
 	mtk_w32(eth, val, reg_map->pdma.rss_glo_cfg);
 }
 
+#ifdef CONFIG_DEBUG_FS
+
+static int mtk_rss_hash_type_show(struct seq_file *m, void *v)
+{
+	struct mtk_eth *eth = m->private;
+	u32 val;
+
+	if (!eth->soc->rss_num)
+		return -EOPNOTSUPP;
+
+	val = mtk_r32(eth, eth->soc->reg_map->pdma.rss_glo_cfg);
+	seq_printf(m, "ipv4=%lu ipv6=%lu\n",
+		   FIELD_GET(MTK_RSS_IPV4_HASH_TYPE, val),
+		   FIELD_GET(MTK_RSS_IPV6_HASH_TYPE, val));
+
+	return 0;
+}
+
+static ssize_t mtk_rss_hash_type_write(struct file *file,
+				       const char __user *ubuf, size_t size,
+				       loff_t *ppos)
+{
+	struct seq_file *m = file->private_data;
+	struct mtk_eth *eth = m->private;
+	unsigned int ipv4, ipv6;
+	char buf[32];
+	u32 val;
+
+	if (!eth->soc->rss_num)
+		return -EOPNOTSUPP;
+
+	if (size >= sizeof(buf))
+		return -E2BIG;
+	if (copy_from_user(buf, ubuf, size))
+		return -EFAULT;
+	buf[size] = '\0';
+
+	if (sscanf(buf, "%u %u", &ipv4, &ipv6) != 2) {
+		/* whitespace-only write (echo from a shell builtin): no-op */
+		if (!*strim(buf))
+			return size;
+		return -EINVAL;
+	}
+	if (ipv4 > 7 || ipv6 > 7)
+		return -EINVAL;
+
+	/* Stage the new hash type, then latch it with the same CFG_REQ
+	 * pulse mtk_rss_init() uses.  RSS_EN and every other bit are
+	 * preserved by the read-modify-write.
+	 */
+	val = mtk_r32(eth, eth->soc->reg_map->pdma.rss_glo_cfg);
+	val &= ~(MTK_RSS_IPV4_HASH_TYPE | MTK_RSS_IPV6_HASH_TYPE);
+	val |= FIELD_PREP(MTK_RSS_IPV4_HASH_TYPE, ipv4) |
+	       FIELD_PREP(MTK_RSS_IPV6_HASH_TYPE, ipv6);
+	mtk_w32(eth, val, eth->soc->reg_map->pdma.rss_glo_cfg);
+
+	val |= MTK_RSS_CFG_REQ;
+	mtk_w32(eth, val, eth->soc->reg_map->pdma.rss_glo_cfg);
+	val &= ~MTK_RSS_CFG_REQ;
+	mtk_w32(eth, val, eth->soc->reg_map->pdma.rss_glo_cfg);
+
+	*ppos += size;
+
+	return size;
+}
+
+static int mtk_rss_hash_type_open(struct inode *inode, struct file *file)
+{
+	return single_open(file, mtk_rss_hash_type_show, inode->i_private);
+}
+
+static const struct file_operations mtk_rss_hash_type_fops = {
+	.owner = THIS_MODULE,
+	.open = mtk_rss_hash_type_open,
+	.read = seq_read,
+	.write = mtk_rss_hash_type_write,
+	.llseek = seq_lseek,
+	.release = single_release,
+};
+
+static void mtk_rss_debugfs_init(struct mtk_eth *eth)
+{
+	struct dentry *root;
+
+	if (!eth->soc->rss_num)
+		return;
+
+	root = debugfs_create_dir("mtk_rss", NULL);
+	if (IS_ERR_OR_NULL(root))
+		return;
+
+	debugfs_create_file("hash_type", 0600, root, eth,
+			    &mtk_rss_hash_type_fops);
+}
+
+#else
+
+static inline void mtk_rss_debugfs_init(struct mtk_eth *eth) { }
+
+#endif
+
 static netdev_features_t mtk_fix_features(struct net_device *dev,
 					  netdev_features_t features)
 {
@@ -6520,6 +6624,8 @@
 		mtk_ppe_internal_debugfs_init(eth);
 	}
 
+	mtk_rss_debugfs_init(eth);
+
 	/* we run 2 devices on the same DMA ring so we need a dummy device
 	 * for NAPI to work
 	 */
```

## Procédure de balayage (après flash CI)

Lecture de l'état initial :

```
cat /sys/kernel/debug/mtk_rss/hash_type
```

Balayage des 8 valeurs (une commande par ligne) :

```
echo "0 0" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "1 1" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "2 2" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "3 3" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "4 4" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "5 5" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "6 6" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
echo "7 7" > /sys/kernel/debug/mtk_rss/hash_type
cat /sys/kernel/debug/mtk_rss/hash_type
```

Pour chaque valeur, la discrimination se fait sur la **distribution des rings
RX 0-3** (`/proc/interrupts`, lignes 119-122) sous un flux `-P 4` vers une même
IP distante. Un mode 5-tuple doit étaler sur les 4 rings ; le mode actuel
concentre.

> Le test de charge associé requiert ton **accord explicite** (règle 4).
> Le nœud seul, sans trafic, peut se balayer à vide pour vérifier qu'il lit et
> écrit correctement.

## Rollback

```
echo "7 7" > /sys/kernel/debug/mtk_rss/hash_type
```

ou, plus radical et toujours disponible : **reboot** (rien n'est persistant).

## Non couvert

- Pas de compilation locale (règle 9) : validation uniquement via CI.
- Pas de push / pas de lancement de CI sans demande explicite (règle 3).
