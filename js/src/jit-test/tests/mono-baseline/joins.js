load(libdir + "asserts.js");

function diamond(a, c) {
  var x = a + 1;
  if (c) {
    x = "s";
  }
  return x + 1;
}
function same(a, c) {
  var x;
  if (c) {
    x = a + 1;
  } else {
    x = a - 1;
  }
  return x * 2;
}
function fallthrough(a, c) {
  var x = a + 1;
  if (c) {
    return 0;
  }
  return x + 1;
}
function tryJoin(a) {
  var x = a + 1;
  try {
    x = "t";
    if (a > 1000) {
      throw 0;
    }
    x = a + 2;
  } catch (e) {}
  return x + 1;
}
function finallyJoin(a) {
  var x = a + 1;
  try {
    if (a > 1000) {
      x = "f";
      return x;
    }
  } finally {
    x = x + 1;
  }
  return x;
}
var last;
function finallyThrow(a) {
  var x = a + 1;
  try {
    x = "e";
    if (a > 1000) {
      throw x;
    }
    x = a + 2;
  } finally {
    x = x + 1;
    last = x;
  }
  return x;
}
function sw(k, a) {
  var x = a + 1;
  switch (k) {
    case 0:
      x = "z";
      break;
    case 1:
      break;
  }
  return x + 1;
}
function cond(a, c) {
  var x = a + 1;
  var y = c ? x : "q";
  return x + 1;
}
function* gen(a) {
  var x = a + 1;
  yield x;
  return x + 1;
}

for (var i = 0; i < 100; i++) {
  assertEq(diamond(i, false), i + 2);
  assertEq(same(i, i & 1), i & 1 ? 2 * i + 2 : 2 * i - 2);
  assertEq(fallthrough(i, false), i + 2);
  assertEq(tryJoin(i), i + 3);
  assertEq(finallyJoin(i), i + 2);
  assertEq(finallyThrow(i), i + 3);
  assertEq(sw(1, i), i + 2);
  assertEq(cond(i, true), i + 2);
  var g = gen(i);
  assertEq(g.next().value, i + 1);
  assertEq(g.next().value, i + 2);
}

assertEq(diamond(1, true), "s1");
assertEq(diamond(1.5, false), 3.5);
assertEq(same(1.5, 1), 5);
assertEq(same("a", 0), NaN);
assertEq(fallthrough("a", false), "a11");
assertEq(tryJoin(2000), "t1");
assertEq(finallyJoin(2000), "f");
assertEq(finallyThrow(1.5), 4.5);
assertThrowsValue(() => finallyThrow(2000), "e");
assertEq(last, "e1");
assertEq(sw(0, 1), "z1");
assertEq(sw(2, "a"), "a11");
assertEq(cond("a", false), "a11");
var g = gen("a");
assertEq(g.next().value, "a1");
assertEq(g.next().value, "a11");
