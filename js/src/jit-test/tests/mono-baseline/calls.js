function add(a, b) {
  return a + b;
}
function sub(a, b) {
  return a - b;
}
function callIt(f, x) {
  return f(x, 1) + Math.abs(-x);
}

var calls = 0;
var o = {
  get g() {
    if (++calls % 50 === 0) {
      gc(undefined, "shrinking");
    }
    return 7;
  },
};
function getter(p) {
  return p.g + p.g;
}

for (var i = 0; i < 300; i++) {
  assertEq(callIt(add, i), i + 1 + i);
  assertEq(getter(o), 14);
  if (i % 100 === 0) {
    minorgc();
  }
}

assertEq(callIt(sub, 5), 4 + 5);
assertEq(getter({ g: 1 }), 2);

for (var i = 0; i < 300; i++) {
  assertEq(callIt(i & 1 ? add : sub, i), (i & 1 ? i + 1 : i - 1) + i);
  assertEq(getter(i & 1 ? o : { g: 2 }), i & 1 ? 14 : 4);
}
