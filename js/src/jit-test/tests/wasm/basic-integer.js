// |jit-test| test-also-wasm-baseline
load(libdir + "wasm.js");

assertEq(wasmEvalText('(module (func (result i32) (i32.const -1)) (export "" 0))')(), -1);
assertEq(wasmEvalText('(module (func (result i32) (i32.const -2147483648)) (export "" 0))')(), -2147483648);
assertEq(wasmEvalText('(module (func (result i32) (i32.const 4294967295)) (export "" 0))')(), -1);

function testUnary(type, opcode, op, expect) {
    var assertFunc = assertEq;
    if (type === 'i64') {
        expect = createI64(expect);
        assertFunc = assertEqI64;
    }

    // Test with constant
    assertFunc(wasmEvalText(`(module (func (result ${type}) (${type}.${opcode} (${type}.const ${op}))) (export "" 0))`)(), expect);

    if (type === 'i64') {
        op = createI64(op);
    }

    // Test with param
    assertFunc(wasmEvalText(`(module (func (param ${type}) (result ${type}) (${type}.${opcode} (get_local 0))) (export "" 0))`)(op), expect);
}

function testBinary64(opcode, lhs, rhs, expect) {
    let lobj = createI64(lhs);
    let robj = createI64(rhs);
    expect = createI64(expect);

    assertEqI64(wasmEvalText(`(module (func (param i64) (param i64) (result i64) (i64.${opcode} (get_local 0) (get_local 1))) (export "" 0))`)(lobj, robj), expect);
    // The same, but now the RHS is a constant.
    assertEqI64(wasmEvalText(`(module (func (param i64) (result i64) (i64.${opcode} (get_local 0) (i64.const ${rhs}))) (export "" 0))`)(lobj), expect);
    // LHS and RHS are constants.
    assertEqI64(wasmEvalText(`(module (func (result i64) (i64.${opcode} (i64.const ${lhs}) (i64.const ${rhs}))) (export "" 0))`)(), expect);
}

function testBinary32(opcode, lhs, rhs, expect) {
    assertEq(wasmEvalText(`(module (func (param i32) (param i32) (result i32) (i32.${opcode} (get_local 0) (get_local 1))) (export "" 0))`)(lhs, rhs), expect);
    // The same, but now the RHS is a constant.
    assertEq(wasmEvalText(`(module (func (param i32) (result i32) (i32.${opcode} (get_local 0) (i32.const ${rhs}))) (export "" 0))`)(lhs), expect);
    // LHS and RHS are constants.
    assertEq(wasmEvalText(`(module (func (result i32) (i32.${opcode} (i32.const ${lhs}) (i32.const ${rhs}))) (export "" 0))`)(), expect);
}

function testComparison32(opcode, lhs, rhs, expect) {
    assertEq(wasmEvalText(`(module (func (param i32) (param i32) (result i32) (i32.${opcode} (get_local 0) (get_local 1))) (export "" 0))`)(lhs, rhs), expect);
}
function testComparison64(opcode, lhs, rhs, expect) {
    let lobj = createI64(lhs);
    let robj = createI64(rhs);

    assertEq(wasmEvalText(`(module
                            (func (param i64) (param i64) (result i32) (i64.${opcode} (get_local 0) (get_local 1)))
                            (export "" 0))`)(lobj, robj), expect);

    // Also test if, for the compare-and-branch path.
    assertEq(wasmEvalText(`(module
                            (func (param i64) (param i64) (result i32)
                             (if (i64.${opcode} (get_local 0) (get_local 1))
                              (i32.const 1)
                              (i32.const 0)))
                              (export "" 0))`)(lobj, robj), expect);
}
function testI64Eqz(input, expect) {
    assertEq(wasmEvalText(`(module (func (result i32) (i64.eqz (i64.const ${input}))) (export "" 0))`)(input), expect);
    input = createI64(input);
    assertEq(wasmEvalText(`(module (func (param i64) (result i32) (i64.eqz (get_local 0))) (export "" 0))`)(input), expect);
}

function testTrap32(opcode, lhs, rhs, expect) {
    assertErrorMessage(() => wasmEvalText(`(module (func (param i32) (param i32) (result i32) (i32.${opcode} (get_local 0) (get_local 1))) (export "" 0))`)(lhs, rhs), Error, expect);
    // The same, but now the RHS is a constant.
    assertErrorMessage(() => wasmEvalText(`(module (func (param i32) (result i32) (i32.${opcode} (get_local 0) (i32.const ${rhs}))) (export "" 0))`)(lhs), Error, expect);
    // LHS and RHS are constants.
    assertErrorMessage(wasmEvalText(`(module (func (result i32) (i32.${opcode} (i32.const ${lhs}) (i32.const ${rhs}))) (export "" 0))`), Error, expect);
}

function testTrap64(opcode, lhs, rhs, expect) {
    let lobj = createI64(lhs);
    let robj = createI64(rhs);

    assertErrorMessage(() => wasmEvalText(`(module (func (param i64) (param i64) (result i64) (i64.${opcode} (get_local 0) (get_local 1))) (export "" 0))`)(lobj, robj), Error, expect);
    // The same, but now the RHS is a constant.
    assertErrorMessage(() => wasmEvalText(`(module (func (param i64) (result i64) (i64.${opcode} (get_local 0) (i64.const ${rhs}))) (export "" 0))`)(lobj), Error, expect);
    // LHS and RHS are constants.
    assertErrorMessage(wasmEvalText(`(module (func (result i64) (i64.${opcode} (i64.const ${lhs}) (i64.const ${rhs}))) (export "" 0))`), Error, expect);
}

testUnary('i32', 'clz', 40, 26);
testUnary('i32', 'clz', 0, 32);
testUnary('i32', 'clz', 0xFFFFFFFF, 0);
testUnary('i32', 'clz', -2147483648, 0);

testUnary('i32', 'ctz', 40, 3);
testUnary('i32', 'ctz', 0, 32);
testUnary('i32', 'ctz', -2147483648, 31);

testUnary('i32', 'popcnt', 40, 2);
testUnary('i32', 'popcnt', 0, 0);
testUnary('i32', 'popcnt', 0xFFFFFFFF, 32);

testUnary('i32', 'eqz', 0, 1);
testUnary('i32', 'eqz', 1, 0);
testUnary('i32', 'eqz', 0xFFFFFFFF, 0);

testBinary32('add', 40, 2, 42);
testBinary32('sub', 40, 2, 38);
testBinary32('mul', 40, 2, 80);
testBinary32('div_s', -40, 2, -20);
testBinary32('div_u', -40, 2, 2147483628);
testBinary32('rem_s', 40, -3, 1);
testBinary32('rem_u', 40, -3, 40);
testBinary32('and', 42, 6, 2);
testBinary32('or', 42, 6, 46);
testBinary32('xor', 42, 2, 40);
testBinary32('shl', 40, 2, 160);
testBinary32('shr_s', -40, 2, -10);
testBinary32('shr_u', -40, 2, 1073741814);

testTrap32('div_s', 42, 0, /integer divide by zero/);
testTrap32('div_s', 0x80000000 | 0, -1, /integer overflow/);
testTrap32('div_u', 42, 0, /integer divide by zero/);
testTrap32('rem_s', 42, 0, /integer divide by zero/);
testTrap32('rem_u', 42, 0, /integer divide by zero/);

testBinary32('rotl', 40, 2, 160);
testBinary32('rotl', 40, 34, 160);
testBinary32('rotr', 40, 2, 10);
testBinary32('rotr', 40, 34, 10);

testComparison32('eq', 40, 40, 1);
testComparison32('ne', 40, 40, 0);
testComparison32('lt_s', 40, 40, 0);
testComparison32('lt_u', 40, 40, 0);
testComparison32('le_s', 40, 40, 1);
testComparison32('le_u', 40, 40, 1);
testComparison32('gt_s', 40, 40, 0);
testComparison32('gt_u', 40, 40, 0);
testComparison32('ge_s', 40, 40, 1);
testComparison32('ge_u', 40, 40, 1);

// Test MTest's GVN branch inversion.
var testTrunc = wasmEvalText(`(module (func (param f32) (result i32) (if (i32.eqz (i32.trunc_s/f32 (get_local 0))) (i32.const 0) (i32.const 1))) (export "" 0))`);
assertEq(testTrunc(0), 0);
assertEq(testTrunc(13.37), 1);

if (hasI64()) {

    setJitCompilerOption('wasm.test-mode', 1);

    testBinary64('add', 40, 2, 42);
    testBinary64('add', "0x1234567887654321", -1, "0x1234567887654320");
    testBinary64('add', "0xffffffffffffffff", 1, 0);
    testBinary64('sub', 40, 2, 38);
    testBinary64('sub', "0x1234567887654321", "0x123456789", "0x12345677641fdb98");
    testBinary64('sub', 3, 5, -2);
    testBinary64('mul', 40, 2, 80);
    testBinary64('mul', -1, 2, -2);
    testBinary64('mul', 0x123456, "0x9876543210", "0xad77d2c5f941160");
    testBinary64('div_s', -40, 2, -20);
    testBinary64('div_s', "0x1234567887654321", 2, "0x91a2b3c43b2a190");
    testBinary64('div_s', "0x1234567887654321", "0x1000000000", "0x1234567");
    testBinary64('div_u', -40, 2, "0x7fffffffffffffec");
    testBinary64('div_u', "0x1234567887654321", 9, "0x205d0b80f0b4059");
    testBinary64('rem_s', 40, -3, 1);
    testBinary64('rem_s', "0x1234567887654321", "0x1000000000", "0x887654321");
    testBinary64('rem_s', "0x7fffffffffffffff", -1, 0);
    testBinary64('rem_s', "0x8000000000000001", 1000, -807);
    testBinary64('rem_s', "0x8000000000000000", -1, 0);
    testBinary64('rem_u', 40, -3, 40);
    testBinary64('rem_u', "0x1234567887654321", "0x1000000000", "0x887654321");
    testBinary64('rem_u', "0x8000000000000000", -1, "0x8000000000000000");
    testBinary64('rem_u', "0x8ff00ff00ff00ff0", "0x100000001", "0x80000001");

    testTrap64('div_s', 10, 0, /integer divide by zero/);
    testTrap64('div_s', "0x8000000000000000", -1, /integer overflow/);
    testTrap64('div_u', 0, 0, /integer divide by zero/);
    testTrap64('rem_s', 10, 0, /integer divide by zero/);
    testTrap64('rem_u', 10, 0, /integer divide by zero/);

    testBinary64('and', 42, 6, 2);
    testBinary64('or', 42, 6, 46);
    testBinary64('xor', 42, 2, 40);
    testBinary64('and', "0x8765432112345678", "0xffff0000ffff0000", "0x8765000012340000");
    testBinary64('or', "0x8765432112345678", "0xffff0000ffff0000", "0xffff4321ffff5678");
    testBinary64('xor', "0x8765432112345678", "0xffff0000ffff0000", "0x789a4321edcb5678");
    testBinary64('shl', 40, 2, 160);
    testBinary64('shr_s', -40, 2, -10);
    testBinary64('shr_u', -40, 2, "0x3ffffffffffffff6");
    testBinary64('shl', 0xff00ff, 28, "0xff00ff0000000");
    testBinary64('shl', 1, 63, "0x8000000000000000");
    testBinary64('shl', 1, 64, 1);
    testBinary64('shr_s', "0xff00ff0000000", 28, 0xff00ff);
    testBinary64('shr_u', "0x8ffff00ff0000000", 56, 0x8f);
    testBinary64('rotl', 40, 2, 160);
    testBinary64('rotr', 40, 2, 10);

    testComparison64('eq', 40, 40, 1);
    testComparison64('ne', 40, 40, 0);
    testComparison64('lt_s', 40, 40, 0);
    testComparison64('lt_u', 40, 40, 0);
    testComparison64('le_s', 40, 40, 1);
    testComparison64('le_u', 40, 40, 1);
    testComparison64('gt_s', 40, 40, 0);
    testComparison64('gt_u', 40, 40, 0);
    testComparison64('ge_s', 40, 40, 1);
    testComparison64('ge_u', 40, 40, 1);
    testComparison64('eq', "0x400012345678", "0x400012345678", 1);
    testComparison64('ne', "0x400012345678", "0x400012345678", 0);
    testComparison64('ne', "0x400012345678", "0x500012345678", 1);
    testComparison64('eq', "0xffffffffffffffff", -1, 1);
    testComparison64('lt_s', "0x8000000012345678", "0x1", 1);
    testComparison64('lt_u', "0x8000000012345678", "0x1", 0);
    testComparison64('le_s', -1, 0, 1);
    testComparison64('le_u', -1, -1, 1);
    testComparison64('gt_s', 1, "0x8000000000000000", 1);
    testComparison64('gt_u', 1, "0x8000000000000000", 0);
    testComparison64('ge_s', 1, "0x8000000000000000", 1);
    testComparison64('ge_u', 1, "0x8000000000000000", 0);

    testUnary('i64', 'clz', 40, 58);
    testUnary('i64', 'clz', "0x8000000000000000", 0);
    testUnary('i64', 'clz', "0x7fffffffffffffff", 1);
    testUnary('i64', 'clz', "0x4000000000000000", 1);
    testUnary('i64', 'clz', "0x3000000000000000", 2);
    testUnary('i64', 'clz', "0x2000000000000000", 2);
    testUnary('i64', 'clz', "0x1000000000000000", 3);
    testUnary('i64', 'clz', "0x0030000000000000", 10);
    testUnary('i64', 'clz', "0x0000800000000000", 16);
    testUnary('i64', 'clz', "0x00000000ffffffff", 32);
    testUnary('i64', 'clz', -1, 0);
    testUnary('i64', 'clz', 0, 64);

    testUnary('i64', 'ctz', 40, 3);
    testUnary('i64', 'ctz', "0x8000000000000000", 63);
    testUnary('i64', 'ctz', "0x7fffffffffffffff", 0);
    testUnary('i64', 'ctz', "0x4000000000000000", 62);
    testUnary('i64', 'ctz', "0x3000000000000000", 60);
    testUnary('i64', 'ctz', "0x2000000000000000", 61);
    testUnary('i64', 'ctz', "0x1000000000000000", 60);
    testUnary('i64', 'ctz', "0x0030000000000000", 52);
    testUnary('i64', 'ctz', "0x0000800000000000", 47);
    testUnary('i64', 'ctz', "0x00000000ffffffff", 0);
    testUnary('i64', 'ctz', -1, 0);
    testUnary('i64', 'ctz', 0, 64);

    testUnary('i64', 'popcnt', 40, 2);
    testUnary('i64', 'popcnt', 0, 0);
    testUnary('i64', 'popcnt', "0x8000000000000000", 1);
    testUnary('i64', 'popcnt', "0x7fffffffffffffff", 63);
    testUnary('i64', 'popcnt', "0x4000000000000000", 1);
    testUnary('i64', 'popcnt', "0x3000000000000000", 2);
    testUnary('i64', 'popcnt', "0x2000000000000000", 1);
    testUnary('i64', 'popcnt', "0x1000000000000000", 1);
    testUnary('i64', 'popcnt', "0x0030000000000000", 2);
    testUnary('i64', 'popcnt', "0x0000800000000000", 1);
    testUnary('i64', 'popcnt', "0x00000000ffffffff", 32);
    testUnary('i64', 'popcnt', -1, 64);
    testUnary('i64', 'popcnt', 0, 0);

    // Test MTest's GVN branch inversion.
    var testTrunc = wasmEvalText(`(module (func (param f32) (result i32) (if (i64.eqz (i64.trunc_s/f32 (get_local 0))) (i32.const 0) (i32.const 1))) (export "" 0))`);
    assertEq(testTrunc(0), 0);
    assertEq(testTrunc(13.37), 1);

    testI64Eqz(40, 0);
    testI64Eqz(0, 1);

    setJitCompilerOption('wasm.test-mode', 0);
} else {
    // Sleeper test: once i64 works on more platforms, remove this if-else.
    try {
        testComparison64('eq', 40, 40, 1);
        assertEq(0, 1);
    } catch(e) {
        assertEq(e.toString().indexOf("NYI on this platform") >= 0, true);
    }
}

assertErrorMessage(() => wasmEvalText('(module (func (param f32) (result i32) (i32.clz (get_local 0))))'), TypeError, mismatchError("f32", "i32"));
assertErrorMessage(() => wasmEvalText('(module (func (param i32) (result f32) (i32.clz (get_local 0))))'), TypeError, mismatchError("i32", "f32"));
assertErrorMessage(() => wasmEvalText('(module (func (param f32) (result f32) (i32.clz (get_local 0))))'), TypeError, mismatchError("f32", "i32"));

assertErrorMessage(() => wasmEvalText('(module (func (param f32) (param i32) (result i32) (i32.add (get_local 0) (get_local 1))))'), TypeError, mismatchError("f32", "i32"));
assertErrorMessage(() => wasmEvalText('(module (func (param i32) (param f32) (result i32) (i32.add (get_local 0) (get_local 1))))'), TypeError, mismatchError("f32", "i32"));
assertErrorMessage(() => wasmEvalText('(module (func (param i32) (param i32) (result f32) (i32.add (get_local 0) (get_local 1))))'), TypeError, mismatchError("i32", "f32"));
assertErrorMessage(() => wasmEvalText('(module (func (param f32) (param f32) (result f32) (i32.add (get_local 0) (get_local 1))))'), TypeError, mismatchError("f32", "i32"));

assertErrorMessage(() => wasmEvalText('(module (func (param f32) (param i32) (result i32) (i32.eq (get_local 0) (get_local 1))))'), TypeError, mismatchError("f32", "i32"));
assertErrorMessage(() => wasmEvalText('(module (func (param i32) (param f32) (result i32) (i32.eq (get_local 0) (get_local 1))))'), TypeError, mismatchError("f32", "i32"));
assertErrorMessage(() => wasmEvalText('(module (func (param i32) (param i32) (result f32) (i32.eq (get_local 0) (get_local 1))))'), TypeError, mismatchError("i32", "f32"));
