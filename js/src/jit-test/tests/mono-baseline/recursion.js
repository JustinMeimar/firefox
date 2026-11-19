function walk(n, o, p) {
  if (n === 0) {
    return p.v;
  }
  return walk(n - 1, o, p) + o.v;
}

var a = { v: 1 };
var b = { w: 0, v: 10 };

for (var i = 0; i < 100; i++) {
  assertEq(walk(5, a, a), 6);
}

// The deepest frame fails while the outer frames still run the code.
assertEq(walk(5, a, b), 15);

for (var i = 0; i < 200; i++) {
  assertEq(walk(4, i & 1 ? a : b, a), i & 1 ? 5 : 41);
}
