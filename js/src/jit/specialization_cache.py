#!/usr/bin/env python3
"""Compatibility entry point; the fossil owns the specialization analyzer."""
import os
from pathlib import Path
import runpy

root = Path(os.environ.get('SPECIALIZATION_FOSSIL',
    Path(__file__).resolve().parents[4] / 'ambermonkey/fossils-II/fossils/0-2-specialization-trace'))
runpy.run_path(str(root / 'specialization_cache.py'), run_name='__main__')
