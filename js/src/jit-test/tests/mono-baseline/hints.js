// Each load runs the same source in a new global, so later loads compile with
// the hints of earlier ones. The third load makes the monomorphic code fail.
var src = `
function getX(o) { return o.x + 1; }
var o = { x: 1 };
for (var i = 0; i < 60; i++) {
  assertEq(getX(o), 2);
}
if (failMono) {
  assertEq(getX({ y: 0, x: 2.5 }), 3.5);
}
for (var i = 0; i < 60; i++) {
  assertEq(getX(i & 1 ? o : { x: 3 }), i & 1 ? 2 : 4);
}
`;
for (var n = 0; n < 4; n++) {
  var g = newGlobal({ sameZoneAs: this });
  g.assertEq = assertEq;
  g.failMono = n == 2;
  g.evaluate(src, { fileName: "mono-baseline-hints.js" });
}
