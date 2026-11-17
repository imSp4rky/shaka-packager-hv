import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "scripts"))
import build

spec = importlib.util.spec_from_file_location(
    "source_version", PROJECT / "packager/version/generate_version_string.py")
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root)] + list(args),
                                   stderr=subprocess.STDOUT, text=True).strip()


def origin(root):
    root.mkdir()
    git(root, "init", "--quiet")
    (root / "pin.txt").write_text("pinned source\n")
    git(root, "add", "pin.txt")
    git(root, "-c", "user.name=Build Test", "-c", "user.email=test@example.invalid",
        "commit", "--quiet", "-m", "Fixture")
    return git(root, "rev-parse", "HEAD")


class DependencyTests(unittest.TestCase):
    def test_real_pins_and_platform_dependencies(self):
        linux = {d.path: d for d in build.dependencies(PROJECT, "linux")}
        windows = {d.path: d for d in build.dependencies(PROJECT, "win32")}
        self.assertEqual(20, len(linux))
        self.assertEqual("a34eabec0d807cf03dc8cfc1a6240156ac2bbd01",
                         linux["packager/base"].revision)
        self.assertIn("packager/third_party/gold", linux)
        self.assertNotIn("packager/third_party/gold", windows)
        self.assertIn("packager/third_party/yasm/source/patched-yasm", windows)

    def test_deps_hooks_are_not_executed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "DEPS").write_text(
                "vars={'host':'https://example.invalid'}\n"
                "deps={'src/packager/base':Var('host')+'/repo@" + "a" * 40 + "'}\n"
                "deps_os={}\n"
                "hooks=[__import__('pathlib').Path('hook-ran').touch()]\n")
            self.assertEqual(1, len(build.dependencies(root, "linux")))
            self.assertFalse((root / "hook-ran").exists())

    def test_unpinned_revisions_and_traversal_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for location, revision in [("src/a", "main"),
                                       ("src/../../escape", "a" * 40)]:
                (root / "DEPS").write_text(
                    "vars={}\ndeps={" + repr(location) + ":" +
                    repr("https://example.invalid/repo@" + revision) + "}\ndeps_os={}\n")
                with self.assertRaises(ValueError):
                    build.dependencies(root, "linux")

    def test_existing_non_git_files_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "packager/base"
            target.mkdir(parents=True)
            (target / "important.txt").write_text("keep me")
            dependency = build.Dependency("packager/base", "unused", "a" * 40)
            with self.assertRaisesRegex(ValueError, "Refusing"):
                build.sync_dependency(root, dependency)
            self.assertEqual("keep me", (target / "important.txt").read_text())

    @unittest.skipUnless(shutil.which("git"), "Git is required")
    def test_local_fetch_pin_repeat_and_dirty_protection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "origin"
            revision = origin(source)
            dest = root / "checkout"
            dependency = build.Dependency("packager/base", source.as_uri(), revision)
            with contextlib.redirect_stdout(io.StringIO()):
                build.sync_dependency(dest, dependency)
            self.assertEqual([], build.missing_dependencies(dest, [dependency]))
            with mock.patch.object(build, "run", side_effect=AssertionError("unexpected fetch")):
                build.sync_dependency(dest, dependency)
            target = dest / dependency.path
            (target / "pin.txt").write_text("local edit")
            with self.assertRaisesRegex(ValueError, "local changes"):
                build.sync_dependency(dest, dependency)
            self.assertEqual("local edit", (target / "pin.txt").read_text())

    def test_symlink_escape_is_rejected(self):
        if os.name == "nt":
            self.skipTest("Creating symlinks may require extra Windows privileges")
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            root = parent / "root"
            root.mkdir()
            (root / "escape").symlink_to(parent, target_is_directory=True)
            with self.assertRaises(ValueError):
                build.dependency_directory(root, build.Dependency("escape/out", "unused", "a" * 40))


class BuildPlanTests(unittest.TestCase):
    def test_plan_needs_no_python2_or_network(self):
        with mock.patch.object(build, "executable", side_effect=AssertionError("tool probe")):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(0, build.main(["--root", str(PROJECT), "--plan"]))
            plan = json.loads(output.getvalue())
            self.assertEqual(["packager", "mpd_generator", "pssh_box_py"], plan["targets"])
            self.assertEqual(20, len(plan["dependencies"]))

    def test_windows_x64_output_path(self):
        self.assertEqual(Path("project/out/Release_x64"),
                         build.output_directory(Path("project"), "Release", "x64", "win32"))
        self.assertEqual(Path("project/out/Release"),
                         build.output_directory(Path("project"), "Release", "x64", "linux"))

    def test_python3_is_rejected_before_sync(self):
        with mock.patch.object(build, "sync_dependency") as sync:
            with contextlib.redirect_stderr(io.StringIO()):
                result = build.main(["--root", str(PROJECT), "--sync", "--python2", sys.executable])
            self.assertEqual(2, result)
            sync.assert_not_called()

    def test_conflicting_check_and_sync_only_are_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                build.main(["--check", "--sync-only"])

    @unittest.skipIf(os.name == "nt", "POSIX shim test")
    def test_local_python_shim_and_explicit_arch_win_over_environment(self):
        with tempfile.TemporaryDirectory(prefix="packager test ") as temp:
            root = Path(temp)
            with mock.patch.dict(os.environ, {"GYP_DEFINES": "target_arch=ia32 libpackager_type=shared_library"}):
                env = build.build_environment(root, sys.executable, "x64", "static", "linux")
            self.assertIn("target_arch=x64", env["GYP_DEFINES"])
            self.assertIn("libpackager_type=static_library", env["GYP_DEFINES"])
            self.assertEqual("shim-ok", build.capture(
                [str(root / "_build_tools/python"), "-c", "print('shim-ok')"]))


class SourceVersionTests(unittest.TestCase):
    def test_zip_fallback_without_git(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual("unknown-version", version.source_version(temp, {}))

    def test_explicit_version_is_validated(self):
        self.assertEqual("b80099a", version.source_version("unused", {"PACKAGER_VERSION": "b80099a"}))
        with self.assertRaises(ValueError):
            version.source_version("unused", {"PACKAGER_VERSION": 'bad"version'})

    @unittest.skipUnless(shutil.which("git"), "Git is required")
    def test_local_commit_only_and_no_parent_repository_inheritance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "origin"
            revision = origin(root)
            self.assertTrue(revision.startswith(version.source_version(str(root), {})))
            child = root / "source-zip"
            child.mkdir()
            self.assertEqual("unknown-version", version.source_version(str(child), {}))


if __name__ == "__main__":
    unittest.main()
