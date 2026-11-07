# RESUME — Audit mac80211 / mt76 (3 axes) — 04/09/2026

**Statut : AUDIT TERMINÉ — document non commité (règle 16), validation Christophe acquise sur les axes A, B, C1 (+ inventaire C2).**
Aucune modification de fichier, de patch, de commit ni de CI pendant l'audit. Téléchargements externes = fichiers individuels uniquement (raw.githubusercontent.com). Ambiguïtés documentées, jamais conclues.

**Périmètre** : fork ImmortalWrt GL-MT6000 (branche openwrt-6.18), firmware r40965-f962474556, kernel 6.18.44, **paquet backports mac80211 v6.18.39** (package/kernel/mac80211 — PAS le mac80211 du kernel), pin mt76 `59676919ea` (2026-07-01), 29 patches dans `package/kernel/mt76/patches/`.

---

## Synthèse exécutive

| Axe | Objet | Verdict |
|---|---|---|
| **A** | Compatibilité API mac80211/mt76 des patches vs 6.18→master | **0 BLOQUANT** — signatures identiques ; À SURVEILLER = uniquement de la maintenance de bump |
| **B** | Applicabilité des 29 patches au pin (apply cumulatif) | **29/29 OK** — 6 propre, 20 offset contexte exact, 2 DRIFT fuzz réel non obsolétisant, 1 fuzz artefact GNU |
| **C** | Features mac80211 inexploitées (C1 driver + C2 repo) | **0 opportunité franche** — 10 INFO (2 ambiguïtés) ; inventaire repo établi pour dé-dup |

**Conclusion d'ensemble** : le pin mt76 actuel (01/07/2026) est **sain** — la série de patches est cohérente avec lui et aucune évolution API 6.18→master n'invalide un patch. Les enseignements ne deviennent pertinents **qu'au moment d'un futur bump** (pin mt76 ou paquet backports).

---

## AXE A — Compatibilité API mac80211/mt76 (6.18 vs master)

**BLOQUANT : aucun.**

Tous les symboles mac80211 référencés par les 29 patches ont une signature et un layout **strictement identiques** entre kernel v6.18 et master : `post_channel_switch`, `add_twt_setup`, `rx_status.chains`/`chain_signal[]`, `radar_detected`, `ieee80211_next_txq`, `ieee80211_stop/wake_queues`, `ieee80211_restart_hw`, `netdev_features`, `IEEE80211_TX_CTL_HW_80211_ENCAP` (BIT(14)), `cfg80211_chandef_dfs_required`, `cfg80211_background_radar_event`. Struct `ieee80211_ops` : 127→129 callbacks (2 ajouts sans rapport, 0 retrait).

Symboles internes mt76 pin→master identiques : `mt76_eeprom_override`, `mt76_wcid.def_wcid` (+ helper `mt76_wcid_primary()` déjà amont), `mt76_token_consume`, `mt76_roc_complete`/`abort_roc`, `mt76_hw_scan`, `mt76_scan_complete`/`abort_scan`, `mt76_txq_schedule_pending_wcid`, `mt76_dma_rx_process`/`mt76_dma_get_buf`.

Symboles « prioritaires » du prompt **absents des 29 patches** (0 occurrence, y compris mediatek/patches-6.18) : `mt76_wed_rro_ind`, `mt76_dma_queue_reset`, `mt76_select_links`, `net_setup_tc`, `page_pool_alloc_frag`. Rien à conclure.

### À SURVEILLER (maintenance de bump, PAS un défaut actuel)

1. **0028→0037 : déjà fusionnés dans mt76 master** (vérifié : mcu.c:1802-1805, scan.c:235, channel.c:334, mac.c:1959/1391/1474/224, mt76_connac_mac.c sans « skip VO »). Au bump master il faut les **retirer** (sinon échec d'application ou double-patch). Corollaire positif : nos 9 backports = code upstream canonique.
2. **0020** : le contexte `status->chains = mphy->antenna_mask` n'existe plus en master, remplacé par le helper `mt7915_band_chainmask(phy)` (dérivé de `band_idx * chainshift`, mt7915.h:429). Intention probablement couverte amont — à re-vérifier fonctionnellement à l'occasion (sémantique phy_idx vs band_idx).
3. **0013, 9602, 9051, 9999-17/18, 9010** : extensions locales non upstream — symboles inchangés mais contextes tx.c/dma.c/init.c dérivés en master → reprise manuelle probable au bump.

**INFO** : tout le reste (détail tableaux ci-dessus). Sources toutes joignables, aucune « non vérifiée ».

---

## AXE B — Applicabilité des 29 patches au pin (apply cumulatif)

**Méthode** : 22 fichiers cibles (union des `diff --git`) téléchargés au pin — tous HTTP 200, aucun absent. Série rejouée cumulativement en ordre `ls -1` (dry-run puis apply réel, GNU patch 2.7.6 + cross-check `git apply` sur les patches à fuzz).

**Verdict : 29/29 dry-run, 29/29 apply réel, 0 hunk refusé.**

| Catégorie | Nb | Patches |
|---|---|---|
| OK propre (0/0) | 6 | 0000_100, 0020, 100, 101, 9602, 9999-18 |
| OK avec offset, contexte exact | 20 | dont 0028 (−8), 0034 (+1), 0035 (+1 / −66 mt7996), 0037 (−34), 6014 (−89), 9999-06 (≤145), 9999-17 (−6), 9604 (−8) |
| DRIFT léger (fuzz réel) | 2 | **0007** (fuzz 1 : `post_channel_switch` ABSENT au pin → ajout légitime), **9999-05** (fuzz 2 : pin a réécrit le prologue `mt7915_mac_sta_poll`, mais `rcu_read_lock()` encore APRÈS le splice → fix UAF toujours requis) |
| Fuzz signalé = artefact GNU patch | 1 | **9010** (fuzz 2 cosmétique ; contexte identique octet par octet, `git apply` exact, sans conséquence) |
| OBSOLETE / CONFLICT / INDÉTERMINÉ | 0 | — |

### Points documentés
- **9999-18 (orphelin)** : s'applique proprement sur du code vivant (`mt76_txq_schedule_list`) ; le pin ne contient PAS le garde `wcid_phy` → non redondant. Portée réelle non confirmable sans test dual-band réel.
- **100 + portions mt7996/mt7921/mt7925 (0013, 0035, 0036, 0037)** : applicables, non redondants, mais code non compilé/exercé sur GL-MT6000 (MT7915 seul) → **inertes au runtime**, pas un risque.
- Le build CI rejouera les mêmes fuzz cosmétiques sans conséquence fonctionnelle.

---

## AXE C — Features mac80211 inexploitées

### Correction d'angle (C2)
La stack mac80211/cfg80211 **n'est pas intégrée au kernel 6.18.44** : c'est le paquet **backports `package/kernel/mac80211` v6.18.39**. Le vrai facteur de risque d'un futur bump est le **paquet backports**, pas le kernel. (Sans impact sur les verdicts AXE A : v6.18 = proxy fidèle des backports 6.18.39.)

### AXE C1 — Candidats driver (filtre pertinence AP WiFi 6 non-MLO, DBDC, QoS/DFS)
`ieee80211_ops` v6.18 : 127 prototypes ; **mt7915@pin en implémente 55** (mt7915/main.c:1842). Après filtre : **0 À SURVEILLER franc, 10 INFO** (2 ambiguïtés notées) ; la majorité des ~72 non implémentés est hors périmètre (STA/mesh/TDLS/NAN/MLO-EHT/P2P/WoWLAN/PMF).

| Callback / Feature | Verdict | Justification |
|---|---|---|
| `set_tid_config` / `reset_tid_config` | INFO | conf_tx AC + BSS_CHANGED_QOS déjà câblés ; réglage par-STA inutilisé par hostapd ni le marquage AF41 |
| `change_interface` | INFO | types fixes (AP) ; retombée mac80211 add/remove_interface |
| `set_tim` + `allow_buffered_frames` | INFO | AP_LINK_PS posé (mt76_mac80211.c:488) → PS/TIM délégué firmware ; `mt76_set_tim` = stub no-op exporté non câblé ; seul `release_buffered_frames` présent |
| `channel_switch` / pre / post / abort | INFO (**ambiguïté DFS**) | `.channel_switch` = CSA côté STA ; AP via `config()`+`channel_switch_beacon` (main.c:734) ; complétion countdown (`mt76_csa_finish`) absente des fichiers vus — contexte DFS HE160 ch128, à rapprocher des backports 0029-0031 |
| `flush` | INFO | no-op toléré ; drain non critique ici, ambigu en marge des fixes SER 0033-0035 |
| `update_vif_offload` | INFO | encap offload posé statiquement à add_interface + par-STA via `sta_set_decap_offload` ; pas de bascule vif dynamique |
| `remain_on_channel` / `cancel` | INFO | helpers mt76 dispo (mt76.h:1653) non câblés ; ROC = P2P/offchannel sans objet AP |
| `WIPHY_FLAG_4ADDR_AP` | INFO | callback `sta_set_4addr` présent, flag absent (init.c:385) → capacité WDS latente inatteignable via nl80211 ; pas de WDS dans ce setup |
| `NL80211_EXT_FEATURE_STA_TX_PWR` | INFO (**ambiguïté**) | `sta_set_txpwr` présent mais feature gated `!is_mt7915` (init.c:399) — gating probablement volontaire (limite firmware) |
| `prep_add_interface` (récent) | INFO | hook de préparation ; AP fixe sans besoin |

### Note ABI transverse (C1)
`.sta_set_airtime_weight = mt7915_sta_set_airtime_weight` (main.c:1887, def :1374) **n'existe dans AUCUN header torvalds v6.17→v6.19/master** → le pin cible une mac80211 **backports/OpenWrt étendue** (API VoW/airtime), pas la 6.18 vanilla. Cohérent avec le contexte VoW du fork (0028, vow_band). Aucun appel `ieee80211_*` disparu repéré dans main.c.

### AXE C2 — Inventaire repo (dé-dup, déjà activé/configuré)

**Config kernel (fragments generic/filogic + kmods)** :
- MAC80211/CFG80211 = **backports** (hors kernel). Mesh 802.11s = y (déf.), debugfs mac80211/cfg80211 = y, LEDs = y, minstrel HT/VHT = y (permanent). TESTMODE = n, WEXT = n, XDP_SOCKETS = n (CI BTF=y → y), CFG80211_CERTIFICATION_ONUS = module forcé backports.
- PAGE_POOL/STATS = y (filogic + mt76), BPF_SYSCALL/JIT = y, NET_SCH_CAKE/RED/NETEM + BPF/mirred/ctinfo = m via kmods, fq_codel/ingress = y/m.
- **WED = y + `MODPARAMS.mt7915e=wed_enable=Y`** (mt76:241-243), `CONFIG_MT798X_WMAC=y` (:483), RELAY activé par mt7915e, package mt7986-firmware.

**Driver mt76** : pin `59676919ea` ; dépendances `kmod-mac80211 +@DRIVER_11AC_SUPPORT +@KERNEL_PAGE_POOL` ; mt7915e `+@DRIVER_11AX_SUPPORT +@KERNEL_RELAY`.

**DTS** (`mt7986a-glinet-gl-mt6000.dts`) : `&wifi` status ok (:292-298), pinctrl wf_2g_5g_pins (4 mA) (:189-205), eeprom@0 0x1000 (:342-344) — pas d'`ieee80211-freq-limit` ni mac-address WiFi. Device packages filogic : `kmod-mt7915e kmod-mt7986-firmware mt7986-wo-firmware kmod-sched-bpf`.

**hostapd / wifi-scripts — déjà bridgé dans l'arbre** (hors candidats C1) : 802.11k/v/r/w (rrm/roaming/bss_transition), WNM, MBO, OCV, **QoS Map + interworking 802.11u** (ap.uc:69, 538-556), HS20, FTM, multi_ap, MLO, **TWT** (`he_twt_*`), **SR spatial reuse** (`he_spr_*`, hostapd.uc:410-415), PMF/OWE/SAE/DPP/MESH (wpad-openssl full, filogic/target.mk).

---

## Implications si futur bump (documentation — AUCUNE action proposée)

1. **Bump pin mt76 vers master** : retirer 0028-0037 (déjà amont) ; ré-évaluer 0020 (helper `mt7915_band_chainmask`) ; reprise manuelle probable des extensions locales 0013, 9010, 9051, 9602, 9999-17/18. AXE B garantit que l'état actuel reste cohérent jusqu'au bump.
2. **Bump paquet backports mac80211** : c'est le vrai risque d'API (mac80211 = backports 6.18.39, pas le kernel). L'ABI VoW/airtime (`sta_set_airtime_weight`) est une extension OpenWrt/backports — à vérifier à chaque bump.
3. **Aucun défaut actif** sur le pin 59676919ea / firmware r40965. Verdicts A/B/C tous INFO ou maintenance de bump.

---

*Audit mené avec des agents dédiés (code-explorer-vision, patch-reviewer-vision, parallel-check-vision), orchestré et consolidé par l'orchestrateur. Fichiers de référence téléchargés dans /tmp (mac80211.h/cfg80211.h v6.18+master, sources mt76 au pin + master). Document NON commité.*

---

## Addendum 05/09 — DNS upstream → AdGuard (changement réel, vérifié routeur)

- **dnsproxy DoQ upstream changé (04/09) : Quad9 → AdGuard non filtré.** Valeur active vérifiée sur le routeur (05/09, SSH lecture seule) :
  `dnsproxy.servers.upstream = 'quic://94.140.14.15:853' 'quic://94.140.15.16:853'`
  Bootstrap inchangé : `94.140.14.140:53` + `1.1.1.1:53`. Écoute `0.0.0.0:5335`.
  Sur Free SAS Paris, UDP 853 non filtré pour AdGuard (confirmé). Validé : `dig @127.0.0.1 -p 5335 google.com` → 142.251.39.206.
- **Contexte** : ce changement figurait dans un bloc externe « SESSION 05/09 (2e partie) » qui mêlait aussi un bump mt76 be5ce791 « actif » (6 commits 104e78aff4→7a2bf30479) et une réactivation qosify-flow-exclude. **Vérifiés le 05/09 : ces 2 éléments ne correspondent PAS à l'état réel** — repo local + origin/openwrt-6.18 = tip `4d948973db` (pin `59676919ea`, 33 patches), les 6 commits cités sont ceux de l'ANCIEN bump be5ce791 sous le revert `3a8774b563` (04/09) ; routeur : Option B toujours OFF (rc.local commenté, pas de S98 qosify-flow-exclude). → **ÉCARTÉS du dossier** (artefact de clone périmé). Seul le DNS AdGuard = réel, enregistré ici + CLAUDE.md §DNS + auto-mémoire.

Document NON commité.
