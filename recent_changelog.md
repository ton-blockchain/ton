## 2026.02 Update

1. Preparation for upcoming network speed up: disabled by default but ready to deploy new broadcast and consensus
2. Consequent external message allowed: liteservers now accept external messages with seqno higher than seqno in last commited state iff it knows previous uncommited external message
3. Improved non-final LS interface which allows faster candidate indexing
4. Fixed memory leak which causes OOM on LSes
5. Improved block compression
6. More stable custom overlays
7. Fixed a few performance and stability issues
8. Added Tontester framework

Besides the work of the core team, this update also includes contributions from Vahagn @vah13, InfiniteSec team [https://x.com/infsec_io](x.com/infsec_io) and Christos from [Cantina and Spearbit](cantina.xyz).
