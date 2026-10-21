#!/usr/bin/env python3
"""
VirusTotal Asset Scanner & GitHub Release Updater for VKey.

Scans release assets (archives, executables, dynamic libraries) with VirusTotal API v3,
formats a clean Markdown report table, and updates the GitHub Release description directly.

No external Python dependencies (pure standard library).
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile
from typing import Any, Dict, List, Optional, Tuple

VT_API_BASE = "https://www.virustotal.com/api/v3"


def compute_sha256(filepath: str) -> str:
    """Compute lowercase SHA-256 hex digest of a file in chunks."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def create_multipart_body(filepath: str, field_name: str = "file") -> Tuple[bytes, str]:
    """Build multipart/form-data payload without external libraries."""
    boundary = f"----WebKitFormBoundary{uuid.uuid4().hex}"
    filename = os.path.basename(filepath)
    with open(filepath, "rb") as f:
        file_bytes = f.read()

    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode("utf-8") + file_bytes + f"\r\n--{boundary}--\r\n".encode("utf-8")

    content_type = f"multipart/form-data; boundary={boundary}"
    return body, content_type


class VirusTotalClient:
    def __init__(self, api_key: str, delay_between_calls: float = 15.5):
        self.api_key = api_key
        self.delay = delay_between_calls
        self.last_call_timestamp: float = 0.0

    def _wait_rate_limit(self) -> None:
        """Enforce spacing between API calls to stay within free-tier rate limits (4 req/min)."""
        now = time.time()
        elapsed = now - self.last_call_timestamp
        if elapsed < self.delay:
            wait_time = self.delay - elapsed
            print(f"  [RateLimit] Waiting {wait_time:.1f}s before next VirusTotal API request...")
            time.sleep(wait_time)
        self.last_call_timestamp = time.time()

    def request(
        self,
        endpoint: str,
        method: str = "GET",
        data: Optional[bytes] = None,
        content_type: str = "application/json",
        max_retries: int = 5,
    ) -> Optional[Dict[str, Any]]:
        """Make an authenticated API call with retries on rate limits (429) or transient server errors."""
        url = endpoint if endpoint.startswith("http") else f"{VT_API_BASE}/{endpoint.lstrip('/')}"
        headers = {
            "x-apikey": self.api_key,
            "Accept": "application/json",
            "User-Agent": "VKey-VirusTotal-CI/1.0",
        }
        if data is not None:
            headers["Content-Type"] = content_type

        for attempt in range(1, max_retries + 1):
            self._wait_rate_limit()
            req = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=120) as resp:
                    resp_data = resp.read()
                    if not resp_data:
                        return None
                    return json.loads(resp_data.decode("utf-8"))
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    # Item does not exist
                    return None
                if e.code == 429:
                    retry_wait = 30.0 * attempt
                    print(f"  [HTTP 429] Rate limit hit. Backing off for {retry_wait:.0f}s (attempt {attempt}/{max_retries})...")
                    time.sleep(retry_wait)
                    continue
                if e.code in (500, 502, 503, 504):
                    retry_wait = 15.0 * attempt
                    print(f"  [HTTP {e.code}] Server error. Retrying in {retry_wait:.0f}s (attempt {attempt}/{max_retries})...")
                    time.sleep(retry_wait)
                    continue
                # Other HTTP errors
                error_msg = e.read().decode("utf-8", errors="replace")
                print(f"  [HTTP Error {e.code}] {error_msg}", file=sys.stderr)
                raise
            except (urllib.error.URLError, TimeoutError) as e:
                retry_wait = 15.0 * attempt
                print(f"  [Network Error] {e}. Retrying in {retry_wait:.0f}s (attempt {attempt}/{max_retries})...")
                time.sleep(retry_wait)
                continue

        raise RuntimeError(f"VirusTotal API request failed after {max_retries} retries: {url}")

    def get_file_report(self, sha256: str) -> Optional[Dict[str, Any]]:
        """Fetch report if file hash is already known to VirusTotal."""
        return self.request(f"files/{sha256}")

    def upload_file(self, filepath: str) -> str:
        """Upload file to VirusTotal and return analysis ID."""
        size = os.path.getsize(filepath)
        upload_url = "files"
        if size > 32 * 1024 * 1024:
            # Files larger than 32MB must request a special upload URL first
            meta = self.request("files/upload_url")
            if not meta or "data" not in meta:
                raise RuntimeError("Failed to obtain large-file upload URL from VirusTotal")
            upload_url = meta["data"]

        body, content_type = create_multipart_body(filepath)
        resp = self.request(upload_url, method="POST", data=body, content_type=content_type)
        if not resp or "data" not in resp or "id" not in resp["data"]:
            raise RuntimeError(f"Unexpected response when uploading {filepath}: {resp}")
        return resp["data"]["id"]

    def poll_analysis(self, analysis_id: str, timeout_sec: int = 600) -> Dict[str, Any]:
        """Poll analysis ID until status is 'completed'."""
        start = time.time()
        while time.time() - start < timeout_sec:
            res = self.request(f"analyses/{analysis_id}")
            if not res or "data" not in res:
                time.sleep(self.delay)
                continue

            attr = res["data"].get("attributes", {})
            status = attr.get("status")
            if status == "completed":
                return res

            print(f"  Analysis status: '{status}'... waiting {self.delay:.1f}s")
            time.sleep(self.delay)

        raise TimeoutError(f"VirusTotal analysis {analysis_id} timed out after {timeout_sec}s")


class ScanResult:
    def __init__(
        self,
        display_name: str,
        sha256: str,
        malicious: int,
        suspicious: int,
        scan_time_utc: str,
    ):
        self.display_name = display_name
        self.sha256 = sha256
        self.malicious = malicious
        self.suspicious = suspicious
        self.scan_time_utc = scan_time_utc

    @property
    def vt_url(self) -> str:
        return f"https://www.virustotal.com/gui/file/{self.sha256}/detection"


def scan_single_file(client: VirusTotalClient, filepath: str, display_name: str) -> ScanResult:
    """Scan or retrieve existing analysis for a single file."""
    sha256 = compute_sha256(filepath)
    print(f"\n[*] Processing: {display_name} (SHA-256: {sha256})")

    # Step 1: Check if already analyzed on VirusTotal
    report = client.get_file_report(sha256)
    if report and "data" in report:
        attr = report["data"].get("attributes", {})
        stats = attr.get("last_analysis_stats", {})
        last_date = attr.get("last_analysis_date")
        if last_date:
            scan_time = datetime.datetime.fromtimestamp(last_date, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        else:
            scan_time = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

        malicious = stats.get("malicious", 0)
        suspicious = stats.get("suspicious", 0)
        print(f"  -> Existing report found: {malicious} malicious, {suspicious} suspicious ({scan_time} UTC)")
        return ScanResult(display_name, sha256, malicious, suspicious, scan_time)

    # Step 2: Upload file if report does not exist
    print("  -> File not found on VirusTotal. Uploading...")
    analysis_id = client.upload_file(filepath)
    print(f"  -> Uploaded successfully (Analysis ID: {analysis_id}). Waiting for analysis...")

    # Step 3: Poll analysis completion
    analysis = client.poll_analysis(analysis_id)
    attr = analysis["data"].get("attributes", {})
    stats = attr.get("stats", {})
    date_val = attr.get("date")
    if date_val:
        scan_time = datetime.datetime.fromtimestamp(date_val, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    else:
        scan_time = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

    malicious = stats.get("malicious", 0)
    suspicious = stats.get("suspicious", 0)
    print(f"  -> Scan completed: {malicious} malicious, {suspicious} suspicious ({scan_time} UTC)")
    return ScanResult(display_name, sha256, malicious, suspicious, scan_time)


def build_markdown_table(results: List[ScanResult], tag: str) -> str:
    """Format scan results into markdown table matching VKey's RELEASE_NOTES format."""
    lines = [
        f"### Kết quả VirusTotal ({tag})",
        "",
        "| Tệp / báo cáo VirusTotal | Engine báo độc hại | Đáng ngờ | Quét lúc (UTC) |",
        "|---|---|---|---|",
    ]
    for r in results:
        lines.append(f"| [{r.display_name}]({r.vt_url}) | {r.malicious} | {r.suspicious} | {r.scan_time_utc} |")

    lines.extend([
        "",
        "VKey sử dụng hook bàn phím và gửi phím tổng hợp để xử lý tiếng Việt; các hành vi này có thể bị bộ quét nhận nhầm. "
        "Đây là khả năng giải thích cảnh báo, chưa phải kết luận từ các hãng antivirus. "
        "Xem [hướng dẫn kiểm tra bản tải](https://github.com/phatMT97/VKey/blob/Main/docs/ANTIVIRUS.md).",
    ])
    return "\n".join(lines)


def update_github_release_notes(tag: str, repo: Optional[str], vt_markdown_block: str) -> bool:
    """Fetch release notes via gh CLI, replace/append VirusTotal section, and update release."""
    gh_cmd = ["gh", "release", "view", tag, "--json", "body", "--jq", ".body"]
    if repo:
        gh_cmd.extend(["--repo", repo])

    print(f"\n[*] Fetching release notes for tag '{tag}' via GitHub CLI...")
    try:
        current_body = subprocess.check_output(gh_cmd, text=True, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        print(f"Error fetching release notes: {e.stderr}", file=sys.stderr)
        return False

    # Regex to find existing VirusTotal section
    # Matches from "### [Kết quả ]VirusTotal..." up to next "### ", "---", or end of string
    vt_pattern = re.compile(
        r"(### (?:Kết quả )?VirusTotal[^\n]*\n)([\s\S]*?)(?=(\n### |\n---|(?:\Z)))",
        re.MULTILINE,
    )

    if vt_pattern.search(current_body):
        print("  -> Existing VirusTotal section found in release notes. Replacing...")
        updated_body = vt_pattern.sub(vt_markdown_block + "\n", current_body)
    else:
        print("  -> VirusTotal section not found in release notes. Appending before trailer...")
        trailer_match = re.search(r"(\n---\s*\n\s*\[Xem các thay đổi)", current_body)
        if trailer_match:
            idx = trailer_match.start()
            updated_body = current_body[:idx] + "\n\n" + vt_markdown_block + "\n" + current_body[idx:]
        else:
            updated_body = current_body.rstrip() + "\n\n" + vt_markdown_block + "\n"

    # Write to a temporary file to avoid command-line quoting issues
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False, suffix=".md") as tmp:
        tmp.write(updated_body)
        tmp_path = tmp.name

    try:
        edit_cmd = ["gh", "release", "edit", tag, "--notes-file", tmp_path]
        if repo:
            edit_cmd.extend(["--repo", repo])
        print(f"[*] Updating GitHub release '{tag}'...")
        subprocess.check_call(edit_cmd)
        print("[+] GitHub release updated successfully!")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error editing GitHub release: {e}", file=sys.stderr)
        return False
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def resolve_files_to_scan(
    files: Optional[List[str]],
    dir_path: Optional[str],
    temp_dir: str,
) -> List[Tuple[str, str]]:
    """
    Resolve file paths and display names.
    Automatically extracts VKey.exe / VKeyClassic.exe from zip archives if missing.
    Order matches canonical VKey release notes:
      1. VKey.zip
      2. VKey.exe
      3. VKeyClassic.zip
      4. VKeyClassic.exe
      5. vkey_engine.dll
    """
    candidates: Dict[str, str] = {}

    def add_candidate(path: str) -> None:
        if os.path.isfile(path):
            candidates[os.path.basename(path)] = os.path.abspath(path)

    if files:
        for f in files:
            add_candidate(f)

    if dir_path and os.path.isdir(dir_path):
        for entry in os.listdir(dir_path):
            add_candidate(os.path.join(dir_path, entry))

    # Auto-extract executables from archives if needed
    sciter_zip = candidates.get("VKey-x64.zip") or candidates.get("VKey.zip")
    if "VKey.exe" not in candidates and sciter_zip:
        try:
            with zipfile.ZipFile(sciter_zip, "r") as z:
                if "VKey.exe" in z.namelist():
                    extracted_path = z.extract("VKey.exe", temp_dir)
                    candidates["VKey.exe"] = extracted_path
                    print(f"[*] Auto-extracted VKey.exe from {sciter_zip}")
        except Exception as e:
            print(f"Warning: Failed to extract VKey.exe: {e}")

    classic_zip = candidates.get("VKeyClassic-x64.zip") or candidates.get("VKeyClassic.zip")
    if "VKeyClassic.exe" not in candidates and classic_zip:
        try:
            with zipfile.ZipFile(classic_zip, "r") as z:
                if "VKeyClassic.exe" in z.namelist():
                    extracted_path = z.extract("VKeyClassic.exe", temp_dir)
                    candidates["VKeyClassic.exe"] = extracted_path
                    print(f"[*] Auto-extracted VKeyClassic.exe from {classic_zip}")
        except Exception as e:
            print(f"Warning: Failed to extract VKeyClassic.exe: {e}")

    # Canonical order
    canonical_order = [
        "VKey-x64.zip",
        "VKey.zip",
        "VKey.exe",
        "VKeyClassic-x64.zip",
        "VKeyClassic.zip",
        "VKeyClassic.exe",
        "vkey_engine.dll",
    ]

    result: List[Tuple[str, str]] = []
    for name in canonical_order:
        if name in candidates:
            result.append((candidates[name], name))

    # Any other matching passed files
    for name, path in candidates.items():
        if name not in canonical_order and not name.endswith((".sha256", ".sig", ".txt")):
            result.append((path, name))

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Scan VKey release assets with VirusTotal & update GitHub Release.")
    parser.add_argument("--api-key", default=os.environ.get("VIRUSTOTAL_API_KEY"), help="VirusTotal API Key")
    parser.add_argument("--tag", default=os.environ.get("GITHUB_REF_NAME"), help="Release tag (e.g. v4.3.2)")
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"), help="GitHub repo (e.g. phatMT97/VKey)")
    parser.add_argument("--dir", help="Directory containing release assets")
    parser.add_argument("--files", nargs="*", help="Specific files to scan")
    parser.add_argument("--delay", type=float, default=15.5, help="Delay in seconds between API calls (default 15.5s)")
    parser.add_argument("--no-update-release", action="store_true", help="Do not update GitHub Release body")
    parser.add_argument("--dry-run", action="store_true", help="Perform SHA-256 calculation and preview without calling VT API")
    args = parser.parse_args()

    if not args.tag:
        print("Error: --tag or GITHUB_REF_NAME must be specified", file=sys.stderr)
        return 1

    if not args.dry_run and not args.api_key:
        print("Warning: VIRUSTOTAL_API_KEY is not set. Skipping VirusTotal scan step.")
        return 0

    with tempfile.TemporaryDirectory() as temp_dir:
        files_to_scan = resolve_files_to_scan(args.files, args.dir, temp_dir)
        if not files_to_scan:
            print("Error: No valid release files found to scan.", file=sys.stderr)
            return 1

        print(f"=== VirusTotal Scan for Release {args.tag} ===")
        print("Files queued for scanning:")
        for path, name in files_to_scan:
            print(f"  • {name} ({os.path.getsize(path):,} bytes)")

        scan_results: List[ScanResult] = []

        if args.dry_run:
            print("\n[DRY RUN] Calculating SHA-256 digests:")
            now_str = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
            for path, name in files_to_scan:
                digest = compute_sha256(path)
                print(f"  {name}: {digest}")
                scan_results.append(ScanResult(name, digest, 0, 0, now_str))
        else:
            client = VirusTotalClient(args.api_key, delay_between_calls=args.delay)
            for path, name in files_to_scan:
                res = scan_single_file(client, path, name)
                scan_results.append(res)

        table_md = build_markdown_table(scan_results, args.tag)
        print("\n=== Formatted Markdown Table ===")
        print(table_md)

        # Output to GitHub Actions Step Summary if available
        step_summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if step_summary_path:
            with open(step_summary_path, "a", encoding="utf-8") as f:
                f.write("\n" + table_md + "\n")
            print(f"[*] Appended table to $GITHUB_STEP_SUMMARY ({step_summary_path})")

        # Update GitHub Release if requested
        if not args.no_update_release and not args.dry_run:
            success = update_github_release_notes(args.tag, args.repo, table_md)
            if not success:
                print("Warning: Failed to update GitHub Release notes, but scan finished.", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
