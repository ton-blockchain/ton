## 2023.12 Update

1. Optimized message queue handling, now queue cleaning speed doesn't depend on total queue size
     * Cleaning delivered messages using lt augmentation instead of random search / consequtive walk
     * Keeping root cell of queue message in memory until outdated (caching)
2. Changes to block collation/validation limits
3. Stop accepting new external message if message queue is overloaded
4. Introducing conditions for shard split/merge based on queue size

Read [more](https://blog.ton.org/technical-report-december-5-inscriptions-launch-on-ton) on that update.
