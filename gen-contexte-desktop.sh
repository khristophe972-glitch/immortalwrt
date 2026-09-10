#!/usr/bin/env bash
# Régénère contexte-projet-desktop.md : concaténation des 7 fichiers d'état,
# destinée à l'upload MANUEL dans le projet Claude Desktop.
#
# Usage :  ./gen-contexte-desktop.sh [note libre pour l'en-tête]
# Exemple : ./gen-contexte-desktop.sh "bloc resume 4e partie + puces CLAUDE.md réécrites"
#
# Le script s'auto-vérifie (chaque bloc doit être identique à sa source) et
# affiche les 7 chemins à déposer dans le projet Desktop.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$REPO/contexte-projet-desktop.md"
NOTE="${1:-}"
cd "$REPO"

FILES=(
  CLAUDE.md
  resume_session_gl-mt6000.txt
  .claude/docs/patches-fork.md
  .claude/docs/systeme-routeur.md
  .claude/docs/qos-dscp.md
  .claude/docs/instruments.md
  .claude/docs/chantiers-clos.md
)

for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "ERREUR : fichier manquant : $f" >&2; exit 1; }
done

GEN="$(date '+%Y-%m-%d %H:%M')"
RS_C="$(wc -c < resume_session_gl-mt6000.txt)"
BLOCKS="$(grep -c '^=== SESSION' resume_session_gl-mt6000.txt || true)"

LIST=""
i=1
for f in "${FILES[@]}"; do
  LIST+="$(printf '%d. `%s` (%s c, %s lignes)' "$i" "$f" "$(wc -c < "$f")" "$(wc -l < "$f")")"
  LIST+=$'\n'
  i=$((i + 1))
done

{
  cat <<EOF
# Contexte projet — GL-MT6000 / ImmortalWrt

> **Généré le $GEN** — concaténation destinée à l'upload dans un projet Claude Desktop.
> **Le disque local (\`$REPO/\`) fait foi** : ce fichier est une copie manuelle.

Vérifier la fraîcheur d'une copie uploadée : la ligne 3 ci-dessus doit porter la même date/heure,
et le total doit être **$RS_C octets / $BLOCKS blocs de session** (voir la liste ci-dessous).

Fichiers concaténés, dans l'ordre :

$LIST
EOF
  if [ -n "$NOTE" ]; then
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M')" "$NOTE"
  fi
  echo '--- # FIN DE L'"'"'EN-TÊTE ---'

  for f in "${FILES[@]}"; do
    printf '\n--- # FICHIER: %s ---\n\n' "$f"
    cat "$f"
  done
} > "$OUT"

# --- auto-vérification : chaque bloc doit être byte-identique à sa source ---
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
ok=0
total=${#FILES[@]}
for f in "${FILES[@]}"; do
  start="$(grep -n "^--- # FICHIER: $f ---\$" "$OUT" | cut -d: -f1)"
  nxt="$(grep -n '^--- # FICHIER: ' "$OUT" | awk -F: -v s="$start" '$1 > s { print $1; exit }')"
  if [ -z "$nxt" ]; then end="$(wc -l < "$OUT")"; else end=$((nxt - 2)); fi
  sed -n "$((start + 2)),${end}p" "$OUT" > "$tmp"
  if diff -q "$f" "$tmp" >/dev/null; then
    ok=$((ok + 1))
  else
    echo "DIFF dans le bloc $f :" >&2
    diff "$f" "$tmp" | head -10 >&2
  fi
done

echo "contexte-projet-desktop.md régénéré : $(wc -c < "$OUT") octets / $(wc -l < "$OUT") lignes"
echo "intégrité : $ok/$total blocs identiques aux sources"
[ "$ok" -eq "$total" ] || { echo "ECHEC d'intégrité — ne pas uploader" >&2; exit 1; }
echo
echo "À déposer dans le projet Claude Desktop (les fichiers de projet ne se"
echo "synchronisent PAS avec le disque : supprimer l'ancienne copie, uploader) :"
echo
for f in "${FILES[@]}"; do echo "  $REPO/$f"; done
echo
echo "Astuce : Ctrl+H dans le sélecteur de fichiers pour voir le dossier .claude/"
