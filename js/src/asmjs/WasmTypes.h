/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_types_h
#define wasm_types_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/RefCounted.h"
#include "mozilla/RefPtr.h"

#include "NamespaceImports.h"

#include "asmjs/WasmBinary.h"
#include "ds/LifoAlloc.h"
#include "jit/IonTypes.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"

namespace js {

class PropertyName;
namespace jit { struct BaselineScript; }

namespace wasm {

using mozilla::DebugOnly;
using mozilla::EnumeratedArray;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::MallocSizeOf;
using mozilla::PodZero;
using mozilla::PodCopy;
using mozilla::RefCounted;

typedef Vector<uint32_t, 0, SystemAllocPolicy> Uint32Vector;

// To call Vector::podResizeToFit, a type must specialize mozilla::IsPod
// which is pretty verbose to do within js::wasm, so factor that process out
// into a macro.

#define WASM_DECLARE_POD_VECTOR(Type, VectorName)                               \
} } namespace mozilla {                                                         \
template <> struct IsPod<js::wasm::Type> : TrueType {};                         \
} namespace js { namespace wasm {                                               \
typedef Vector<Type, 0, SystemAllocPolicy> VectorName;

// A wasm Module and everything it contains must support serialization and
// deserialization. Some data can be simply copied as raw bytes and,
// as a convention, is stored in an inline CacheablePod struct. Everything else
// should implement the below methods which are called recusively by the
// containing Module.

#define WASM_DECLARE_SERIALIZABLE(Type)                                         \
    size_t serializedSize() const;                                              \
    uint8_t* serialize(uint8_t* cursor) const;                                  \
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);    \
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#define WASM_DECLARE_SERIALIZABLE_VIRTUAL(Type)                                 \
    virtual size_t serializedSize() const;                                      \
    virtual uint8_t* serialize(uint8_t* cursor) const;                          \
    virtual const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);\
    virtual size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#define WASM_DECLARE_SERIALIZABLE_OVERRIDE(Type)                                \
    size_t serializedSize() const override;                                     \
    uint8_t* serialize(uint8_t* cursor) const override;                         \
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor) override;\
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const override;

// ValType/ExprType utilities

// ExprType::Limit is an out-of-band value and has no wasm-semantic meaning. For
// the purpose of recursive validation, we use this value to represent the type
// of branch/return instructions that don't actually return to the parent
// expression and can thus be used in any context.
const ExprType AnyType = ExprType::Limit;

inline ExprType
Unify(ExprType a, ExprType b)
{
    if (a == AnyType)
        return b;
    if (b == AnyType)
        return a;
    if (a == b)
        return a;
    return ExprType::Void;
}

static inline bool
IsVoid(ExprType et)
{
    return et == ExprType::Void;
}

static inline ValType
NonVoidToValType(ExprType et)
{
    MOZ_ASSERT(!IsVoid(et));
    return ValType(et);
}

static inline ExprType
ToExprType(ValType vt)
{
    return ExprType(vt);
}

static inline bool
IsSimdType(ValType vt)
{
    switch (vt) {
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        return true;
      default:
        return false;
    }
}

static inline uint32_t
NumSimdElements(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::B8x16:
        return 16;
      case ValType::I16x8:
      case ValType::B16x8:
        return 8;
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B32x4:
        return 4;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline ValType
SimdElementType(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
        return ValType::I32;
      case ValType::F32x4:
        return ValType::F32;
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        return ValType::I32;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline ValType
SimdBoolType(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::B8x16:
        return ValType::B8x16;
      case ValType::I16x8:
      case ValType::B16x8:
        return ValType::B16x8;
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B32x4:
        return ValType::B32x4;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline bool
IsSimdType(ExprType et)
{
    return IsVoid(et) ? false : IsSimdType(ValType(et));
}

static inline bool
IsSimdBoolType(ValType vt)
{
    return vt == ValType::B8x16 || vt == ValType::B16x8 || vt == ValType::B32x4;
}

static inline jit::MIRType
ToMIRType(ValType vt)
{
    switch (vt) {
      case ValType::I32: return jit::MIRType::Int32;
      case ValType::I64: return jit::MIRType::Int64;
      case ValType::F32: return jit::MIRType::Float32;
      case ValType::F64: return jit::MIRType::Double;
      case ValType::I8x16: return jit::MIRType::Int8x16;
      case ValType::I16x8: return jit::MIRType::Int16x8;
      case ValType::I32x4: return jit::MIRType::Int32x4;
      case ValType::F32x4: return jit::MIRType::Float32x4;
      case ValType::B8x16: return jit::MIRType::Bool8x16;
      case ValType::B16x8: return jit::MIRType::Bool16x8;
      case ValType::B32x4: return jit::MIRType::Bool32x4;
      case ValType::Limit: break;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("bad type");
}

static inline jit::MIRType
ToMIRType(ExprType et)
{
    return IsVoid(et) ? jit::MIRType::None : ToMIRType(ValType(et));
}

static inline const char*
ToCString(ExprType type)
{
    switch (type) {
      case ExprType::Void:  return "void";
      case ExprType::I32:   return "i32";
      case ExprType::I64:   return "i64";
      case ExprType::F32:   return "f32";
      case ExprType::F64:   return "f64";
      case ExprType::I8x16: return "i8x16";
      case ExprType::I16x8: return "i16x8";
      case ExprType::I32x4: return "i32x4";
      case ExprType::F32x4: return "f32x4";
      case ExprType::B8x16: return "b8x16";
      case ExprType::B16x8: return "b16x8";
      case ExprType::B32x4: return "b32x4";
      case ExprType::Limit:;
    }
    MOZ_CRASH("bad expression type");
}

static inline const char*
ToCString(ValType type)
{
    return ToCString(ToExprType(type));
}

// The Val class represents a single WebAssembly value of a given value type,
// mostly for the purpose of numeric literals and initializers. A Val does not
// directly map to a JS value since there is not (currently) a precise
// representation of i64 values. A Val may contain non-canonical NaNs since,
// within WebAssembly, floats are not canonicalized. Canonicalization must
// happen at the JS boundary.

class Val
{
    ValType type_;
    union {
        uint32_t i32_;
        uint64_t i64_;
        float f32_;
        double f64_;
        I8x16 i8x16_;
        I16x8 i16x8_;
        I32x4 i32x4_;
        F32x4 f32x4_;
    } u;

  public:
    Val() = default;

    explicit Val(uint32_t i32) : type_(ValType::I32) { u.i32_ = i32; }
    explicit Val(uint64_t i64) : type_(ValType::I64) { u.i64_ = i64; }
    explicit Val(float f32) : type_(ValType::F32) { u.f32_ = f32; }
    explicit Val(double f64) : type_(ValType::F64) { u.f64_ = f64; }

    explicit Val(const I8x16& i8x16, ValType type = ValType::I8x16) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I8x16 || type_ == ValType::B8x16);
        memcpy(u.i8x16_, i8x16, sizeof(u.i8x16_));
    }
    explicit Val(const I16x8& i16x8, ValType type = ValType::I16x8) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I16x8 || type_ == ValType::B16x8);
        memcpy(u.i16x8_, i16x8, sizeof(u.i16x8_));
    }
    explicit Val(const I32x4& i32x4, ValType type = ValType::I32x4) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        memcpy(u.i32x4_, i32x4, sizeof(u.i32x4_));
    }
    explicit Val(const F32x4& f32x4) : type_(ValType::F32x4) {
        memcpy(u.f32x4_, f32x4, sizeof(u.f32x4_));
    }

    ValType type() const { return type_; }
    bool isSimd() const { return IsSimdType(type()); }

    uint32_t i32() const { MOZ_ASSERT(type_ == ValType::I32); return u.i32_; }
    uint64_t i64() const { MOZ_ASSERT(type_ == ValType::I64); return u.i64_; }
    float f32() const { MOZ_ASSERT(type_ == ValType::F32); return u.f32_; }
    double f64() const { MOZ_ASSERT(type_ == ValType::F64); return u.f64_; }

    const I8x16& i8x16() const {
        MOZ_ASSERT(type_ == ValType::I8x16 || type_ == ValType::B8x16);
        return u.i8x16_;
    }
    const I16x8& i16x8() const {
        MOZ_ASSERT(type_ == ValType::I16x8 || type_ == ValType::B16x8);
        return u.i16x8_;
    }
    const I32x4& i32x4() const {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        return u.i32x4_;
    }
    const F32x4& f32x4() const {
        MOZ_ASSERT(type_ == ValType::F32x4);
        return u.f32x4_;
    }

    void writePayload(uint8_t* dst);
};

// The Sig class represents a WebAssembly function signature which takes a list
// of value types and returns an expression type. The engine uses two in-memory
// representations of the argument Vector's memory (when elements do not fit
// inline): normal malloc allocation (via SystemAllocPolicy) and allocation in
// a LifoAlloc (via LifoAllocPolicy). The former Sig objects can have any
// lifetime since they own the memory. The latter Sig objects must not outlive
// the associated LifoAlloc mark/release interval (which is currently the
// duration of module validation+compilation). Thus, long-lived objects like
// WasmModule must use malloced allocation.

class Sig
{
    ValTypeVector args_;
    ExprType ret_;

    Sig(const Sig&) = delete;
    Sig& operator=(const Sig&) = delete;

  public:
    Sig() : args_(), ret_(ExprType::Void) {}
    Sig(Sig&& rhs) : args_(Move(rhs.args_)), ret_(rhs.ret_) {}
    Sig(ValTypeVector&& args, ExprType ret) : args_(Move(args)), ret_(ret) {}

    MOZ_MUST_USE bool clone(const Sig& rhs) {
        ret_ = rhs.ret_;
        MOZ_ASSERT(args_.empty());
        return args_.appendAll(rhs.args_);
    }
    Sig& operator=(Sig&& rhs) {
        ret_ = rhs.ret_;
        args_ = Move(rhs.args_);
        return *this;
    }

    ValType arg(unsigned i) const { return args_[i]; }
    const ValTypeVector& args() const { return args_; }
    const ExprType& ret() const { return ret_; }

    HashNumber hash() const {
        return AddContainerToHash(args_, HashNumber(ret_));
    }
    bool operator==(const Sig& rhs) const {
        return ret() == rhs.ret() && EqualContainers(args(), rhs.args());
    }
    bool operator!=(const Sig& rhs) const {
        return !(*this == rhs);
    }
};

struct SigHashPolicy
{
    typedef const Sig& Lookup;
    static HashNumber hash(Lookup sig) { return sig.hash(); }
    static bool match(const Sig* lhs, Lookup rhs) { return *lhs == rhs; }
};

// A GlobalDesc describes a single global variable. Currently, globals are only
// exposed through asm.js.

struct GlobalDesc
{
    ValType type;
    unsigned globalDataOffset;
    bool isConst;
    GlobalDesc(ValType type, unsigned offset, bool isConst)
      : type(type), globalDataOffset(offset), isConst(isConst)
    {}
};

typedef Vector<GlobalDesc, 0, SystemAllocPolicy> GlobalDescVector;

// A "declared" signature is a Sig object that is created and owned by the
// ModuleGenerator. These signature objects are read-only and have the same
// lifetime as the ModuleGenerator. This type is useful since some uses of Sig
// need this extended lifetime and want to statically distinguish from the
// common stack-allocated Sig objects that get passed around.

struct DeclaredSig : Sig
{
    DeclaredSig() = default;
    DeclaredSig(DeclaredSig&& rhs) : Sig(Move(rhs)) {}
    explicit DeclaredSig(Sig&& sig) : Sig(Move(sig)) {}
    void operator=(Sig&& rhs) { Sig& base = *this; base = Move(rhs); }
};

typedef Vector<DeclaredSig, 0, SystemAllocPolicy> DeclaredSigVector;
typedef Vector<const DeclaredSig*, 0, SystemAllocPolicy> DeclaredSigPtrVector;

// The (,Profiling,Func)Offsets classes are used to record the offsets of
// different key points in a CodeRange during compilation.

struct Offsets
{
    explicit Offsets(uint32_t begin = 0, uint32_t end = 0)
      : begin(begin), end(end)
    {}

    // These define a [begin, end) contiguous range of instructions compiled
    // into a CodeRange.
    uint32_t begin;
    uint32_t end;

    void offsetBy(uint32_t offset) {
        begin += offset;
        end += offset;
    }
};

struct ProfilingOffsets : Offsets
{
    MOZ_IMPLICIT ProfilingOffsets(uint32_t profilingReturn = 0)
      : Offsets(), profilingReturn(profilingReturn)
    {}

    // For CodeRanges with ProfilingOffsets, 'begin' is the offset of the
    // profiling entry.
    uint32_t profilingEntry() const { return begin; }

    // The profiling return is the offset of the return instruction, which
    // precedes the 'end' by a variable number of instructions due to
    // out-of-line codegen.
    uint32_t profilingReturn;

    void offsetBy(uint32_t offset) {
        Offsets::offsetBy(offset);
        profilingReturn += offset;
    }
};

struct FuncOffsets : ProfilingOffsets
{
    MOZ_IMPLICIT FuncOffsets()
      : ProfilingOffsets(),
        tableEntry(0),
        tableProfilingJump(0),
        nonProfilingEntry(0),
        profilingJump(0),
        profilingEpilogue(0)
    {}

    // Function CodeRanges have a table entry which takes an extra signature
    // argument which is checked against the callee's signature before falling
    // through to the normal prologue. When profiling is enabled, a nop on the
    // fallthrough is patched to instead jump to the profiling epilogue.
    uint32_t tableEntry;
    uint32_t tableProfilingJump;

    // Function CodeRanges have an additional non-profiling entry that comes
    // after the profiling entry and a non-profiling epilogue that comes before
    // the profiling epilogue.
    uint32_t nonProfilingEntry;

    // When profiling is enabled, the 'nop' at offset 'profilingJump' is
    // overwritten to be a jump to 'profilingEpilogue'.
    uint32_t profilingJump;
    uint32_t profilingEpilogue;

    void offsetBy(uint32_t offset) {
        ProfilingOffsets::offsetBy(offset);
        tableEntry += offset;
        tableProfilingJump += offset;
        nonProfilingEntry += offset;
        profilingJump += offset;
        profilingEpilogue += offset;
    }
};

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes a single callsite to which CallSite adds
// the metadata necessary to walk up to the next frame. Lastly CallSiteAndTarget
// adds the function index of the callee.

class CallSiteDesc
{
    uint32_t lineOrBytecode_ : 31;
    uint32_t kind_ : 1;
  public:
    enum Kind {
        Relative,  // pc-relative call
        Register   // call *register
    };
    CallSiteDesc() {}
    explicit CallSiteDesc(Kind kind)
      : lineOrBytecode_(0), kind_(kind)
    {}
    CallSiteDesc(uint32_t lineOrBytecode, Kind kind)
      : lineOrBytecode_(lineOrBytecode), kind_(kind)
    {
        MOZ_ASSERT(lineOrBytecode_ == lineOrBytecode, "must fit in 31 bits");
    }
    uint32_t lineOrBytecode() const { return lineOrBytecode_; }
    Kind kind() const { return Kind(kind_); }
};

class CallSite : public CallSiteDesc
{
    uint32_t returnAddressOffset_;
    uint32_t stackDepth_;

  public:
    CallSite() {}

    CallSite(CallSiteDesc desc, uint32_t returnAddressOffset, uint32_t stackDepth)
      : CallSiteDesc(desc),
        returnAddressOffset_(returnAddressOffset),
        stackDepth_(stackDepth)
    { }

    void setReturnAddressOffset(uint32_t r) { returnAddressOffset_ = r; }
    void offsetReturnAddressBy(int32_t o) { returnAddressOffset_ += o; }
    uint32_t returnAddressOffset() const { return returnAddressOffset_; }

    // The stackDepth measures the amount of stack space pushed since the
    // function was called. In particular, this includes the pushed return
    // address on all archs (whether or not the call instruction pushes the
    // return address (x86/x64) or the prologue does (ARM/MIPS)).
    uint32_t stackDepth() const { return stackDepth_; }
};

WASM_DECLARE_POD_VECTOR(CallSite, CallSiteVector)

class CallSiteAndTarget : public CallSite
{
    uint32_t targetIndex_;

  public:
    CallSiteAndTarget(CallSite cs, uint32_t targetIndex)
      : CallSite(cs), targetIndex_(targetIndex)
    { }

    static const uint32_t NOT_INTERNAL = UINT32_MAX;

    bool isInternal() const { return targetIndex_ != NOT_INTERNAL; }
    uint32_t targetIndex() const { MOZ_ASSERT(isInternal()); return targetIndex_; }
};

typedef Vector<CallSiteAndTarget, 0, SystemAllocPolicy> CallSiteAndTargetVector;

// Metadata for a bounds check that may need patching later.

class BoundsCheck
{
  public:
    BoundsCheck() = default;

    explicit BoundsCheck(uint32_t cmpOffset)
      : cmpOffset_(cmpOffset)
    { }

    uint8_t* patchAt(uint8_t* code) const { return code + cmpOffset_; }
    void offsetBy(uint32_t offset) { cmpOffset_ += offset; }

  private:
    uint32_t cmpOffset_; // absolute offset of the comparison
};

// Summarizes a heap access made by wasm code that needs to be patched later
// and/or looked up by the wasm signal handlers. Different architectures need
// to know different things (x64: offset and length, ARM: where to patch in
// heap length, x86: where to patch in heap length and base).

#if defined(JS_CODEGEN_X86)
class MemoryAccess
{
    uint32_t nextInsOffset_;

  public:
    MemoryAccess() = default;

    explicit MemoryAccess(uint32_t nextInsOffset)
      : nextInsOffset_(nextInsOffset)
    { }

    void* patchHeapPtrImmAt(uint8_t* code) const { return code + nextInsOffset_; }
    void offsetBy(uint32_t offset) { nextInsOffset_ += offset; }
};
#elif defined(JS_CODEGEN_X64)
class MemoryAccess
{
    uint32_t insnOffset_;
    uint8_t offsetWithinWholeSimdVector_; // if is this e.g. the Z of an XYZ
    bool throwOnOOB_;                     // should we throw on OOB?
    bool wrapOffset_;                     // should we wrap the offset on OOB?

  public:
    enum OutOfBoundsBehavior {
        Throw,
        CarryOn,
    };
    enum WrappingBehavior {
        WrapOffset,
        DontWrapOffset,
    };

    MemoryAccess() = default;

    MemoryAccess(uint32_t insnOffset, OutOfBoundsBehavior onOOB, WrappingBehavior onWrap,
                 uint32_t offsetWithinWholeSimdVector = 0)
      : insnOffset_(insnOffset),
        offsetWithinWholeSimdVector_(offsetWithinWholeSimdVector),
        throwOnOOB_(onOOB == OutOfBoundsBehavior::Throw),
        wrapOffset_(onWrap == WrappingBehavior::WrapOffset)
    {
        MOZ_ASSERT(offsetWithinWholeSimdVector_ == offsetWithinWholeSimdVector, "fits in uint8");
    }

    uint32_t insnOffset() const { return insnOffset_; }
    uint32_t offsetWithinWholeSimdVector() const { return offsetWithinWholeSimdVector_; }
    bool throwOnOOB() const { return throwOnOOB_; }
    bool wrapOffset() const { return wrapOffset_; }

    void offsetBy(uint32_t offset) { insnOffset_ += offset; }
};
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
      defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) || \
      defined(JS_CODEGEN_NONE)
// Nothing! We just want bounds checks on these platforms.
class MemoryAccess {
  public:
    void offsetBy(uint32_t) { MOZ_CRASH(); }
    uint32_t insnOffset() const { MOZ_CRASH(); }
};
#endif

WASM_DECLARE_POD_VECTOR(MemoryAccess, MemoryAccessVector)
WASM_DECLARE_POD_VECTOR(BoundsCheck, BoundsCheckVector)

// A wasm::SymbolicAddress represents a pointer to a well-known function or
// object that is embedded in wasm code. Since wasm code is serialized and
// later deserialized into a different address space, symbolic addresses must be
// used for *all* pointers into the address space. The MacroAssembler records a
// list of all SymbolicAddresses and the offsets of their use in the code for
// later patching during static linking.

enum class SymbolicAddress
{
    ToInt32,
#if defined(JS_CODEGEN_ARM)
    aeabi_idivmod,
    aeabi_uidivmod,
    AtomicCmpXchg,
    AtomicXchg,
    AtomicFetchAdd,
    AtomicFetchSub,
    AtomicFetchAnd,
    AtomicFetchOr,
    AtomicFetchXor,
#endif
    ModD,
    SinD,
    CosD,
    TanD,
    ASinD,
    ACosD,
    ATanD,
    CeilD,
    CeilF,
    FloorD,
    FloorF,
    TruncD,
    TruncF,
    NearbyIntD,
    NearbyIntF,
    ExpD,
    LogD,
    PowD,
    ATan2D,
    Runtime,
    RuntimeInterruptUint32,
    StackLimit,
    ReportOverRecursed,
    HandleExecutionInterrupt,
    HandleTrap,
    CallImport_Void,
    CallImport_I32,
    CallImport_I64,
    CallImport_F64,
    CoerceInPlace_ToInt32,
    CoerceInPlace_ToNumber,
    Limit
};

void*
AddressOf(SymbolicAddress imm, ExclusiveContext* cx);

// A wasm::Trap is a reason for why we reached a trap in executed code. Each
// different trap is mapped to a different error message.

enum class Trap
{
    // The Unreachable opcode has been executed.
    Unreachable,
    // An integer arithmetic operation led to an overflow.
    IntegerOverflow,
    // Trying to coerce NaN to an integer.
    InvalidConversionToInteger,
    // Integer division by zero.
    IntegerDivideByZero,
    // Out of bounds on wasm memory accesses and asm.js SIMD/atomic accesses.
    OutOfBounds,
    // Bad signature for an indirect call.
    BadIndirectCall,

    // (asm.js only) SIMD float to int conversion failed because the input
    // wasn't in bounds.
    ImpreciseSimdConversion,

    Limit
};

// A wasm::JumpTarget represents one of a special set of stubs that can be
// jumped to from any function. Because wasm modules can be larger than the
// range of a plain jump, these potentially out-of-range jumps must be recorded
// and patched specially by the MacroAssembler and ModuleGenerator.

enum class JumpTarget
{
    // Traps
    Unreachable = unsigned(Trap::Unreachable),
    IntegerOverflow = unsigned(Trap::IntegerOverflow),
    InvalidConversionToInteger = unsigned(Trap::InvalidConversionToInteger),
    IntegerDivideByZero = unsigned(Trap::IntegerDivideByZero),
    OutOfBounds = unsigned(Trap::OutOfBounds),
    BadIndirectCall = unsigned(Trap::BadIndirectCall),
    ImpreciseSimdConversion = unsigned(Trap::ImpreciseSimdConversion),
    // Non-traps
    StackOverflow,
    Throw,
    Limit
};

typedef EnumeratedArray<JumpTarget, JumpTarget::Limit, Uint32Vector> JumpSiteArray;

// The CompileArgs struct captures global parameters that affect all wasm code
// generation. It also currently is the single source of truth for whether or
// not to use signal handlers for different purposes.

struct CompileArgs
{
    bool useSignalHandlersForOOB;
    bool useSignalHandlersForInterrupt;

    CompileArgs() = default;
    explicit CompileArgs(ExclusiveContext* cx);
    bool operator==(CompileArgs rhs) const;
    bool operator!=(CompileArgs rhs) const { return !(*this == rhs); }
};

// A Module can either be asm.js or wasm.

enum ModuleKind
{
    Wasm,
    AsmJS
};

// ImportExit describes the region of wasm global memory allocated for an
// import. This is accessed directly from JIT code and mutated by Instance as
// exits become optimized and deoptimized.

struct ImportExit
{
    void* code;
    jit::BaselineScript* baselineScript;
    GCPtrFunction fun;
    static_assert(sizeof(GCPtrFunction) == sizeof(void*), "for JIT access");
};

// ExportArg holds the unboxed operands to the wasm entry trampoline which can
// be called through an ExportFuncPtr.

struct ExportArg
{
    uint64_t lo;
    uint64_t hi;
};

typedef int32_t (*ExportFuncPtr)(ExportArg* args, uint8_t* global);

// Constants:

static const unsigned ActivationGlobalDataOffset = 0;
static const unsigned HeapGlobalDataOffset       = ActivationGlobalDataOffset + sizeof(void*);
static const unsigned NaN64GlobalDataOffset      = HeapGlobalDataOffset + sizeof(void*);
static const unsigned NaN32GlobalDataOffset      = NaN64GlobalDataOffset + sizeof(double);
static const unsigned InitialGlobalDataBytes     = NaN32GlobalDataOffset + sizeof(float);

static const unsigned MaxSigs                    =        4 * 1024;
static const unsigned MaxFuncs                   =      512 * 1024;
static const unsigned MaxLocals                  =       64 * 1024;
static const unsigned MaxImports                 =       64 * 1024;
static const unsigned MaxExports                 =       64 * 1024;
static const unsigned MaxTables                  =        4 * 1024;
static const unsigned MaxTableElems              =      128 * 1024;
static const unsigned MaxArgsPerFunc             =        4 * 1024;
static const unsigned MaxBrTableElems            = 4 * 1024 * 1024;

} // namespace wasm
} // namespace js

#endif // wasm_types_h
