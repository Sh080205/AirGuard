"""Shared ML-1 fixtures; synthetic CSV output stays in data/processed/synth/."""

import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from ml.synth import generate


@pytest.fixture(scope="session")
def week():
    return generate(days=7, seed=42)


@pytest.fixture
def output_dir():
    parent = ROOT / "data" / "processed" / "synth"
    parent.mkdir(parents=True, exist_ok=True)
    # TemporaryDirectory creates and removes only this verified workspace child.
    with TemporaryDirectory(prefix="ml1-test-", dir=parent) as directory:
        yield Path(directory)
