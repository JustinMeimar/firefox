function locals(o) {
  var x = o.p;
  var y = x + 1;
  return x * 2 + y;
}
function args(a, b) {
  return a + b + a;
}
function strictThis() {
  "use strict";
  return this.x + this.y;
}
function stores(a) {
  var x = a + 1;
  var y = x - 1;
  x = "s";
  return x + y;
}
function argStores(a, b) {
  var t = a + 1;
  a = b;
  return a + t;
}
function loop(n) {
  var x = 0;
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = s + x;
    x = i & 1 ? "a" : i;
  }
  return s;
}
function caught(a) {
  var x = a;
  try {
    x = x + 1;
    if (a > 1000) {
      throw 0;
    }
  } catch (e) {
    x = "c";
  }
  return x + 1;
}

var o = { p: 3 };
var t = { x: 1, y: 2 };

for (var i = 0; i < 100; i++) {
  assertEq(locals(o), 10);
  assertEq(args(i, 2), i + 2 + i);
  assertEq(strictThis.call(t), 3);
  assertEq(stores(i), "s" + i);
  assertEq(argStores(i, 5), i + 6);
  assertEq(loop(4), "0" + "a" + 2);
  assertEq(caught(i), i + 2);
}

assertEq(locals({ p: 1.5 }), 5.5);
assertEq(locals({ p: "q" }), "NaNq1");
assertEq(args("a", 1), "a1a");
assertEq(args(1, "b"), "1b1");
assertEq(strictThis.call({ x: "x", y: 1 }), "x1");
assertEq(stores(0.5), "s0.5");
assertEq(argStores(1, "b"), "b2");
assertEq(argStores(1.5, 2), 4.5);
assertEq(loop(6), "0a2a4");
assertEq(caught(2000), "c1");
assertEq(caught(1.5), 3.5);
