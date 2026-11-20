function f(o, p) { return o.x + p.y; }

var a = { x: 1 };
var b = { w: 0, x: 2 };
var c = { v: 0, w: 0, x: 3 };
var p = { y: 10 };
var q = { z: 0, y: 20 };

for (var i = 0; i < 100; i++) {
  assertEq(f(i & 1 ? a : b, p), (i & 1 ? 1 : 2) + 10);
}

for (var i = 0; i < 100; i++) {
  assertEq(f(i % 3 == 0 ? a : i % 3 == 1 ? b : c, p), (i % 3) + 1 + 10);
}

for (var i = 0; i < 100; i++) {
  assertEq(f(i & 1 ? a : c, i & 1 ? p : q), i & 1 ? 11 : 23);
}
