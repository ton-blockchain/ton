from abc import ABC, abstractmethod

from ..models import ConsensusData


class GroupParser(ABC):
    @abstractmethod
    def list_groups(self) -> list[str]:
        pass

    @abstractmethod
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        pass
