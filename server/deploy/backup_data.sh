#!/usr/bin/env bash
# Backup local rotativo: /opt/isolated/backups/isolated-AAAA-MM-DD-HHMM.tar.gz (contas, recibos, estado do mundo e o mundo). Mantém os KEEP mais novos.
# As contas são gravadas de forma atômica (tmp + rename), então cada arquivo no tar é consistente mesmo com o servidor rodando.
set -euo pipefail
KEEP="${KEEP:-14}"; B=/opt/isolated/backups; mkdir -p "$B"
F="$B/isolated-$(date -u +%Y-%m-%d-%H%M).tar.gz"
tar -czf "$F.tmp" -C /opt/isolated --exclude='*.tmp' data worlds
mv -f "$F.tmp" "$F"
ls -1t "$B"/isolated-*.tar.gz | tail -n +$((KEEP + 1)) | xargs -r rm -f
ls -1t "$B"/pre-deploy-*.tar.gz 2>/dev/null | tail -n +11 | xargs -r rm -f   # backups automáticos do deploy: guarda os 10 últimos
echo "backup: $F ($(du -h "$F" | cut -f1)); mantidos $(ls -1 "$B"/isolated-*.tar.gz | wc -l)"
