#!/usr/bin/env python
# Compatible with Python 2.7 (legacy GYP) and Python 3.
# Use of this source code is governed by the BSD-style license in LICENSE.
"""Resolve the local source version without contacting another repository."""
from __future__ import print_function
import os
import re
import subprocess
import sys


def source_version(root, environ=None):
  environ = os.environ if environ is None else environ
  supplied = environ.get('PACKAGER_VERSION', '').strip()
  if supplied:
    if not re.match(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$', supplied):
      raise ValueError('PACKAGER_VERSION contains invalid characters')
    return supplied
  # ZIPs inside another checkout must not inherit an unrelated commit ID.
  if not os.path.exists(os.path.join(root, '.git')):
    return 'unknown-version'
  try:
    actual = subprocess.check_output(
        ['git', 'rev-parse', '--show-toplevel'], cwd=root,
        stderr=subprocess.STDOUT).decode('utf-8').strip()
    if os.path.normcase(os.path.realpath(actual)) != os.path.normcase(os.path.realpath(root)):
      return 'unknown-version'
    version = subprocess.check_output(
        ['git', 'rev-parse', '--short', 'HEAD'], cwd=root,
        stderr=subprocess.STDOUT).decode('ascii').strip()
    if re.match(r'^[0-9a-f]{7,40}$', version):
      return version
  except (OSError, subprocess.CalledProcessError):
    pass
  return 'unknown-version'


if __name__ == '__main__':
  root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
  try:
    print(source_version(root))
  except ValueError as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
