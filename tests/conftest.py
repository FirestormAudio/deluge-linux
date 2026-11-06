import importlib.util
import sys
from pathlib import Path

# tools/ is not a package; put it on the path so tests can import from it.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

_TOOLS = Path(__file__).resolve().parent.parent / "tools"


def _load(module_name, filename):
    """The CLI tools are hyphenated, so they cannot be imported by name."""
    spec = importlib.util.spec_from_file_location(module_name, _TOOLS / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


mk_app_elf = _load("mk_app_elf", "mk-app-elf.py")
check_app_elf = _load("check_app_elf", "check-app-elf.py")
