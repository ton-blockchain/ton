func -SPA -o storage-contract.fif ../../../crypto/smartcont/stdlib.fc storage-contract.fc && \
  (echo "\"storage-contract.fif\" include boc>B \"storage-contract-code.boc\" B>file" | fift) && \
  func -SPA -o storage-provider.fif ../../../crypto/smartcont/stdlib.fc storage-provider.fc && \
  (echo "\"storage-provider.fif\" include boc>B \"storage-provider-code.boc\" B>file" | fift)
