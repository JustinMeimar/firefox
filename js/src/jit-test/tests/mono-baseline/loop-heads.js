function invariant(a, n) {
  var x = a + 1;
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = s + x;
  }
  return s;
}
function assigned(a, n) {
  var x = a + 1;
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = s + x;
    x = i & 1 ? "a" : i;
  }
  return s;
}
function argInvariant(a, n) {
  var t = a - 1;
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = s + a + t;
  }
  return s;
}
function nested(a, n) {
  var x = a + 1;
  var s = 0;
  for (var i = 0; i < n; i++) {
    for (var j = 0; j < n; j++) {
      s = s + x;
    }
    if (i == n - 1) {
      x = "e";
    }
  }
  return s + x;
}

// Entered by OSR from the Baseline Interpreter while it runs.
function osr(a, n) {
  var x = a + 1;
  var k = 2;
  var s = 0;
  for (var i = 0; i < n; i++) {
    s = s + x + k;
  }
  return s;
}
assertEq(osr(1, 1000), 4000);

for (var i = 0; i < 100; i++) {
  assertEq(invariant(i, 3), 3 * (i + 1));
  assertEq(assigned(i, 4), i + 1 + "a" + 2);
  assertEq(argInvariant(i, 2), 2 * (2 * i - 1));
  assertEq(nested(i, 2), 4 * (i + 1) + "e");
}

assertEq(invariant("a", 2), "0a1a1");
assertEq(invariant(1.5, 2), 5);
assertEq(assigned(1.5, 4), "2.5a2");
assertEq(argInvariant("a", 1), "0aNaN");
assertEq(nested("a", 2), "0a1a1a1a1e");
assertEq(osr("a", 2), "0a12a12");
