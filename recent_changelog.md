## 2024.08 Update

1. Introduction of dispatch queues, message envelopes with transaction chain metadata, and explicitly stored msg_queue size, which will be activated by `Config8.version >= 8` and new `Config8.capabilities` bits: `capStoreOutMsgQueueSize`, `capMsgMetadata`, `capDeferMessages`. 
2. A number of changes to transcation executor which will activated for `Config8.version >= 8`:
    - Check mode on invalid `action_send_msg`. Ignore action if `IGNORE_ERROR` (+2) bit is set, bounce if `BOUNCE_ON_FAIL` (+16) bit is set.
    - Slightly change random seed generation to fix mix of `addr_rewrite` and `addr`.
    - Fill in `skipped_actions` for both invalid and valid messages with `IGNORE_ERROR` mode that can't be sent.
    - Allow unfreeze through external messages.
    - Don't use user-provided `fwd_fee` and `ihr_fee` for internal messages.
3. A few issues with broadcasts were fixed: stop on receiving last piece, response to AdnlMessageCreateChannel
4. A number of fixes and improvements for emulator and tonlib: correct work with config_addr, not accepted externals, bounces, debug ops gas consumption, added version and c5 dump, fixed tonlib crashes
5. Added new flags and commands to the node, in particular `--fast-state-serializer`, `getcollatoroptionsjson`, `setcollatoroptionsjson`

Besides the work of the core team, this update is based on the efforts of @krigga (emulator), stonfi team, in particular @dbaranovstonfi and @hey-researcher (emulator), and  @loeul, @xiaoxianBoy, @simlecode (typos in comments and docs).


