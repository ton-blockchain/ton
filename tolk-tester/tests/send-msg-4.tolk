struct LiquidityDepositWithInitData {
    code: cell
    data: cell
}

type LiquidityDepositAddress = address

type LiquidityDepositDestination = LiquidityDepositWithInitData | LiquidityDepositAddress

struct (0x1b434676) AddLiquidityPartTon {
    destination: LiquidityDepositDestination
}

type AllowedMessages = AddLiquidityPartTon

fun main() {
    val msgCell = beginCell().
        storeUint(0x1b434676, 32)
        .storeBool(true)    // either right
        .storeAddress(address("Ef_vA6yRfmt2P4UHnxlrQUZFcBnKux8mL2eMqBgpeMFPorr4"))
        .endCell();

    val msg = lazy AllowedMessages.fromCell(msgCell);

    match (msg) {
        AddLiquidityPartTon => {
            // dynamic union: the compiler inserts lots of IFs at runtime to handle this
            var destination: address | AutoDeployAddress;

            match (msg.destination) {
                LiquidityDepositWithInitData => {
                    destination = {
                        stateInit: { code: msg.destination.code, data: msg.destination.data },
                    };
                }
                LiquidityDepositAddress => {
                    destination = msg.destination;
                }
            }

            val sendMsg = createMessage({
                bounce: false,
                dest: destination,
                value: 0,
            });
            sendMsg.send(SEND_MODE_REGULAR);
        }
    }

    return 0;
}

/**
@testcase | 0 | | 0

@fif_codegen HASHBU
@fif_codegen }>ELSE<{
@fif_codegen HASHCU
@fif_codegen 256 STU
 */
