# Storage Provider
Simple smart-contract system for conclusion of a storage agreements.
- guarantees that the provider stores the file
- no storage - no payment
- no penalties, if provider doesn't store file client can stop payment at any time
- no control that provider upload the file: client can stop payment at any time if not satisfied

## Storage Agreements Fabric

Storage provider deploy storage agreements fabric. Any client may request fabric to deploy storage agreement contract.
Fabric provides get-method `get_storage_params` which returns
- `accept_new_contracts?` - whether provider accepts new contracts
- `rate_per_mb_day` - price in nanoTON per Megabyte per day
- `max_span` - maximal timespan between proving file storage which will be paid
- `minimal_file_size` - minimal file size accepted by provider
- `maximal_file_size` - maximal file size accepted by provider

## Storage agreement
Agreement contract has client account and accept deposits to this account.

It also knows merkle root and allows provider to withdraw money from client account by providing merkle proof of file storage.

Client can stop agreement at any time.
