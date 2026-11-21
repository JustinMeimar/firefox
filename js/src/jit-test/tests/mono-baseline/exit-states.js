// Inline stubs exiting with values the compiler had not stored below their
// operands, with their inputs unboxed, and after their stubs were discarded.

function withLocal(a, o) {
  var l = a * 2;
  return l + (a + o.x);
}
function withConstant(o) {
  return "s" + o.x;
}
function g(t, v) {
  return t.base + v;
}
function withThis(o) {
  "use strict";
  return g(this, o.x);
}
function setProp(o, v) {
  o.y = v;
  return o.y;
}
function setElem(a, i, v) {
  a[i] = v;
  return a[i];
}
function cmp(a, b) {
  return a < b;
}
function loop(objs, n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    s += objs[i % 3].x;
    if (i === n / 2) {
      gc(undefined, "shrinking");
    }
  }
  return s;
}

var mono = {x: 1, y: 0};
var other = {w: 0, x: 10};
var holder = {base: 100};
var arr = [1, 2, 3];
var objs = [mono, mono, mono];

for (var i = 0; i < 100; i++) {
  assertEq(withLocal(i, mono), 3 * i + 1);
  assertEq(withConstant(mono), "s1");
  assertEq(withThis.call(holder, mono), 101);
  assertEq(setProp(mono, i), i);
  assertEq(setElem(arr, i % 3, i), i);
  assertEq(cmp(i, 50), i < 50);
}

assertEq(withLocal(3, other), 19);
assertEq(withLocal(4, 5), NaN);
assertEq(withConstant(other), "s10");
assertEq(withConstant("str"), "sundefined");
assertEq(withThis.call(holder, other), 110);
assertEq(withThis.call(holder, null === 1 ? mono : {x: "q"}), "100q");
assertEq(setProp(other, "v"), "v");
assertEq(setProp(Object.freeze({y: 7}), 1), 7);
assertEq(setElem(arr, 10, 5), 5);
assertEq(setElem({}, "k", 3), 3);
assertEq(setElem(arr, 1.5, 2), 2);
assertEq(cmp(1.5, 2), true);
assertEq(cmp("b", "a"), false);

for (var i = 0; i < 40; i++) {
  assertEq(loop(objs, 200), 200);
}
