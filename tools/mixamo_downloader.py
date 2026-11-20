#!/usr/bin/env python3
"""
Mixamo Bulk Animation Downloader
Automates exporting and downloading Mixamo animations into import assets.
Supports individual motions and motion packs, in-place toggle, retry logic, and progress checkpoints.
"""

import os
import sys
import glob
import time
import json
import signal
import sqlite3
import shutil
import tempfile
import argparse
from datetime import datetime

try:
    import cramjam
except ImportError:
    print("Erro: cramjam não instalado. Instale com: pip install cramjam")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("Erro: requests não instalado. Instale com: pip install requests")
    sys.exit(1)

# Default configuration
DEFAULT_CHAR_ID = "4f5d21e1-4ccc-41f1-b35b-fb2547bd8493"  # Y Bot
DEFAULT_OUT_DIR = "/home/neto/Área de trabalho/isolated/import assets"
BASE_URL = "https://www.mixamo.com/api/v1"
API_KEY = "mixamo2"

# Global run flag for clean shutdown
RUNNING = True

def handle_signal(sig, frame):
    global RUNNING
    print("\n[Mixamo Downloader] Interrupção recebida! Salvando progresso e finalizando...")
    RUNNING = False

signal.signal(signal.SIGINT, handle_signal)
signal.signal(signal.SIGTERM, handle_signal)
if hasattr(signal, "SIGHUP"):
    signal.signal(signal.SIGHUP, signal.SIG_IGN)


def sanitize_filename(name: str) -> str:
    invalid_chars = '<>:"/\\|?*'
    for c in invalid_chars:
        name = name.replace(c, "_")
    return " ".join(name.split()).strip()


def get_token_from_firefox() -> str:
    pattern = os.path.expanduser("~/.mozilla/firefox/*/storage/default/https+++www.mixamo.com/ls/data.sqlite")
    matches = glob.glob(pattern)
    if not matches:
        raise RuntimeError("Não foi possível encontrar o banco de dados do Mixamo no Firefox.")
    
    db_path = matches[0]
    tmp_path = tempfile.mktemp(suffix=".sqlite")
    try:
        shutil.copyfile(db_path, tmp_path)
        con = sqlite3.connect(tmp_path)
        cur = con.cursor()
        cur.execute("SELECT value FROM data WHERE key = 'access_token'")
        row = cur.fetchone()
        con.close()
        if not row or not row[0]:
            raise RuntimeError("Chave 'access_token' não encontrada no banco do Firefox.")
        
        raw = bytes(cramjam.snappy.decompress_raw(row[0]))
        token = raw.decode("utf-8")
        return token
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


class MixamoDownloader:
    def __init__(self, output_dir: str, character_id: str = DEFAULT_CHAR_ID, token: str = None, in_place: bool = True):
        self.output_dir = output_dir
        self.character_id = character_id
        self.in_place = in_place
        self.token = token or get_token_from_firefox()
        self.progress_file = os.path.join(output_dir, ".progress.json")
        self.catalog_file = os.path.join(output_dir, ".catalog_cache.json")
        self.log_file = os.path.join(output_dir, "download.log")
        
        os.makedirs(self.output_dir, exist_ok=True)
        self.progress = self.load_progress()

    def log(self, msg: str):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{timestamp}] {msg}"
        print(line, flush=True)
        try:
            with open(self.log_file, "a", encoding="utf-8") as f:
                f.write(line + "\n")
                f.flush()
        except Exception:
            pass

    def load_progress(self) -> dict:
        if os.path.exists(self.progress_file):
            try:
                with open(self.progress_file, "r", encoding="utf-8") as f:
                    return json.load(f)
            except Exception:
                pass
        return {"completed_ids": [], "failed_ids": []}

    def save_progress(self):
        try:
            with open(self.progress_file, "w", encoding="utf-8") as f:
                json.dump(self.progress, f, indent=2)
        except Exception as e:
            self.log(f"Aviso: Não foi possível salvar progresso: {e}")

    def get_headers(self) -> dict:
        return {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/json",
            "Content-Type": "application/json",
            "X-Api-Key": API_KEY,
            "X-Requested-With": "XMLHttpRequest",
        }

    def request_with_retry(self, method: str, url: str, **kwargs) -> requests.Response:
        max_retries = 5
        backoff = 3
        for attempt in range(1, max_retries + 1):
            try:
                res = requests.request(method, url, headers=self.get_headers(), **kwargs)
                if res.status_code == 401:
                    self.log("Token expirado! Tentando atualizar token a partir do Firefox...")
                    self.token = get_token_from_firefox()
                    continue
                if res.status_code == 429:
                    self.log(f"Taxa limite (429)! Aguardando {backoff}s antes de tentar novamente...")
                    time.sleep(backoff)
                    backoff *= 2
                    continue
                if res.status_code >= 500:
                    time.sleep(backoff)
                    backoff *= 2
                    continue
                res.raise_for_status()
                return res
            except requests.RequestException as e:
                if attempt == max_retries:
                    raise
                time.sleep(backoff)
                backoff *= 2
        raise RuntimeError(f"Falha na requisição {method} {url} após {max_retries} tentativas.")

    def fetch_full_catalog(self, force_refresh: bool = False) -> list:
        if not force_refresh and os.path.exists(self.catalog_file):
            try:
                with open(self.catalog_file, "r", encoding="utf-8") as f:
                    cached = json.load(f)
                    self.log(f"Catálogo carregado do cache ({len(cached)} itens).")
                    return cached
            except Exception:
                pass

        self.log("Obtendo catálogo completo de animações do Mixamo (26 páginas)...")
        all_items = []
        page = 1
        total_pages = 26
        while page <= total_pages:
            url = f"{BASE_URL}/products?page={page}&limit=96&order=&type=Motion%2CMotionPack&query="
            res = self.request_with_retry("GET", url).json()
            total_pages = res.get("pagination", {}).get("num_pages", total_pages)
            results = res.get("results", [])
            all_items.extend(results)
            self.log(f"Página {page}/{total_pages} carregada ({len(all_items)} animações acumuladas).")
            page += 1
            time.sleep(0.3)

        with open(self.catalog_file, "w", encoding="utf-8") as f:
            json.dump(all_items, f, indent=2)
        self.log(f"Catálogo salvo com sucesso ({len(all_items)} itens).")
        return all_items

    def export_and_download(self, gms_hash: dict, product_name: str, out_filepath: str) -> bool:
        if os.path.exists(out_filepath) and os.path.getsize(out_filepath) > 1000:
            return True

        if self.in_place and gms_hash.get("inplace") is not None:
            gms_hash["inplace"] = True

        params = gms_hash.get("params")
        if isinstance(params, list):
            gms_hash["params"] = ",".join(str(p[1]) for p in params)

        payload = {
            "character_id": self.character_id,
            "gms_hash": [gms_hash],
            "preferences": {
                "format": "fbx7",
                "skin": "false",
                "fps": "30",
                "reducekf": "0"
            },
            "product_name": product_name,
            "type": "Motion"
        }

        export_url = f"{BASE_URL}/animations/export"
        self.request_with_retry("POST", export_url, json=payload)

        # Polling monitor
        monitor_url = f"{BASE_URL}/characters/{self.character_id}/monitor"
        download_url = None
        for _ in range(40):
            time.sleep(1.8)
            mr = self.request_with_retry("GET", monitor_url).json()
            status = mr.get("status")
            if status == "completed":
                download_url = mr.get("job_result")
                break
            elif status == "failed":
                raise RuntimeError(f"Exportação falhou no servidor: {mr.get('message')}")

        if not download_url:
            raise TimeoutError("Tempo esgotado aguardando exportação do Mixamo.")

        dl_res = requests.get(download_url, stream=True, timeout=60)
        dl_res.raise_for_status()
        
        tmp_target = out_filepath + ".tmp"
        with open(tmp_target, "wb") as f:
            for chunk in dl_res.iter_content(chunk_size=65536):
                f.write(chunk)
        os.replace(tmp_target, out_filepath)
        return True

    def process_motion(self, item: dict) -> bool:
        anim_id = item["id"]
        name = item.get("name") or item.get("description", "animation")
        safe_name = sanitize_filename(name)
        out_filepath = os.path.join(self.output_dir, f"{safe_name}.fbx")

        if os.path.exists(out_filepath) and os.path.getsize(out_filepath) > 1000:
            return True

        prod_url = f"{BASE_URL}/products/{anim_id}?similar=0&character_id={self.character_id}"
        pdata = self.request_with_retry("GET", prod_url).json()
        gms = dict(pdata["details"]["gms_hash"])
        if pdata["details"].get("supports_inplace") and self.in_place:
            gms["inplace"] = True

        return self.export_and_download(gms, name, out_filepath)

    def process_pack(self, item: dict) -> bool:
        pack_id = item["id"]
        pack_name = item.get("name") or item.get("description", "pack")
        safe_pack_name = sanitize_filename(pack_name)
        pack_dir = os.path.join(self.output_dir, "_Packs", safe_pack_name)
        os.makedirs(pack_dir, exist_ok=True)

        prod_url = f"{BASE_URL}/products/{pack_id}?similar=0&character_id={self.character_id}"
        pdata = self.request_with_retry("GET", prod_url).json()
        motions = pdata.get("details", {}).get("motions", [])

        if not motions:
            # Fallback if gms_hash is single
            if pdata.get("details", {}).get("gms_hash"):
                safe_name = sanitize_filename(pack_name)
                out_path = os.path.join(pack_dir, f"{safe_name}.fbx")
                return self.export_and_download(pdata["details"]["gms_hash"], pack_name, out_path)
            return False

        all_ok = True
        for m in motions:
            global RUNNING
            if not RUNNING:
                break
            motion_name = m.get("name", "motion")
            safe_motion_name = sanitize_filename(motion_name)
            out_filepath = os.path.join(pack_dir, f"{safe_motion_name}.fbx")

            if os.path.exists(out_filepath) and os.path.getsize(out_filepath) > 1000:
                continue

            try:
                gms = dict(m["gms_hash"])
                self.export_and_download(gms, motion_name, out_filepath)
                size_kb = os.path.getsize(out_filepath) // 1024
                self.log(f"  [Pack: {safe_pack_name}] Baixado: {safe_motion_name}.fbx ({size_kb} KB)")
                time.sleep(0.3)
            except Exception as e:
                self.log(f"  [Pack: {safe_pack_name}] Erro em {safe_motion_name}: {e}")
                all_ok = False
        return all_ok

    def run(self, max_items: int = None):
        catalog = self.fetch_full_catalog()
        total = len(catalog)
        if max_items:
            total = min(total, max_items)
            catalog = catalog[:total]

        completed_set = set(self.progress.get("completed_ids", []))
        stats = {"downloaded": 0, "skipped": 0, "failed": 0}

        self.log(f"Iniciando download em lote para {total} itens do catálogo...")
        self.log(f"Destino: {self.output_dir}")
        self.log(f"Personagem ID: {self.character_id} (Y Bot)")
        self.log(f"Opções: FBX Binary (fbx7), Without Skin, 30 FPS, In Place: {self.in_place}")

        start_time = time.time()
        for idx, item in enumerate(catalog, start=1):
            global RUNNING
            if not RUNNING:
                break

            item_id = item["id"]
            item_type = item.get("type", "Motion")
            name = item.get("name", "unnamed")

            if item_id in completed_set:
                stats["skipped"] += 1
                continue

            percent = (idx / total) * 100
            self.log(f"[{idx}/{total}] ({percent:.1f}%) Processando {item_type}: \"{name}\"...")

            success = False
            for attempt in range(3):
                try:
                    if item_type == "MotionPack":
                        success = self.process_pack(item)
                    else:
                        success = self.process_motion(item)
                    break
                except Exception as e:
                    self.log(f"  Tentativa {attempt + 1}/3 falhou para \"{name}\": {e}")
                    time.sleep(2 * (attempt + 1))

            if success:
                stats["downloaded"] += 1
                completed_set.add(item_id)
                self.progress["completed_ids"].append(item_id)
                self.save_progress()
            else:
                stats["failed"] += 1
                self.progress["failed_ids"].append(item_id)
                self.save_progress()

            time.sleep(0.4)

        elapsed = time.time() - start_time
        self.log("=" * 60)
        self.log(f"Concluído! Tempo decorrido: {elapsed/60:.1f} min")
        self.log(f"Estatísticas: Baixados: {stats['downloaded']} | Existentes/Ignorados: {stats['skipped']} | Falhas: {stats['failed']}")
        self.log("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="Mixamo Bulk Animation Downloader")
    parser.add_argument("--out", default=DEFAULT_OUT_DIR, help="Diretório de saída")
    parser.add_argument("--char", default=DEFAULT_CHAR_ID, help="ID do personagem Mixamo")
    parser.add_argument("--max", type=int, default=None, help="Limite máximo de itens para download")
    parser.add_argument("--refresh-catalog", action="store_true", help="Força atualização do catálogo")
    parser.add_argument("--no-in-place", action="store_true", help="Desativa In Place")

    args = parser.parse_args()

    downloader = MixamoDownloader(
        output_dir=args.out,
        character_id=args.char,
        in_place=not args.no_in_place
    )

    downloader.run(max_items=args.max)


if __name__ == "__main__":
    main()
