from abc import ABC, abstractmethod

from ..models import ConsensusData, GroupData


class GroupParser(ABC):
    @abstractmethod
    def list_groups(self) -> list[GroupData]:
        pass

    @abstractmethod
    def parse_group(self, valgroup_name: str) -> ConsensusData:
        pass
