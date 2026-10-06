from pathlib import Path

import esphome.codegen as cg
from tests.testing_helpers import ComponentManifestOverride

# The host platform has no lwIP; tests/stubs/lwip provides a counting IGMP fake.
STUBS_DIR = Path(__file__).resolve().parents[3] / "stubs"


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # Benchmarks construct their own E131Component; only add the stub include path.
    async def to_code(config):
        cg.add_build_flag(f"-I{STUBS_DIR}")

    manifest.enable_codegen()
    manifest.to_code = to_code
