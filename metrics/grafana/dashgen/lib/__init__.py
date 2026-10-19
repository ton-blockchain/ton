"""The vocabulary the boards are written in.

`conventions` holds the house archetypes, `core` the Grafana JSON primitives they
emit; which of the two a name comes from is library bookkeeping, so boards import
everything from here.
"""

from .conventions import *  # noqa: F403
from .core import *  # noqa: F403
