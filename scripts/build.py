#!/usr/bin/env python3
"""Build this legacy checkout using pinned DEPS, without replacing its source."""

import argparse
import ast
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys


@dataclass(frozen=True)
class Dependency:
    path: str
    url: str
    revision: str


def _value(node, variables):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Dict):
        return {_value(k, variables): _value(v, variables)
                for k, v in zip(node.keys, node.values)}
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return _value(node.left, variables) + _value(node.right, variables)
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
            and node.func.id == "Var" and len(node.args) == 1
            and not node.keywords):
        return variables[_value(node.args[0], variables)]
    raise ValueError("Unsupported expression in DEPS data")


def dependencies(root, platform):
    """Read data assignments only; never execute DEPS hooks or other code."""
    data = {}
    for node in ast.parse((root / "DEPS").read_text(encoding="utf-8")).body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in ("vars", "deps", "deps_os")):
            data[node.targets[0].id] = _value(node.value, data.get("vars", {}))
    selected = dict(data["deps"])
    selected.update(data.get("deps_os", {}).get(
        "win" if platform == "win32" else "unix" if platform == "linux" else "mac", {}))
    result = []
    for location, spec in sorted(selected.items()):
        if not location.startswith("src/"):
            raise ValueError("Dependency must be rooted under src/: " + location)
        relative = PurePosixPath(location[4:])
        if relative.is_absolute() or ".." in relative.parts or str(relative) == ".":
            raise ValueError("Unsafe dependency path: " + location)
        url, revision = spec.rsplit("@", 1)
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError("Dependency needs a complete commit ID: " + location)
        result.append(Dependency(str(relative), url, revision))
    return result


def capture(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, stderr=subprocess.STDOUT,
                                   universal_newlines=True).strip()


def run(args, cwd=None, env=None):
    print("+ " + subprocess.list2cmdline([str(a) for a in args]), flush=True)
    subprocess.check_call([str(a) for a in args], cwd=cwd, env=env)


def dependency_directory(root, dependency):
    root = root.resolve()
    target = (root / dependency.path).resolve()
    if root not in target.parents:
        raise ValueError("Dependency path escapes the checkout")
    return target


def sync_dependency(root, dependency, git="git"):
    target = dependency_directory(root, dependency)
    if target.exists() and any(target.iterdir()):
        if not (target / ".git").exists():
            raise ValueError("Refusing to replace existing files: " + str(target))
        actual_root = Path(capture([git, "-C", str(target), "rev-parse", "--show-toplevel"]))
        if actual_root.resolve() != target:
            raise ValueError("Dependency is not its own Git checkout: " + str(target))
        if capture([git, "-C", str(target), "status", "--porcelain"]):
            raise ValueError("Dependency has local changes: " + str(target))
        try:
            current = capture([git, "-C", str(target), "rev-parse", "HEAD"])
        except subprocess.CalledProcessError:
            current = ""  # Resume an interrupted initial fetch.
        if current == dependency.revision:
            print("Already pinned: " + dependency.path, flush=True)
            return
        origin = capture([git, "-C", str(target), "remote", "get-url", "origin"])
        if origin != dependency.url:
            raise ValueError("Unexpected dependency origin: " + str(target))
    else:
        target.mkdir(parents=True, exist_ok=True)
        run([git, "init", "--quiet", str(target)])
        run([git, "-C", str(target), "remote", "add", "origin", dependency.url])
    run([git, "-C", str(target), "fetch", "--depth", "1", "origin", dependency.revision])
    run([git, "-C", str(target), "checkout", "--detach", "FETCH_HEAD"])
    if capture([git, "-C", str(target), "rev-parse", "HEAD"]) != dependency.revision:
        raise ValueError("Fetched dependency does not match its pinned revision")


def missing_dependencies(root, items, git="git"):
    missing = []
    for dependency in items:
        target = dependency_directory(root, dependency)
        try:
            if not (target / ".git").exists():
                raise ValueError("not initialized")
            if capture([git, "-C", str(target), "rev-parse", "HEAD"]) != dependency.revision:
                raise ValueError("wrong revision")
        except (ValueError, OSError, subprocess.CalledProcessError):
            missing.append(dependency.path)
    return missing


def merge_defines(existing, defaults):
    values = dict(defaults)
    for item in shlex.split(existing):
        key, separator, value = item.partition("=")
        if not separator:
            raise ValueError("Invalid GYP_DEFINES item: " + item)
        values[key] = value
    return " ".join(key + "=" + shlex.quote(str(value)) for key, value in sorted(values.items()))


def build_environment(root, python2, arch, library_type, platform):
    env = os.environ.copy()
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    env["GYP_GENERATORS"] = "ninja"
    env["GYP_DEFINES"] = merge_defines(env.get("GYP_DEFINES", ""), {
        "target_arch": arch, "clang": 0, "use_sysroot": 0,
        "use_custom_libcxx": 0, "use_allocator": "none",
        "use_experimental_allocator_shim": 0, "werror": 0,
        "libpackager_type": library_type + "_library",
    })
    # The output directory and these explicit CLI selections must agree.
    env["GYP_DEFINES"] = merge_defines(
        env["GYP_DEFINES"] + " target_arch=" + arch +
        " libpackager_type=" + library_type + "_library", {})
    if platform == "win32":
        # A normal Python 2.7 Windows installation supplies python.exe here.
        if Path(python2).name.lower() != "python.exe":
            raise ValueError("On Windows --python2 must point to Python 2.7 python.exe")
        python_dir = Path(python2).resolve().parent
        env.setdefault("GYP_MSVS_VERSION", "2019")
    else:
        # GYP actions invoke 'python'; keep those on the selected Python 2.7.
        python_dir = root / "_build_tools"
        python_dir.mkdir(exist_ok=True)
        shim = python_dir / "python"
        shim.write_text("#!/bin/sh\nexec " + shlex.quote(str(Path(python2).resolve()))
                        + ' "$@"\n', encoding="utf-8")
        shim.chmod(0o755)
    env["PATH"] = str(python_dir) + os.pathsep + env.get("PATH", "")
    return env


def output_directory(root, configuration, arch, platform):
    suffix = "_x64" if platform == "win32" and arch == "x64" else ""
    return root / "out" / (configuration + suffix)


def executable(value):
    found = shutil.which(value)
    if not found:
        raise ValueError("Required tool not found: " + value)
    return str(Path(found).resolve())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--sync", action="store_true", help="Fetch exact dependency commits from DEPS")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--sync-only", action="store_true", help="Fetch dependencies without compiling")
    modes.add_argument("--check", action="store_true", help="Check prerequisites without fetching or building")
    modes.add_argument("--plan", action="store_true", help="Show the build/dependency plan without making changes")
    parser.add_argument("--python2", default="python2", help="Python 2.7 executable used by legacy GYP")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--arch", choices=("x64", "ia32"), default="x64")
    parser.add_argument("--library-type", choices=("static", "shared"), default="static")
    parser.add_argument("--targets", nargs="+", default=["packager", "mpd_generator", "pssh_box_py"])
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if not (root / "gyp_packager.py").is_file():
            raise ValueError("Not a Packager source directory: " + str(root))
        items = dependencies(root, sys.platform)
        output = output_directory(root, args.configuration, args.arch, sys.platform)
        if args.plan:
            print(json.dumps({"root": str(root), "output": str(output),
                              "targets": args.targets, "python2": args.python2,
                              "dependencies": [d.__dict__ for d in items]}, indent=2))
            return 0
        git = executable("git")
        # Check the complete toolchain before a potentially long initial sync.
        if not args.sync_only:
            python2 = executable(args.python2)
            version = capture([python2, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"])
            if version != "2.7":
                raise ValueError("Legacy GYP requires Python 2.7; --python2 selected " + version)
            ninja = executable(args.ninja)
        if (args.sync or args.sync_only) and not args.check:
            for dependency in items:
                sync_dependency(root, dependency, git)
        if args.sync_only:
            return 0
        missing = missing_dependencies(root, items, git)
        if missing:
            raise ValueError("Missing/unpinned dependencies; run with --sync:\n  " + "\n  ".join(missing))
        if args.check:
            print("Toolchain and pinned dependencies are ready.")
            return 0
        env = build_environment(root, python2, args.arch, args.library_type, sys.platform)
        lastchange = root / "packager/build/util/lastchange.py"
        if lastchange.is_file():
            run([python2, lastchange, "-o", lastchange.parent / "LASTCHANGE"], cwd=root, env=env)
        run([python2, root / "gyp_packager.py"], cwd=root, env=env)
        run([ninja, "-C", output] + args.targets, cwd=root, env=env)
        print("Built artifacts: " + str(output))
        return 0
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print("Build error: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
