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

@inline
fun AllowedMessageToVault.setJettonAmount(mutate self, newValue: coins) {
    match (self) {
        AskToTransfer => {
            self.jettonAmount = newValue;
        }
        TransferNotificationForRecipient => {
            self.jettonAmount = newValue;
        }
    }
}

fun main() {
    var m = lazy AllowedMessageToVault.fromSlice("");
    match (m) {
        AskToTransfer => {
            return m.setJettonAmount(123);
        }
        TransferNotificationForRecipient => {
            return m.setJettonAmount(456);
        }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `m` changes its type inside `match`
 */
