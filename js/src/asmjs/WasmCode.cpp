/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "asmjs/WasmCode.h"

#include "mozilla/Atomics.h"
#include "mozilla/EnumeratedRange.h"

#include "jsprf.h"

#include "asmjs/WasmModule.h"
#include "asmjs/WasmSerialize.h"
#include "jit/ExecutableAllocator.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#ifdef MOZ_VTUNE
# include "vtune/VTuneWrapper.h"
#endif

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using mozilla::Atomic;
using mozilla::MakeEnumeratedRange;
using JS::GenericNaN;

// Limit the number of concurrent wasm code allocations per process. Note that
// on Linux, the real maximum is ~32k, as each module requires 2 maps (RW/RX),
// and the kernel's default max_map_count is ~65k.
//
// Note: this can be removed once writable/non-executable global data stops
// being stored in the code segment.
static Atomic<uint32_t> wasmCodeAllocations(0);
static const uint32_t MaxWasmCodeAllocations = 16384;

static uint8_t*
AllocateCodeSegment(ExclusiveContext* cx, uint32_t totalLength)
{
    if (wasmCodeAllocations >= MaxWasmCodeAllocations)
        return nullptr;

    // Allocate RW memory. DynamicallyLinkModule will reprotect the code as RX.
    unsigned permissions =
        ExecutableAllocator::initialProtectionFlags(ExecutableAllocator::Writable);

    void* p = AllocateExecutableMemory(nullptr, totalLength, permissions,
                                       "wasm-code-segment", gc::SystemPageSize());
    if (!p) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    wasmCodeAllocations++;
    return (uint8_t*)p;
}

static void
StaticallyLink(CodeSegment& cs, const LinkData& linkData, ExclusiveContext* cx)
{
    for (LinkData::InternalLink link : linkData.internalLinks) {
        uint8_t* patchAt = cs.code() + link.patchAtOffset;
        void* target = cs.code() + link.targetOffset;
        if (link.isRawPointerPatch())
            *(void**)(patchAt) = target;
        else
            Assembler::PatchInstructionImmediate(patchAt, PatchedImmPtr(target));
    }

    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        const Uint32Vector& offsets = linkData.symbolicLinks[imm];
        for (size_t i = 0; i < offsets.length(); i++) {
            uint8_t* patchAt = cs.code() + offsets[i];
            void* target = AddressOf(imm, cx);
            Assembler::PatchDataWithValueCheck(CodeLocationLabel(patchAt),
                                               PatchedImmPtr(target),
                                               PatchedImmPtr((void*)-1));
        }
    }

    // Initialize data in the code segment that needs absolute addresses:

    *(double*)(cs.globalData() + NaN64GlobalDataOffset) = GenericNaN();
    *(float*)(cs.globalData() + NaN32GlobalDataOffset) = GenericNaN();

    for (const LinkData::FuncTable& table : linkData.funcTables) {
        auto array = reinterpret_cast<void**>(cs.globalData() + table.globalDataOffset);
        for (size_t i = 0; i < table.elemOffsets.length(); i++)
            array[i] = cs.code() + table.elemOffsets[i];
    }
}

static void
SpecializeToHeap(CodeSegment& cs, const Metadata& metadata, uint8_t* heapBase, uint32_t heapLength)
{
    for (const BoundsCheck& check : metadata.boundsChecks)
        Assembler::UpdateBoundsCheck(check.patchAt(cs.code()), heapLength);

#if defined(JS_CODEGEN_X86)
    for (const MemoryAccess& access : metadata.memoryAccesses) {
        // Patch memory pointer immediate.
        void* addr = access.patchHeapPtrImmAt(cs.code());
        uint32_t disp = reinterpret_cast<uint32_t>(X86Encoding::GetPointer(addr));
        MOZ_ASSERT(disp <= INT32_MAX);
        X86Encoding::SetPointer(addr, (void*)(heapBase + disp));
    }
#endif
}

static bool
SendCodeRangesToProfiler(JSContext* cx, CodeSegment& cs, const Bytes& bytecode,
                         const Metadata& metadata)
{
    bool enabled = false;
#ifdef JS_ION_PERF
    enabled |= PerfFuncEnabled();
#endif
#ifdef MOZ_VTUNE
    enabled |= IsVTuneProfilingActive();
#endif
    if (!enabled)
        return true;

    for (const CodeRange& codeRange : metadata.codeRanges) {
        if (!codeRange.isFunction())
            continue;

        uintptr_t start = uintptr_t(cs.code() + codeRange.begin());
        uintptr_t end = uintptr_t(cs.code() + codeRange.end());
        uintptr_t size = end - start;

        TwoByteName name(cx);
        if (!metadata.getFuncName(cx, &bytecode, codeRange.funcIndex(), &name))
            return false;

        UniqueChars chars(
            (char*)JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, name.begin(), name.length()).get());
        if (!chars)
            return false;

        // Avoid "unused" warnings
        (void)start;
        (void)size;

#ifdef JS_ION_PERF
        if (PerfFuncEnabled()) {
            const char* file = metadata.filename.get();
            unsigned line = codeRange.funcLineOrBytecode();
            unsigned column = 0;
            writePerfSpewerAsmJSFunctionMap(start, size, file, line, column, chars.get());
        }
#endif
#ifdef MOZ_VTUNE
        if (IsVTuneProfilingActive()) {
            unsigned method_id = iJIT_GetNewMethodID();
            if (method_id == 0)
                return true;
            iJIT_Method_Load method;
            method.method_id = method_id;
            method.method_name = chars.get();
            method.method_load_address = (void*)start;
            method.method_size = size;
            method.line_number_size = 0;
            method.line_number_table = nullptr;
            method.class_id = 0;
            method.class_file_name = nullptr;
            method.source_file_name = nullptr;
            iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&method);
        }
#endif
    }

    return true;
}

/* static */ UniqueCodeSegment
CodeSegment::create(JSContext* cx,
                    const Bytes& bytecode,
                    const LinkData& linkData,
                    const Metadata& metadata,
                    uint8_t* heapBase,
                    uint32_t heapLength)
{
    MOZ_ASSERT(bytecode.length() % gc::SystemPageSize() == 0);
    MOZ_ASSERT(linkData.globalDataLength % gc::SystemPageSize() == 0);
    MOZ_ASSERT(linkData.functionCodeLength < bytecode.length());

    auto cs = cx->make_unique<CodeSegment>();
    if (!cs)
        return nullptr;

    cs->bytes_ = AllocateCodeSegment(cx, bytecode.length() + linkData.globalDataLength);
    if (!cs->bytes_)
        return nullptr;

    cs->functionCodeLength_ = linkData.functionCodeLength;
    cs->codeLength_ = bytecode.length();
    cs->globalDataLength_ = linkData.globalDataLength;
    cs->interruptCode_ = cs->code() + linkData.interruptOffset;
    cs->outOfBoundsCode_ = cs->code() + linkData.outOfBoundsOffset;

    {
        JitContext jcx(CompileRuntime::get(cx->compartment()->runtimeFromAnyThread()));
        AutoFlushICache afc("CodeSegment::create");
        AutoFlushICache::setRange(uintptr_t(cs->code()), cs->codeLength());

        memcpy(cs->code(), bytecode.begin(), bytecode.length());
        StaticallyLink(*cs, linkData, cx);
        SpecializeToHeap(*cs, metadata, heapBase, heapLength);
    }

    if (!ExecutableAllocator::makeExecutable(cs->code(), cs->codeLength())) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    if (!SendCodeRangesToProfiler(cx, *cs, bytecode, metadata))
        return nullptr;

    return cs;
}

CodeSegment::~CodeSegment()
{
    if (!bytes_)
        return;

    MOZ_ASSERT(wasmCodeAllocations > 0);
    wasmCodeAllocations--;

    MOZ_ASSERT(totalLength() > 0);
    DeallocateExecutableMemory(bytes_, totalLength(), gc::SystemPageSize());
}

size_t
CodeSegment::serializedSize() const
{
    return sizeof(uint32_t) +
           sizeof(uint32_t) +
           codeLength_;
}

uint8_t*
CodeSegment::serialize(uint8_t* cursor) const
{
    cursor = WriteScalar<uint32_t>(cursor, codeLength_);
    cursor = WriteScalar<uint32_t>(cursor, globalDataLength_);
    cursor = WriteBytes(cursor, bytes_, codeLength_);
    return cursor;
}

const uint8_t*
CodeSegment::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    cursor = ReadScalar<uint32_t>(cursor, &codeLength_);
    cursor = ReadScalar<uint32_t>(cursor, &globalDataLength_);

    bytes_ = AllocateCodeSegment(cx, codeLength_ + globalDataLength_);
    if (!bytes_)
        return nullptr;

    cursor = ReadBytes(cursor, bytes_, codeLength_);
    return cursor;
}

static size_t
SerializedSigSize(const Sig& sig)
{
    return sizeof(ExprType) +
           SerializedPodVectorSize(sig.args());
}

static uint8_t*
SerializeSig(uint8_t* cursor, const Sig& sig)
{
    cursor = WriteScalar<ExprType>(cursor, sig.ret());
    cursor = SerializePodVector(cursor, sig.args());
    return cursor;
}

static const uint8_t*
DeserializeSig(ExclusiveContext* cx, const uint8_t* cursor, Sig* sig)
{
    ExprType ret;
    cursor = ReadScalar<ExprType>(cursor, &ret);

    ValTypeVector args;
    cursor = DeserializePodVector(cx, cursor, &args);
    if (!cursor)
        return nullptr;

    *sig = Sig(Move(args), ret);
    return cursor;
}

static size_t
SizeOfSigExcludingThis(const Sig& sig, MallocSizeOf mallocSizeOf)
{
    return sig.args().sizeOfExcludingThis(mallocSizeOf);
}

size_t
Export::serializedSize() const
{
    return SerializedSigSize(sig_) +
           sizeof(pod);
}

uint8_t*
Export::serialize(uint8_t* cursor) const
{
    cursor = SerializeSig(cursor, sig_);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

const uint8_t*
Export::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = DeserializeSig(cx, cursor, &sig_)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

size_t
Export::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return SizeOfSigExcludingThis(sig_, mallocSizeOf);
}

size_t
Import::serializedSize() const
{
    return SerializedSigSize(sig_) +
           sizeof(pod);
}

uint8_t*
Import::serialize(uint8_t* cursor) const
{
    cursor = SerializeSig(cursor, sig_);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

const uint8_t*
Import::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = DeserializeSig(cx, cursor, &sig_)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

size_t
Import::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return SizeOfSigExcludingThis(sig_, mallocSizeOf);
}

CodeRange::CodeRange(Kind kind, Offsets offsets)
  : begin_(offsets.begin),
    profilingReturn_(0),
    end_(offsets.end),
    funcIndex_(0),
    funcLineOrBytecode_(0),
    funcBeginToTableEntry_(0),
    funcBeginToTableProfilingJump_(0),
    funcBeginToNonProfilingEntry_(0),
    funcProfilingJumpToProfilingReturn_(0),
    funcProfilingEpilogueToProfilingReturn_(0),
    kind_(kind)
{
    MOZ_ASSERT(begin_ <= end_);
    MOZ_ASSERT(kind_ == Entry || kind_ == Inline || kind_ == CallThunk);
}

CodeRange::CodeRange(Kind kind, ProfilingOffsets offsets)
  : begin_(offsets.begin),
    profilingReturn_(offsets.profilingReturn),
    end_(offsets.end),
    funcIndex_(0),
    funcLineOrBytecode_(0),
    funcBeginToTableEntry_(0),
    funcBeginToTableProfilingJump_(0),
    funcBeginToNonProfilingEntry_(0),
    funcProfilingJumpToProfilingReturn_(0),
    funcProfilingEpilogueToProfilingReturn_(0),
    kind_(kind)
{
    MOZ_ASSERT(begin_ < profilingReturn_);
    MOZ_ASSERT(profilingReturn_ < end_);
    MOZ_ASSERT(kind_ == ImportJitExit || kind_ == ImportInterpExit);
}

CodeRange::CodeRange(uint32_t funcIndex, uint32_t funcLineOrBytecode, FuncOffsets offsets)
  : begin_(offsets.begin),
    profilingReturn_(offsets.profilingReturn),
    end_(offsets.end),
    funcIndex_(funcIndex),
    funcLineOrBytecode_(funcLineOrBytecode),
    funcBeginToTableEntry_(offsets.tableEntry - begin_),
    funcBeginToTableProfilingJump_(offsets.tableProfilingJump - begin_),
    funcBeginToNonProfilingEntry_(offsets.nonProfilingEntry - begin_),
    funcProfilingJumpToProfilingReturn_(profilingReturn_ - offsets.profilingJump),
    funcProfilingEpilogueToProfilingReturn_(profilingReturn_ - offsets.profilingEpilogue),
    kind_(Function)
{
    MOZ_ASSERT(begin_ < profilingReturn_);
    MOZ_ASSERT(profilingReturn_ < end_);
    MOZ_ASSERT(funcBeginToTableEntry_ == offsets.tableEntry - begin_);
    MOZ_ASSERT(funcBeginToTableProfilingJump_ == offsets.tableProfilingJump - begin_);
    MOZ_ASSERT(funcBeginToNonProfilingEntry_ == offsets.nonProfilingEntry - begin_);
    MOZ_ASSERT(funcProfilingJumpToProfilingReturn_ == profilingReturn_ - offsets.profilingJump);
    MOZ_ASSERT(funcProfilingEpilogueToProfilingReturn_ == profilingReturn_ - offsets.profilingEpilogue);
}

static size_t
StringLengthWithNullChar(const char* chars)
{
    return chars ? strlen(chars) + 1 : 0;
}

size_t
CacheableChars::serializedSize() const
{
    return sizeof(uint32_t) + StringLengthWithNullChar(get());
}

uint8_t*
CacheableChars::serialize(uint8_t* cursor) const
{
    uint32_t lengthWithNullChar = StringLengthWithNullChar(get());
    cursor = WriteScalar<uint32_t>(cursor, lengthWithNullChar);
    cursor = WriteBytes(cursor, get(), lengthWithNullChar);
    return cursor;
}

const uint8_t*
CacheableChars::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    uint32_t lengthWithNullChar;
    cursor = ReadBytes(cursor, &lengthWithNullChar, sizeof(uint32_t));

    if (lengthWithNullChar) {
        reset(cx->pod_malloc<char>(lengthWithNullChar));
        if (!get())
            return nullptr;

        cursor = ReadBytes(cursor, get(), lengthWithNullChar);
    } else {
        MOZ_ASSERT(!get());
    }

    return cursor;
}

size_t
CacheableChars::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(get());
}

size_t
Metadata::serializedSize() const
{
    return sizeof(pod()) +
           SerializedVectorSize(imports) +
           SerializedVectorSize(exports) +
           SerializedPodVectorSize(memoryAccesses) +
           SerializedPodVectorSize(boundsChecks) +
           SerializedPodVectorSize(codeRanges) +
           SerializedPodVectorSize(callSites) +
           SerializedPodVectorSize(callThunks) +
           SerializedPodVectorSize(funcNames) +
           filename.serializedSize();
}

uint8_t*
Metadata::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
    cursor = SerializeVector(cursor, imports);
    cursor = SerializeVector(cursor, exports);
    cursor = SerializePodVector(cursor, memoryAccesses);
    cursor = SerializePodVector(cursor, boundsChecks);
    cursor = SerializePodVector(cursor, codeRanges);
    cursor = SerializePodVector(cursor, callSites);
    cursor = SerializePodVector(cursor, callThunks);
    cursor = SerializePodVector(cursor, funcNames);
    cursor = filename.serialize(cursor);
    return cursor;
}

/* static */ const uint8_t*
Metadata::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
    (cursor = DeserializeVector(cx, cursor, &imports)) &&
    (cursor = DeserializeVector(cx, cursor, &exports)) &&
    (cursor = DeserializePodVector(cx, cursor, &memoryAccesses)) &&
    (cursor = DeserializePodVector(cx, cursor, &boundsChecks)) &&
    (cursor = DeserializePodVector(cx, cursor, &codeRanges)) &&
    (cursor = DeserializePodVector(cx, cursor, &callSites)) &&
    (cursor = DeserializePodVector(cx, cursor, &callThunks)) &&
    (cursor = DeserializePodVector(cx, cursor, &funcNames)) &&
    (cursor = filename.deserialize(cx, cursor));
    return cursor;
}

size_t
Metadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return SizeOfVectorExcludingThis(imports, mallocSizeOf) +
           SizeOfVectorExcludingThis(exports, mallocSizeOf) +
           memoryAccesses.sizeOfExcludingThis(mallocSizeOf) +
           boundsChecks.sizeOfExcludingThis(mallocSizeOf) +
           codeRanges.sizeOfExcludingThis(mallocSizeOf) +
           callSites.sizeOfExcludingThis(mallocSizeOf) +
           callThunks.sizeOfExcludingThis(mallocSizeOf) +
           funcNames.sizeOfExcludingThis(mallocSizeOf) +
           filename.sizeOfExcludingThis(mallocSizeOf);
}

bool
Metadata::getFuncName(JSContext* cx, const Bytes* maybeBytecode, uint32_t funcIndex,
                      TwoByteName* name) const
{
    if (funcIndex < funcNames.length()) {
        MOZ_ASSERT(maybeBytecode, "NameInBytecode requires preserved bytecode");

        const NameInBytecode& n = funcNames[funcIndex];
        MOZ_ASSERT(n.offset + n.length < maybeBytecode->length());

        if (n.length == 0)
            goto invalid;

        UTF8Chars utf8((const char*)maybeBytecode->begin() + n.offset, n.length);

        // This code could be optimized by having JS::UTF8CharsToNewTwoByteCharsZ
        // return a Vector directly.
        size_t twoByteLength;
        UniqueTwoByteChars chars(JS::UTF8CharsToNewTwoByteCharsZ(cx, utf8, &twoByteLength).get());
        if (!chars)
            goto invalid;

        if (!name->growByUninitialized(twoByteLength))
            return false;

        PodCopy(name->begin(), chars.get(), twoByteLength);
        return true;
    }

  invalid:

    // For names that are out of range or invalid, synthesize a name.

    UniqueChars chars(JS_smprintf("wasm-function[%u]", funcIndex));
    if (!chars) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (!name->growByUninitialized(strlen(chars.get())))
        return false;

    CopyAndInflateChars(name->begin(), chars.get(), name->length());
    return true;
}
