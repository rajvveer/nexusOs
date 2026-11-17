"""Tests' handle on arch/x86_64/nexus_paths.py (where tests keep work folders, caches and disk copies; see there)."""
import importlib.util as _util
from pathlib import Path as _Path

_spec = _util.spec_from_file_location('nexus_paths_impl', _Path(__file__).resolve().parents[1] / 'arch/x86_64/nexus_paths.py')
_impl = _util.module_from_spec(_spec); _spec.loader.exec_module(_impl)
globals().update({_k: _v for _k, _v in vars(_impl).items() if not _k.startswith('__')})
