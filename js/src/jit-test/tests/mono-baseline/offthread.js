// |jit-test| --baseline-offthread-compile=on
// Compiled off thread. The first compile finds no templates, so it calls its
// stubs as copies, and the second copies its stubs from the templates built
// when the first was linked. Both exit with values the compiler had not stored
// below their operands.
function first(a, o) {
  var l = a * 2;
  return l + (a + o.x);
}
function second(a, o) {
  var l = a * 2;
  return l + (a + o.x);
}
var mono = {x: 1};
var other = {w: 0, x: 10};
for (var i = 0; i < 100000; i++) {
  assertEq(first(i, mono), 3 * i + 1);
}
for (var i = 0; i < 100000; i++) {
  assertEq(second(i, mono), 3 * i + 1);
}
assertEq(first(3, other), 19);
assertEq(second(3, other), 19);
