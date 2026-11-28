// |jit-test| --baseline-offthread-compile=on; --baseline-mono-speculate-warmup=10
// Compiled monomorphic off thread at the speculation threshold. The code of
// stable is used once it is warm, and the code of changed is dropped since
// its ICs changed after it was compiled.
function stable(a, o) {
  return a + o.x;
}
function changed(a, o) {
  return a + o.x;
}
var mono = {x: 1};
var other = {w: 0, x: 10};
for (var i = 0; i < 100000; i++) {
  assertEq(stable(i, mono), i + 1);
  assertEq(changed(i, i < 20 ? mono : other), i < 20 ? i + 1 : i + 10);
}
assertEq(stable(3, other), 13);
