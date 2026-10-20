struct CalcSwapCostReply {
    cost: coins
}

get fun calc_swap_cost(): CalcSwapCostReply;

fun main(): int {
    var f = calc_swap_cost;
    return 0;
}

/**
@cmd_line_option --allow-empty-get-fun
@compilation_should_fail
@stderr can not get reference to this function, it's a get method prototype
 */
