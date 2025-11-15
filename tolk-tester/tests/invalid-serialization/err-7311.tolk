struct (0x01) AskToTransfer {
    jettonAmount: coins
    another: uint8
}

struct (0x02) TransferNotificationForRecipient {
    jettonAmount: coins
    another: uint8
}

type AllowedMessageToVault =
    | AskToTransfer
    | TransferNotificationForRecipient

fun main() {
    var m = lazy AllowedMessageToVault.fromSlice("");
    match (m) {
        AskToTransfer => {
        }
        TransferNotificationForRecipient => {
            m = AskToTransfer{jettonAmount: ton("0.1"), another: 0};
            m.another = 1;
        }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `m` changes its type inside `match`
 */
