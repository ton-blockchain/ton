from abc import ABC, abstractmethod

from ..models import ConsensusData


class Parser(ABC):
    @abstractmethod
    def parse(self) -> ConsensusData:
        pass
