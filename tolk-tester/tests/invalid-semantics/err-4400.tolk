struct CalcSwapCostReply {
    cost: coins
}

get fun calc_swap_cost(): CalcSwapCostReply

fun main(): int {
    calc_swap_cost();
    return 0;
}

/**
@cmd_line_option --allow-empty-get-fun
@compilation_should_fail
@stderr get method `calc_swap_cost` is a prototype and can not be called
 */
