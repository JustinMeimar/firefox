// The caller compiles monomorphic, then trial inlining replaces the stub of
// its call with one passing the callee an ICScript of its own, which the
// caller runs through its IC chain.
function callee(o) {
  return o.x;
}

function caller(os, n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t += callee(os[i & 1]);
  }
  return t;
}

var a = { x: 1 };
var b = { w: 0, x: 2 };
var os = [a, b];

// Tenure the functions, so only calling a scripted function keeps the stub of
// the call from being copied.
gc();

for (var k = 0; k < 300; k++) {
  assertEq(caller(os, 10), 15);
}

var c = { v: 0, w: 0, x: 3 };
for (var k = 0; k < 100; k++) {
  assertEq(caller([c, a], 10), 20);
}
