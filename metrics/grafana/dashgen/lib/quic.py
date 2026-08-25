"""QUIC metric compatibility during the message-confirmation rename rollout."""

from .conventions import RATE_WINDOW, summed
from .core import SEL


def confirmation_rate(suffix, by, *, selector=SEL):
    """Prefer confirmation per series before summing; old-only exporters remain visible."""
    return summed(by, " or ".join(
        f"rate(ton_quic_message_{name}_{suffix}{selector}[{RATE_WINDOW}])"
        for name in ("confirmation", "delivery")))
