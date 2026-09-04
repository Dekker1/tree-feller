include "globals.mzn";
int: n = 3;
set of int: N = 1..n;
array[N] of var 1..10: x;
array[N, N] of int: m = array2d(N, N, [1, 2, 3, 4, 5, 6, 7, 8, 9]);

predicate all_even(array[int] of var int: a) = forall(i in index_set(a))(a[i] mod 2 == 0);

function var int: twice(var int: v) :: promise_total = 2 * v;

constraint forall(i in N where i > 1)(x[i] >= x[i - 1]);
constraint sum(i in N)(x[i]) <= 20;
constraint let { var int: t = twice(x[1]) } in t < 15;
constraint if n > 2 then x[1] = 1 else x[1] = 2 endif;

solve :: int_search(x, first_fail, indomain_min) minimize sum(x);
output ["x = \(x)\n"];
