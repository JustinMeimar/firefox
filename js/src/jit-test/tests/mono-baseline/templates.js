// Inline stubs copied from templates: many scripts whose stubs have the same
// CacheIR but different shapes, slot offsets and stored values, in two zones,
// with GCs and the pre-barrier verifier.

var source = `
var makers = [];
for (var n = 0; n < 12; n++) {
  makers.push(new Function("o", "v", "o.p" + n + " = v; return o.p" + n + " + o.q;"));
}
function obj(n, dynamic) {
  var o = {};
  var count = dynamic ? 40 : n + 1;
  for (var i = 0; i < count; i++) {
    o["p" + i] = i;
  }
  o.q = 100 + n;
  return o;
}
function run(iters) {
  var fixed = [], dynamic = [];
  for (var n = 0; n < makers.length; n++) {
    fixed.push(obj(n, false));
    dynamic.push(obj(n, true));
  }
  var total = 0;
  for (var i = 0; i < iters; i++) {
    for (var n = 0; n < makers.length; n++) {
      var o = (i & 1) ? fixed[n] : dynamic[n];
      total += makers[n](o, i);
      if (o["p" + n] !== i) {
        throw new Error("bad store " + n);
      }
    }
  }
  return total;
}
function expected(iters) {
  var total = 0;
  for (var i = 0; i < iters; i++) {
    for (var n = 0; n < 12; n++) {
      total += i + 100 + n;
    }
  }
  return total;
}
var holders = [];
for (var n = 0; n < 12; n++) {
  holders.push(new Function("h", "v", "h.x" + (n % 3) + " = v; return h.x" + (n % 3) + ";"));
}
function stores(iters) {
  var hs = [];
  for (var n = 0; n < holders.length; n++) {
    hs.push({x0: null, x1: null, x2: null});
  }
  for (var i = 0; i < iters; i++) {
    for (var n = 0; n < holders.length; n++) {
      var v = {i: i, n: n};
      if (holders[n](hs[n], v) !== v) {
        throw new Error("bad holder " + n);
      }
    }
  }
  return hs;
}
`;

eval(source);
assertEq(run(200), expected(200));

var g = newGlobal({newCompartment: true});
g.eval(source);
assertEq(g.run(200), g.expected(200));

var hs = stores(50);
minorgc();
gc();
for (var n = 0; n < hs.length; n++) {
  assertEq(hs[n]["x" + (n % 3)].i, 49);
}

startgc(1);
for (var i = 0; i < 20 && gcstate() !== "NotActive"; i++) {
  stores(5);
  g.stores(5);
  gcslice(1);
}
finishgc();

gczeal(4);
stores(30);
g.stores(30);
gczeal(0);

var ghs = g.stores(50);
gc();
for (var n = 0; n < ghs.length; n++) {
  assertEq(ghs[n]["x" + (n % 3)].n, n);
}
