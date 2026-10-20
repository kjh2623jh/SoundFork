#!/usr/bin/env python3
"""Fail-closed release validation/ClamAV scan; Python 3.10+, standard library only.

Never executes release binaries. The workflow refreshes ClamAV signatures before
calling this script. --verify-only is also used by the validation
step of the publish job to bind publication to the build's checksum manifest.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile


MAX_FILE_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024
MAX_FILES = 4096
MAX_DEPTH = 32
SCAN_TIMEOUT_SECONDS = 900
VERSION_PATTERN = r"[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc)\.[0-9]+)?"


class ScanError(RuntimeError):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def release_names(version):
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ScanError("Unsupported release version")
    return (f"SoundFork-Setup-v{version}.exe", f"SoundFork-v{version}-windows-x64.zip")


def verify_release(directory, version, manifest_sha256=None):
    names = release_names(version)
    expected = set(names) | {"SHA256SUMS.txt"}
    if directory.is_symlink() or not directory.is_dir():
        raise ScanError("Artifact directory must be a real directory")
    if {path.name for path in directory.iterdir()} != expected:
        raise ScanError("Artifact must contain exactly Setup.exe, ZIP and SHA256SUMS.txt")
    for name in expected:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ScanError(f"Non-regular release file: {name}")
        if not 0 < path.stat().st_size <= MAX_FILE_BYTES:
            raise ScanError(f"Empty or oversized release file: {name}")

    manifest = directory / "SHA256SUMS.txt"
    if manifest.stat().st_size > 4096:
        raise ScanError("Oversized checksum manifest")
    manifest_digest = sha256(manifest)
    if manifest_sha256 is not None:
        if not re.fullmatch(r"[0-9a-f]{64}", manifest_sha256):
            raise ScanError("Invalid expected manifest SHA-256")
        if manifest_digest != manifest_sha256:
            raise ScanError("Checksum manifest differs from the original build")
    entries = {}
    for line in manifest.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+)", line)
        if not match or match[2] not in names or match[2] in entries:
            raise ScanError("Invalid, duplicate or unexpected checksum entry")
        entries[match[2]] = match[1]
    if set(entries) != set(names):
        raise ScanError("Checksum manifest must cover both release files")
    for name, expected_digest in entries.items():
        if sha256(directory / name) != expected_digest:
            raise ScanError(f"Checksum mismatch: {name}")
    return {"SHA256SUMS.txt": manifest_digest, **entries}


def extract_bounded(archive, destination, version):
    """Extract without trusting ZIP paths, file types or decompressed sizes."""
    root_name = f"SoundFork-v{version}-windows-x64"
    files = 0
    nonempty_files = 0
    total = 0
    seen = set()
    with zipfile.ZipFile(archive) as package:
        members = package.infolist()
        if not members or len(members) > MAX_FILES:
            raise ScanError("Empty ZIP or ZIP entry limit exceeded")
        for member in members:
            # PowerShell Compress-Archive produces Windows paths on some versions.
            raw = member.filename.replace("\\", "/")
            parts = raw.rstrip("/").split("/")
            if (not parts or parts[0] != root_name or len(parts) > MAX_DEPTH
                    or any(part in ("", ".", "..") or ":" in part
                           or part.endswith((".", " "))
                           or any(ord(char) < 32 for char in part) for part in parts)):
                raise ScanError("Unsafe ZIP path")
            key = "/".join(parts).casefold()
            if key in seen:
                raise ScanError("Duplicate ZIP path")
            seen.add(key)
            mode = (member.external_attr >> 16) & 0xFFFF
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise ScanError("ZIP links and special files are forbidden")
            if member.flag_bits & 1:
                raise ScanError("Encrypted ZIP entry")
            # is_dir() alone treats trailing backslashes differently across OSes.
            # Compress-Archive's directory entries must work on the Linux runner.
            if raw.endswith("/"):
                if member.file_size != 0:
                    raise ScanError("ZIP directory has unexpected contents")
                continue
            if stat.S_IFMT(mode) == stat.S_IFDIR:
                raise ScanError("ZIP directory is missing its path separator")
            files += 1
            if not 0 <= member.file_size <= MAX_FILE_BYTES:
                raise ScanError("ZIP file size limit exceeded")
            if member.file_size:
                nonempty_files += 1
            total += member.file_size
            if total > MAX_TOTAL_BYTES:
                raise ScanError("ZIP expanded size limit exceeded")
            target = destination.joinpath(*PurePosixPath(raw).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            written = 0
            with package.open(member) as source, target.open("xb") as output:
                while block := source.read(1024 * 1024):
                    written += len(block)
                    if written > member.file_size or written > MAX_FILE_BYTES:
                        raise ScanError("ZIP expanded size differs from its header")
                    output.write(block)
            if written != member.file_size:
                raise ScanError("Truncated ZIP entry")
    if files == 0:
        raise ScanError("ZIP contains no files")
    return {"extracted_files": files, "extracted_nonempty_files": nonempty_files,
            "extracted_bytes": total}


def run_clamav(command, log_path, timeout=SCAN_TIMEOUT_SECONDS):
    # A regular log file also preserves partial output when the process times out.
    with log_path.open("w", encoding="utf-8") as output:
        completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                   check=False, timeout=timeout)
    if completed.returncode != 0:
        label = "detection or scan limit" if completed.returncode == 1 else "scanner error"
        raise ScanError(f"ClamAV {label} (exit {completed.returncode}); see {log_path.name}")
    # Some partially successful scans can leave warnings despite exit code zero.
    # Do not silently treat an unreadable file or engine warning as a clean scan.
    log = log_path.read_text(encoding="utf-8", errors="replace")
    if re.search(r"^(?:LibClamAV )?(?:WARNING|ERROR):| ERROR\r?$", log,
                 flags=re.IGNORECASE | re.MULTILINE):
        raise ScanError(f"ClamAV warning/error in output; see {log_path.name}")


def scan_release(directory, version, report_directory, manifest_sha256=None):
    # Reports must not enter the scanned input or a published archive.
    if (report_directory.resolve() == directory.resolve()
            or directory.resolve() in report_directory.resolve().parents):
        raise ScanError("Reports must be outside the artifact directory")
    report_directory.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "version": version, "limits": {
        "file_bytes": MAX_FILE_BYTES, "expanded_bytes": MAX_TOTAL_BYTES,
        "entries": MAX_FILES, "recursion": MAX_DEPTH,
        "process_timeout_seconds": SCAN_TIMEOUT_SECONDS,
        "maximum_database_age_days": 3}}
    try:
        report["sha256"] = verify_release(directory, version, manifest_sha256)
        scanner = shutil.which("clamscan")
        if scanner is None:
            raise ScanError("clamscan was not found")
        version_log = report_directory / "clamav-version.log"
        run_clamav([scanner, "--version"], version_log, timeout=30)
        report["engine_and_database"] = version_log.read_text(encoding="utf-8").strip()
        with tempfile.TemporaryDirectory(prefix="soundfork-scan-") as scratch:
            extracted = Path(scratch) / "payload"
            extracted.mkdir()
            report.update(extract_bounded(directory / release_names(version)[1], extracted, version))
            run_clamav([
                scanner, "--recursive=yes", "--scan-archive=yes", "--scan-pe=yes",
                "--nocerts", f"--max-dir-recursion={MAX_DEPTH}",
                "--alert-exceeds-max=yes", "--alert-encrypted=yes",
                "--fail-if-cvd-older-than=3", "--max-filesize=128M",
                "--max-scansize=512M", f"--max-files={MAX_FILES}",
                f"--max-recursion={MAX_DEPTH}", "--max-scantime=120000",
                str(directory.resolve()), str(extracted.resolve()),
            ], report_directory / "clamscan.log")
            scan_log = (report_directory / "clamscan.log").read_text(encoding="utf-8", errors="replace")
            count = re.search(r"^Scanned files:\s+(\d+)\s*$", scan_log, re.MULTILINE)
            # ClamAV reports empty files separately, not as scanned files.
            if count is None or int(count[1]) < report["extracted_nonempty_files"] + 3:
                raise ScanError("ClamAV did not confirm scanning all release inputs")
            report["scanned_files"] = int(count[1])
        # Inputs are read-only; detect any accidental change during inspection.
        if verify_release(directory, version, manifest_sha256) != report["sha256"]:
            raise ScanError("Release files changed during the scan")
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        (report_directory / "scan-result.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")
        hashes = "\n".join(f"- `{name}`: `{digest}`"
                           for name, digest in report.get("sha256", {}).items())
        summary = (f"## Release checks · ClamAV: {report['status']}\n\n"
                   f"Version: `{version}`\n\n"
                   f"Engine / database: `{report.get('engine_and_database', 'unavailable')}`\n\n"
                   f"{hashes}\n\nSee the security-log artifact for details. "
                   "A successful scan is not a security guarantee.\n")
        (report_directory / "summary.md").write_text(summary, encoding="utf-8")
        if step_summary := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(step_summary, "a", encoding="utf-8") as output:
                output.write(summary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.verify_only:
            hashes = verify_release(args.artifacts, args.version, args.manifest_sha256)
            print(json.dumps(hashes, indent=2))
        else:
            if args.reports is None:
                parser.error("--reports is required for a scan")
            scan_release(args.artifacts, args.version, args.reports, args.manifest_sha256)
    except (ScanError, OSError, ValueError, zipfile.BadZipFile,
            RuntimeError, subprocess.SubprocessError) as error:
        print(f"Release verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
