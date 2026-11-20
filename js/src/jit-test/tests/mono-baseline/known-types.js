function inc(x) { return x + 1; }
function scale(x) { return x * 1.5; }
function concat(s) { return s + "a"; }
function notPlus(v) { return !v + 1; }
function isNull(x) { return x === null; }
function clamp(ta, i) { ta[i] = 300; return ta[i]; }
function store(ta, i) { ta[i] = -5; return ta[i]; }
function sum(a, b) { var t = a + 2; return t + b; }

var clamped = new Uint8ClampedArray(4);
var ints = new Int32Array(4);

for (var i = 0; i < 100; i++) {
  assertEq(inc(i), i + 1);
  assertEq(scale(2), 3);
  assertEq(concat("b"), "ba");
  assertEq(notPlus(i), i === 0 ? 2 : 1);
  assertEq(isNull(i ? null : 0), i !== 0);
  assertEq(clamp(clamped, i % 4), 255);
  assertEq(store(ints, i % 4), -5);
  assertEq(sum(i, 3), i + 5);
}

assertEq(inc(1.5), 2.5);
assertEq(inc("x"), "x1");
assertEq(scale("2"), 3);
assertEq(concat(1), "1a");
assertEq(notPlus(""), 2);
assertEq(isNull(undefined), false);
assertEq(clamp([], 0), 300);
assertEq(store(clamped, 1), 0);
assertEq(sum("s", 1), "s21");
assertEq(sum(1, "s"), "3s");
