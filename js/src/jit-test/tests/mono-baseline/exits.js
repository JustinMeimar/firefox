function getX(o) { return o.x + 1; }
function setY(o, v) { o.y = v; return o.y; }
function elem(a, i) { return a[i] * 2; }
function setElem(a, i, v) { a[i] = v; return a[i]; }
function cmp(a, b) { return a < b ? "lt" : "ge"; }
function notb(v) { return !v; }

var mono = { x: 1, y: 0 };
var other = { z: 0, x: 2.5 };
var arr = [1, 2, 3];

for (var i = 0; i < 100; i++) {
  assertEq(getX(mono), 2);
  assertEq(setY(mono, i), i);
  assertEq(elem(arr, i % 3), (i < 3 ? i + 1 : i - 3) * 2);
  assertEq(setElem(arr, i % 3, i), i);
  assertEq(cmp(i, 50), i < 50 ? "lt" : "ge");
  assertEq(notb(mono), false);
}

assertEq(getX(other), 3.5);
assertEq(getX("str"), NaN);
assertEq(setY(other, "q"), "q");
assertEq(setY(Object.freeze({ y: 7 }), 1), 7);
assertEq(elem(arr, "length"), 6);
assertEq(elem({ 1: 4 }, 1), 8);
assertEq(setElem(arr, 10, 5), 5);
assertEq(setElem({}, "k", 3), 3);
assertEq(cmp("a", "b"), "lt");
assertEq(cmp(3.5, 2), "ge");
assertEq(notb(0), true);
assertEq(notb(""), true);

for (var i = 0; i < 100; i++) {
  assertEq(getX(i & 1 ? mono : other), i & 1 ? 2 : 3.5);
  assertEq(elem(i & 1 ? arr : { 0: 1 }, 0), i & 1 ? arr[0] * 2 : 2);
}
