"""Isolated release-gate tests; real EICAR testing is opt-in on CI only."""

import importlib.util
import contextlib
import io
import json
import os
import re
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "scan_release", Path(__file__).resolve().parents[1] / "scripts" / "scan_release.py")
scan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scan)
VERSION = "1.0.0-rc.3"
ROOT = f"SoundFork-v{VERSION}-windows-x64"


class BuildPolicyTests(unittest.TestCase):
    def test_only_codeql_skips_execution_not_compilation(self):
        repository = Path(__file__).resolve().parents[1]
        build = (repository / "scripts/Build-SoundFork.ps1").read_text(encoding="utf-8")
        self.assertRegex(build, r"\[switch\]\$SkipTests\s*\)")
        self.assertNotRegex(build, r"\$SkipTests\s*=\s*\$true")
        self.assertLess(build.index("& $cmake --build --preset"), build.index("if ($SkipTests)"))
        self.assertRegex(build, r"(?s)if \(\$SkipTests\).*?else\s*\{.*?& \$ctest .*?--output-on-failure")
        codeql = (repository / ".github/workflows/codeql.yml").read_text(encoding="utf-8")
        self.assertRegex(codeql, r"run: .*Build-SoundFork.ps1[^\n]*-WithObsPlugin[^\n]*-SkipTests")
        self.assertIn("build-mode: manual", codeql)
        self.assertIn("queries: security-extended", codeql)
        for file in (".github/workflows/ci.yml", ".github/workflows/release.yml",
                     "scripts/Build-ReleasePackage.ps1"):
            self.assertNotIn("-SkipTests", (repository / file).read_text(encoding="utf-8"))

    def test_ci_and_release_preserve_test_failure_logs(self):
        repository = Path(__file__).resolve().parents[1]
        for file in ("ci.yml", "release.yml"):
            workflow = (repository / ".github/workflows" / file).read_text(encoding="utf-8")
            step = re.search(r"(?ms)^      - name: Preserve CTest logs.*?(?=^      - name:|\Z)", workflow)
            self.assertIsNotNone(step)
            self.assertIn("if: always()", step.group())
            self.assertIn("LastTest.log", step.group())
            self.assertIn("LastTestsFailed.log", step.group())


class ReleaseScanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="soundfork-scan-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.reports = self.root / "reports"
        self.setup_name, self.zip_name = scan.release_names(VERSION)
        (self.artifacts / self.setup_name).write_bytes(b"Non-executable setup test fixture")
        self.make_zip()
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": ""}).start()

    def make_zip(self, payload=b"SoundFork clean fixture", name=None, mode=None):
        with zipfile.ZipFile(self.artifacts / self.zip_name, "w") as package:
            info = zipfile.ZipInfo(name or f"{ROOT}/bin/soundfork_driver.dll")
            if mode is not None:
                info.create_system = 3
                info.external_attr = mode << 16
            package.writestr(info, payload)
        self.manifest()

    def manifest(self):
        manifest = self.artifacts / "SHA256SUMS.txt"
        manifest.write_text("".join(f"{scan.sha256(self.artifacts / name)}  {name}\n"
                                    for name in (self.zip_name, self.setup_name)),
                            encoding="ascii", newline="\n")
        self.manifest_hash = scan.sha256(manifest)

    def verify(self):
        return scan.verify_release(self.artifacts, VERSION, self.manifest_hash)

    def extract(self):
        destination = self.root / "extracted"
        destination.mkdir(exist_ok=True)
        return scan.extract_bounded(self.artifacts / self.zip_name, destination, VERSION)

    def scanner(self):
        scan.scan_release(self.artifacts, VERSION, self.reports, self.manifest_hash)

    def fake_clamav(self, command, log, timeout=scan.SCAN_TIMEOUT_SECONDS):
        log.write_text("ClamAV test double\nScanned files: 4\n", encoding="utf-8")

    def test_complete_release_and_crlf_manifest(self):
        manifest = self.artifacts / "SHA256SUMS.txt"
        manifest.write_bytes(manifest.read_bytes().replace(b"\n", b"\r\n"))
        self.manifest_hash = scan.sha256(manifest)
        self.assertEqual(set(self.verify()), {self.zip_name, self.setup_name, "SHA256SUMS.txt"})

    def test_wrong_manifest_digest_is_rejected(self):
        self.manifest_hash = "0" * 64
        with self.assertRaisesRegex(scan.ScanError, "original build"):
            self.verify()

    def test_tampered_file_is_rejected(self):
        (self.artifacts / self.setup_name).write_bytes(b"changed")
        with self.assertRaisesRegex(scan.ScanError, "Checksum mismatch"):
            self.verify()

    def test_replacing_files_and_manifest_does_not_bypass_binding(self):
        original = self.manifest_hash
        (self.artifacts / self.setup_name).write_bytes(b"changed")
        self.manifest()
        self.manifest_hash = original
        with self.assertRaisesRegex(scan.ScanError, "original build"):
            self.verify()

    def test_missing_and_extra_files_are_rejected(self):
        extra = self.artifacts / "extra.dll"
        extra.write_bytes(b"unexpected")
        with self.assertRaises(scan.ScanError):
            self.verify()
        extra.unlink()
        (self.artifacts / self.setup_name).unlink()
        with self.assertRaises(scan.ScanError):
            self.verify()

    def test_empty_and_oversized_files_are_rejected(self):
        with patch.object(scan, "MAX_FILE_BYTES", 5):
            with self.assertRaisesRegex(scan.ScanError, "oversized"):
                self.verify()
        (self.artifacts / self.setup_name).write_bytes(b"")
        self.manifest()
        with self.assertRaisesRegex(scan.ScanError, "Empty"):
            self.verify()

    def test_manifest_must_have_exact_entries(self):
        manifest = self.artifacts / "SHA256SUMS.txt"
        original = manifest.read_text(encoding="ascii")
        cases = [original + original, original.splitlines()[0] + "\n",
                 "0" * 64 + "  ../other.exe\n", original + "\n", "not a checksum\n"]
        for content in cases:
            with self.subTest(content=content):
                manifest.write_text(content, encoding="ascii")
                self.manifest_hash = scan.sha256(manifest)
                with self.assertRaises(scan.ScanError):
                    self.verify()

    def test_invalid_versions_are_rejected(self):
        for version in ("v0.1.0", "../../bad", "0.1.0-dev", "0.1.0\n", "0.1.0-beta.x"):
            with self.subTest(version=version), self.assertRaises(scan.ScanError):
                scan.release_names(version)

    def test_normal_zip_extraction(self):
        self.assertEqual(self.extract()["extracted_files"], 1)
        path = self.root / "extracted" / ROOT / "bin" / "soundfork_driver.dll"
        self.assertEqual(path.read_bytes(), b"SoundFork clean fixture")

    def test_windows_zip_separators(self):
        self.make_zip(name=f"{ROOT}\\bin\\soundfork_driver.dll")
        self.assertEqual(self.extract()["extracted_files"], 1)

    def test_compress_archive_windows_directory_entries(self):
        with zipfile.ZipFile(self.artifacts / self.zip_name, "w") as package:
            for name, payload in ((f"{ROOT}\\bin\\", b""),
                                  (f"{ROOT}\\bin\\soundfork_driver.dll", b"fixture")):
                info = zipfile.ZipInfo(name)
                info.create_system = 0
                info.external_attr = 0
                package.writestr(info, payload)
        self.assertEqual(self.extract()["extracted_files"], 1)

    def test_empty_payload_files_are_counted_separately(self):
        self.make_zip(payload=b"")
        result = self.extract()
        self.assertEqual(result["extracted_files"], 1)
        self.assertEqual(result["extracted_nonempty_files"], 0)

    def test_unsafe_zip_paths(self):
        for name in ("../escape.dll", "/absolute.dll", "C:/escape.dll",
                     f"{ROOT}/../escape.dll", f"{ROOT}/bin/./x",
                     f"{ROOT}/bin/file:stream", f"{ROOT}/bin/file. ",
                     f"{ROOT}/bin//x", f"{ROOT}/bin/line\nbreak"):
            with self.subTest(name=name):
                self.make_zip(name=name)
                with self.assertRaisesRegex(scan.ScanError, "Unsafe"):
                    self.extract()

    def test_zip_links_and_special_files(self):
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFIFO | 0o600):
            with self.subTest(mode=mode):
                self.make_zip(mode=mode)
                with self.assertRaisesRegex(scan.ScanError, "special files"):
                    self.extract()

    def test_zip_size_and_entry_limits(self):
        with patch.object(scan, "MAX_FILE_BYTES", 2):
            with self.assertRaisesRegex(scan.ScanError, "file size"):
                self.extract()
        with patch.object(scan, "MAX_TOTAL_BYTES", 2):
            with self.assertRaisesRegex(scan.ScanError, "expanded size"):
                self.extract()
        with patch.object(scan, "MAX_FILES", 0):
            with self.assertRaisesRegex(scan.ScanError, "entry limit"):
                self.extract()
        with patch.object(scan, "MAX_DEPTH", 1):
            with self.assertRaisesRegex(scan.ScanError, "Unsafe"):
                self.extract()

    def test_empty_zip_rejected(self):
        with zipfile.ZipFile(self.artifacts / self.zip_name, "w"):
            pass
        with self.assertRaises(scan.ScanError):
            self.extract()

    def test_duplicate_case_insensitive_zip_path(self):
        with zipfile.ZipFile(self.artifacts / self.zip_name, "w") as package:
            package.writestr(f"{ROOT}/file.dll", b"one")
            package.writestr(f"{ROOT}/FILE.dll", b"two")
        with self.assertRaisesRegex(scan.ScanError, "Duplicate"):
            self.extract()

    def test_encrypted_zip_flag_rejected(self):
        archive = self.artifacts / self.zip_name
        contents = bytearray(archive.read_bytes())
        central = contents.index(b"PK\x01\x02")
        contents[central + 8] |= 1
        archive.write_bytes(contents)
        with self.assertRaisesRegex(scan.ScanError, "Encrypted"):
            self.extract()

    def test_corrupt_zip_rejected(self):
        (self.artifacts / self.zip_name).write_bytes(b"not a zip")
        with self.assertRaises(zipfile.BadZipFile):
            self.extract()

    def test_success_records_hashes_and_enforces_scanner_flags(self):
        with patch.object(scan.shutil, "which", return_value="clamscan"), \
                patch.object(scan, "run_clamav", side_effect=self.fake_clamav) as runner:
            self.scanner()
        result = json.loads((self.reports / "scan-result.json").read_text())
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["sha256"]["SHA256SUMS.txt"], self.manifest_hash)
        command = runner.call_args_list[1].args[0]
        self.assertIn("--alert-exceeds-max=yes", command)
        self.assertIn("--alert-encrypted=yes", command)
        self.assertIn("--fail-if-cvd-older-than=3", command)
        self.assertIn("--nocerts", command)
        self.assertIn("--max-dir-recursion=32", command)
        self.assertIn(str(self.artifacts.resolve()), command)

    def test_silent_or_incomplete_scan_is_not_success(self):
        for text in ("", "Scanned files: 0\n", "Scanned files: 3\n"):
            def incomplete(command, log, timeout=scan.SCAN_TIMEOUT_SECONDS):
                log.write_text(text, encoding="utf-8")
            with self.subTest(text=text), \
                    patch.object(scan.shutil, "which", return_value="clamscan"), \
                    patch.object(scan, "run_clamav", side_effect=incomplete):
                with self.assertRaisesRegex(scan.ScanError, "all release inputs"):
                    self.scanner()

    def test_missing_scanner_is_failure(self):
        with patch.object(scan.shutil, "which", return_value=None):
            with self.assertRaisesRegex(scan.ScanError, "not found"):
                self.scanner()
        self.assertEqual(json.loads((self.reports / "scan-result.json").read_text())["status"], "failed")

    def test_detection_scanner_error_and_timeout_fail_closed(self):
        for error in (scan.ScanError("detection"), scan.ScanError("scanner error"),
                      subprocess.TimeoutExpired("clamscan", 1)):
            with self.subTest(error=error), \
                    patch.object(scan.shutil, "which", return_value="clamscan"), \
                    patch.object(scan, "run_clamav", side_effect=error):
                with self.assertRaises(type(error)):
                    self.scanner()
                result = json.loads((self.reports / "scan-result.json").read_text())
                self.assertEqual(result["status"], "failed")

    def test_run_clamav_rejects_both_nonzero_exit_codes(self):
        for code in (1, 2):
            with self.subTest(code=code), self.assertRaises(scan.ScanError):
                scan.run_clamav([sys.executable, "-c", f"raise SystemExit({code})"], self.root / "exit.log")

    def test_run_clamav_timeout(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            scan.run_clamav([sys.executable, "-c", "import time; time.sleep(2)"],
                            self.root / "timeout.log", timeout=0.01)

    def test_zero_exit_with_warning_or_error_is_failure(self):
        for message in ("WARNING: unreadable file", "LibClamAV Warning: engine issue",
                        "LibClamAV Error: database issue", "file.dll: read error ERROR"):
            with self.subTest(message=message), self.assertRaisesRegex(scan.ScanError, "warning/error"):
                scan.run_clamav([sys.executable, "-c", f"print({message!r})"], self.root / "warning.log")

    def test_changed_files_during_scan_are_rejected(self):
        def tamper(command, log, timeout=scan.SCAN_TIMEOUT_SECONDS):
            self.fake_clamav(command, log, timeout)
            if "--version" not in command:
                (self.artifacts / self.setup_name).write_bytes(b"changed during scan")
        with patch.object(scan.shutil, "which", return_value="clamscan"), \
                patch.object(scan, "run_clamav", side_effect=tamper):
            with self.assertRaisesRegex(scan.ScanError, "Checksum mismatch"):
                self.scanner()

    def test_reports_cannot_modify_artifact_directory(self):
        with self.assertRaisesRegex(scan.ScanError, "outside"):
            scan.scan_release(self.artifacts, VERSION, self.artifacts / "reports")

    def test_verify_only_cli(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(scan.main([
                "--artifacts", str(self.artifacts), "--version", VERSION,
                "--manifest-sha256", self.manifest_hash, "--verify-only"]), 0)
        self.assertFalse(self.reports.exists())

    def test_cli_returns_failure_for_tampering(self):
        (self.artifacts / self.setup_name).write_bytes(b"changed")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(scan.main([
                "--artifacts", str(self.artifacts), "--version", VERSION,
                "--manifest-sha256", self.manifest_hash, "--verify-only"]), 1)

    @unittest.skipUnless(os.environ.get("SOUNDFORK_TEST_CLAMAV") == "1", "real ClamAV CI test is opt-in")
    def test_real_clamav_clean_release_and_eicar_in_zip(self):
        self.scanner()
        self.assertEqual(json.loads((self.reports / "scan-result.json").read_text())["status"], "passed")
        # Construct the standard harmless AV test sample only in an isolated CI
        # temporary directory. Never commit the sample or put it in release assets.
        sample = (b"X5O!P%@AP[4\\PZX54(P^)7CC)7}" + b"$EICAR-STANDARD-"
                  + b"ANTIVIRUS-TEST-FILE!$H+H*")
        self.make_zip(payload=sample)
        with self.assertRaisesRegex(scan.ScanError, "detection"):
            self.scanner()
        self.assertEqual(json.loads((self.reports / "scan-result.json").read_text())["status"], "failed")


if __name__ == "__main__":
    unittest.main()
