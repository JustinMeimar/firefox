var proto = { get g() { return 3; }, d: 4 };
function C(x) { this.x = x; }
C.prototype = proto;

function f(o) { return o.x + o.d; }

var objs = [];
for (var i = 0; i < 10; i++) {
  objs.push(new C(i));
}

for (var i = 0; i < 300; i++) {
  assertEq(f(objs[i % 10]), i % 10 + 4);
  if (i === 100) {
    minorgc();
  }
  if (i === 150) {
    gc();
  }
  if (i === 200) {
    gc(undefined, "shrinking");
  }
}

proto.d = 5;
assertEq(f(objs[0]), 5);
