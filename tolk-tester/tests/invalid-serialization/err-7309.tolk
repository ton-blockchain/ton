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
fun AllowedMessageToVault.getJettonAmount(self, delta: coins) {
    return match (self) {
        AskToTransfer => self.jettonAmount + delta,
        TransferNotificationForRecipient => self.jettonAmount + delta,
    }
}

fun main() {
    var m = lazy AllowedMessageToVault.fromSlice("");
    match (m) {
        AskToTransfer => {
            return m.getJettonAmount(123);
        }
        TransferNotificationForRecipient => {
            return m.getJettonAmount(456);
        }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `m` changes its type inside `match`
@stderr  ^^^^
@stderr hint: probably, it's reassigned, or called a method with a different receiver
 */
