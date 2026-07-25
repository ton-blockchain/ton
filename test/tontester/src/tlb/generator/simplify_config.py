"""Configuration for well-known type simplifications."""

from collections.abc import Mapping
from dataclasses import dataclass, field

from .sema.types import Module, WellKnownType


def _empty_aug_classes() -> dict[tuple[Module, str], str]:
    return {}


@dataclass(frozen=True)
class SimplifyConfig:
    """Controls which well-known type simplifications are active.

    `aug_classes` keys are `(origin_module, type_name)` of typedefs (or
    constructors) wrapping a HashmapAugE; the value is the bare class name
    of the augmentation class to instantiate (e.g. `"DepthBalanceAug"`).
    HashmapAugE simplification only fires when an aug entry exists for the
    containing typedef — otherwise the field falls back to the generic
    (unsimplified) HashmapAugE handling.
    """

    simplify: frozenset[WellKnownType] = field(default_factory=frozenset)
    inline_records: bool = False
    aug_classes: Mapping[tuple[Module, str], str] = field(default_factory=_empty_aug_classes)

    @staticmethod
    def all() -> SimplifyConfig:
        return SimplifyConfig(simplify=frozenset(WellKnownType), inline_records=True)

    @staticmethod
    def none() -> SimplifyConfig:
        return SimplifyConfig()

    def is_enabled(self, wkt: WellKnownType) -> bool:
        return wkt in self.simplify

    def aug_for(self, module: Module, type_name: str) -> str | None:
        return self.aug_classes.get((module, type_name))
