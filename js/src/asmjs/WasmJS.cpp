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

#include "asmjs/WasmJS.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"

#include "jsprf.h"

#include "asmjs/WasmCompile.h"
#include "asmjs/WasmInstance.h"
#include "asmjs/WasmModule.h"
#include "asmjs/WasmSignalHandlers.h"
#include "builtin/Promise.h"
#include "jit/JitOptions.h"
#include "vm/Interpreter.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using mozilla::CheckedInt;
using mozilla::IsNaN;
using mozilla::IsSame;
using mozilla::Nothing;

bool
wasm::HasCompilerSupport(ExclusiveContext* cx)
{
    if (!cx->jitSupportsFloatingPoint())
        return false;

    if (!cx->jitSupportsUnalignedAccesses())
        return false;

    if (!wasm::HaveSignalHandlers())
        return false;

#if defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_ARM64)
    return false;
#else
    return true;
#endif
}

template<typename T>
JSObject*
js::wasm::CreateCustomNaNObject(JSContext* cx, T* addr)
{
    MOZ_ASSERT(IsNaN(*addr));

    RootedObject obj(cx, JS_NewPlainObject(cx));
    if (!obj)
        return nullptr;

    int32_t* i32 = (int32_t*)addr;
    RootedValue intVal(cx, Int32Value(i32[0]));
    if (!JS_DefineProperty(cx, obj, "nan_low", intVal, JSPROP_ENUMERATE))
        return nullptr;

    if (IsSame<double, T>::value) {
        intVal = Int32Value(i32[1]);
        if (!JS_DefineProperty(cx, obj, "nan_high", intVal, JSPROP_ENUMERATE))
            return nullptr;
    }

    return obj;
}

template JSObject* js::wasm::CreateCustomNaNObject(JSContext* cx, float* addr);
template JSObject* js::wasm::CreateCustomNaNObject(JSContext* cx, double* addr);

bool
js::wasm::ReadCustomFloat32NaNObject(JSContext* cx, HandleValue v, uint32_t* ret)
{
    RootedObject obj(cx, &v.toObject());
    RootedValue val(cx);

    int32_t i32;
    if (!JS_GetProperty(cx, obj, "nan_low", &val))
        return false;
    if (!ToInt32(cx, val, &i32))
        return false;

    *ret = i32;
    return true;
}

bool
js::wasm::ReadCustomDoubleNaNObject(JSContext* cx, HandleValue v, uint64_t* ret)
{
    RootedObject obj(cx, &v.toObject());
    RootedValue val(cx);

    int32_t i32;
    if (!JS_GetProperty(cx, obj, "nan_high", &val))
        return false;
    if (!ToInt32(cx, val, &i32))
        return false;
    *ret = uint32_t(i32);
    *ret <<= 32;

    if (!JS_GetProperty(cx, obj, "nan_low", &val))
        return false;
    if (!ToInt32(cx, val, &i32))
        return false;
    *ret |= uint32_t(i32);

    return true;
}

JSObject*
wasm::CreateI64Object(JSContext* cx, int64_t i64)
{
    RootedObject result(cx, JS_NewPlainObject(cx));
    if (!result)
        return nullptr;

    RootedValue val(cx, Int32Value(uint32_t(i64)));
    if (!JS_DefineProperty(cx, result, "low", val, JSPROP_ENUMERATE))
        return nullptr;

    val = Int32Value(uint32_t(i64 >> 32));
    if (!JS_DefineProperty(cx, result, "high", val, JSPROP_ENUMERATE))
        return nullptr;

    return result;
}

bool
wasm::ReadI64Object(JSContext* cx, HandleValue v, int64_t* i64)
{
    if (!v.isObject()) {
        JS_ReportErrorASCII(cx, "i64 JS value must be an object");
        return false;
    }

    RootedObject obj(cx, &v.toObject());

    int32_t* i32 = (int32_t*)i64;

    RootedValue val(cx);
    if (!JS_GetProperty(cx, obj, "low", &val))
        return false;
    if (!ToInt32(cx, val, &i32[0]))
        return false;

    if (!JS_GetProperty(cx, obj, "high", &val))
        return false;
    if (!ToInt32(cx, val, &i32[1]))
        return false;

    return true;
}

// ============================================================================
// (Temporary) Wasm class and static methods

static bool
ThrowBadImportArg(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_IMPORT_ARG);
    return false;
}

static bool
ThrowBadImportField(JSContext* cx, const char* str)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_IMPORT_FIELD, str);
    return false;
}

static bool
GetProperty(JSContext* cx, HandleObject obj, const char* chars, MutableHandleValue v)
{
    JSAtom* atom = AtomizeUTF8Chars(cx, chars, strlen(chars));
    if (!atom)
        return false;

    RootedId id(cx, AtomToId(atom));
    return GetProperty(cx, obj, obj, id, v);
}

static bool
GetImports(JSContext* cx,
           const Module& module,
           HandleObject importObj,
           MutableHandle<FunctionVector> funcImports,
           MutableHandleWasmTableObject tableImport,
           MutableHandleWasmMemoryObject memoryImport,
           ValVector* globalImports)
{
    const ImportVector& imports = module.imports();
    if (!imports.empty() && !importObj)
        return ThrowBadImportArg(cx);

    const Metadata& metadata = module.metadata();

    uint32_t globalIndex = 0;
    const GlobalDescVector& globals = metadata.globals;
    for (const Import& import : imports) {
        RootedValue v(cx);
        if (!GetProperty(cx, importObj, import.module.get(), &v))
            return false;

        if (!v.isObject())
            return ThrowBadImportField(cx, "an Object");

        RootedObject obj(cx, &v.toObject());
        if (!GetProperty(cx, obj, import.func.get(), &v))
            return false;

        switch (import.kind) {
          case DefinitionKind::Function:
            if (!IsFunctionObject(v))
                return ThrowBadImportField(cx, "a Function");

            if (!funcImports.append(&v.toObject().as<JSFunction>()))
                return false;

            break;
          case DefinitionKind::Table:
            if (!v.isObject() || !v.toObject().is<WasmTableObject>())
                return ThrowBadImportField(cx, "a Table");

            MOZ_ASSERT(!tableImport);
            tableImport.set(&v.toObject().as<WasmTableObject>());
            break;
          case DefinitionKind::Memory:
            if (!v.isObject() || !v.toObject().is<WasmMemoryObject>())
                return ThrowBadImportField(cx, "a Memory");

            MOZ_ASSERT(!memoryImport);
            memoryImport.set(&v.toObject().as<WasmMemoryObject>());
            break;

          case DefinitionKind::Global:
            Val val;
            const GlobalDesc& global = globals[globalIndex++];
            MOZ_ASSERT(global.importIndex() == globalIndex - 1);
            MOZ_ASSERT(!global.isMutable());
            switch (global.type()) {
              case ValType::I32: {
                int32_t i32;
                if (!ToInt32(cx, v, &i32))
                    return false;
                val = Val(uint32_t(i32));
                break;
              }
              case ValType::I64: {
                MOZ_ASSERT(JitOptions.wasmTestMode, "no int64 in JS");
                int64_t i64;
                if (!ReadI64Object(cx, v, &i64))
                    return false;
                val = Val(uint64_t(i64));
                break;
              }
              case ValType::F32: {
                if (JitOptions.wasmTestMode && v.isObject()) {
                    uint32_t bits;
                    if (!ReadCustomFloat32NaNObject(cx, v, &bits))
                        return false;
                    val = Val(RawF32::fromBits(bits));
                    break;
                }
                double d;
                if (!ToNumber(cx, v, &d))
                    return false;
                val = Val(RawF32(float(d)));
                break;
              }
              case ValType::F64: {
                if (JitOptions.wasmTestMode && v.isObject()) {
                    uint64_t bits;
                    if (!ReadCustomDoubleNaNObject(cx, v, &bits))
                        return false;
                    val = Val(RawF64::fromBits(bits));
                    break;
                }
                double d;
                if (!ToNumber(cx, v, &d))
                    return false;
                val = Val(RawF64(d));
                break;
              }
              default: {
                MOZ_CRASH("unexpected import value type");
              }
            }
            if (!globalImports->append(val))
                return false;
        }
    }

    MOZ_ASSERT(globalIndex == globals.length() || !globals[globalIndex].isImport());

    return true;
}

static bool
DescribeScriptedCaller(JSContext* cx, ScriptedCaller* scriptedCaller)
{
    // Note: JS::DescribeScriptedCaller returns whether a scripted caller was
    // found, not whether an error was thrown. This wrapper function converts
    // back to the more ordinary false-if-error form.

    JS::AutoFilename af;
    if (JS::DescribeScriptedCaller(cx, &af, &scriptedCaller->line, &scriptedCaller->column)) {
        scriptedCaller->filename = DuplicateString(cx, af.get());
        if (!scriptedCaller->filename)
            return false;
    }

    return true;
}

bool
wasm::Eval(JSContext* cx, Handle<TypedArrayObject*> code, HandleObject importObj,
           MutableHandleWasmInstanceObject instanceObj)
{
    if (!GlobalObject::ensureConstructor(cx, cx->global(), JSProto_WebAssembly))
        return false;

    MutableBytes bytecode = cx->new_<ShareableBytes>();
    if (!bytecode)
        return false;

    if (!bytecode->append((uint8_t*)code->viewDataEither().unwrap(), code->byteLength())) {
        ReportOutOfMemory(cx);
        return false;
    }

    ScriptedCaller scriptedCaller;
    if (!DescribeScriptedCaller(cx, &scriptedCaller))
        return false;

    CompileArgs compileArgs;
    if (!compileArgs.initFromContext(cx, Move(scriptedCaller)))
        return false;

    UniqueChars error;
    SharedModule module = Compile(*bytecode, compileArgs, &error);
    if (!module) {
        if (error) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_COMPILE_ERROR,
                                      error.get());
            return false;
        }
        ReportOutOfMemory(cx);
        return false;
    }

    Rooted<FunctionVector> funcs(cx, FunctionVector(cx));
    RootedWasmTableObject table(cx);
    RootedWasmMemoryObject memory(cx);
    ValVector globals;
    if (!GetImports(cx, *module, importObj, &funcs, &table, &memory, &globals))
        return false;

    return module->instantiate(cx, funcs, table, memory, globals, nullptr, instanceObj);
}

#if JS_HAS_TOSOURCE
static bool
wasm_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().Wasm);
    return true;
}
#endif

static const JSFunctionSpec wasm_static_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,     wasm_toSource,     0, 0),
#endif
    JS_FS_END
};

const Class js::WasmClass = {
    js_Wasm_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Wasm)
};

JSObject*
js::InitWasmClass(JSContext* cx, HandleObject global)
{
    MOZ_RELEASE_ASSERT(HasCompilerSupport(cx));
    MOZ_ASSERT(cx->options().wasm());

    RootedObject proto(cx, global->as<GlobalObject>().getOrCreateObjectPrototype(cx));
    if (!proto)
        return nullptr;

    RootedObject Wasm(cx, NewObjectWithGivenProto(cx, &WasmClass, proto, SingletonObject));
    if (!Wasm)
        return nullptr;

    if (!JS_DefineProperty(cx, global, js_Wasm_str, Wasm, JSPROP_RESOLVING))
        return nullptr;

    RootedValue version(cx, Int32Value(EncodingVersion));
    if (!JS_DefineProperty(cx, Wasm, "experimentalVersion", version, JSPROP_RESOLVING))
        return nullptr;

    if (!JS_DefineFunctions(cx, Wasm, wasm_static_methods))
        return nullptr;

    global->as<GlobalObject>().setConstructor(JSProto_Wasm, ObjectValue(*Wasm));
    return Wasm;
}

// ============================================================================
// Common functions

static bool
ToNonWrappingUint32(JSContext* cx, HandleValue v, uint32_t max, const char* kind, const char* noun,
                    uint32_t* u32)
{
    double dbl;
    if (!ToInteger(cx, v, &dbl))
        return false;

    if (dbl < 0 || dbl > max) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_UINT32,
                                  kind, noun);
        return false;
    }

    *u32 = uint32_t(dbl);
    MOZ_ASSERT(double(*u32) == dbl);
    return true;
}

static bool
GetResizableLimits(JSContext* cx, HandleObject obj, uint32_t max, const char* kind,
                   ResizableLimits* limits)
{
    JSAtom* initialAtom = Atomize(cx, "initial", strlen("initial"));
    if (!initialAtom)
        return false;
    RootedId initialId(cx, AtomToId(initialAtom));

    RootedValue initialVal(cx);
    if (!GetProperty(cx, obj, obj, initialId, &initialVal))
        return false;

    if (!ToNonWrappingUint32(cx, initialVal, max, kind, "initial size", &limits->initial))
        return false;

    JSAtom* maximumAtom = Atomize(cx, "maximum", strlen("maximum"));
    if (!maximumAtom)
        return false;
    RootedId maximumId(cx, AtomToId(maximumAtom));

    bool found;
    if (HasProperty(cx, obj, maximumId, &found) && found) {
        RootedValue maxVal(cx);
        if (!GetProperty(cx, obj, obj, maximumId, &maxVal))
            return false;

        limits->maximum.emplace();
        if (!ToNonWrappingUint32(cx, maxVal, max, kind, "maximum size", limits->maximum.ptr()))
            return false;

        if (limits->initial > *limits->maximum) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_UINT32,
                                      kind, "maximum size");
            return false;
        }
    }

    return true;
}

// ============================================================================
// WebAssembly.Module class and methods

const ClassOps WasmModuleObject::classOps_ =
{
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    WasmModuleObject::finalize
};

const Class WasmModuleObject::class_ =
{
    "WebAssembly.Module",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(WasmModuleObject::RESERVED_SLOTS) |
    JSCLASS_FOREGROUND_FINALIZE,
    &WasmModuleObject::classOps_,
};

const JSPropertySpec WasmModuleObject::properties[] =
{ JS_PS_END };

const JSFunctionSpec WasmModuleObject::methods[] =
{ JS_FS_END };

/* static */ void
WasmModuleObject::finalize(FreeOp* fop, JSObject* obj)
{
    obj->as<WasmModuleObject>().module().Release();
}

/* static */ WasmModuleObject*
WasmModuleObject::create(ExclusiveContext* cx, Module& module, HandleObject proto)
{
    AutoSetNewObjectMetadata metadata(cx);
    auto* obj = NewObjectWithGivenProto<WasmModuleObject>(cx, proto);
    if (!obj)
        return nullptr;

    obj->initReservedSlot(MODULE_SLOT, PrivateValue(&module));
    module.AddRef();
    return obj;
}

static bool
GetCompileArgs(JSContext* cx, CallArgs callArgs, const char* name, MutableBytes* bytecode,
               CompileArgs* compileArgs)
{
    if (!callArgs.requireAtLeast(cx, name, 1))
        return false;

    if (!callArgs[0].isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_BUF_ARG);
        return false;
    }

    *bytecode = cx->new_<ShareableBytes>();
    if (!*bytecode)
        return false;

    JSObject* unwrapped = CheckedUnwrap(&callArgs[0].toObject());
    if (unwrapped && unwrapped->is<TypedArrayObject>()) {
        TypedArrayObject& view = unwrapped->as<TypedArrayObject>();
        if (!(*bytecode)->append((uint8_t*)view.viewDataEither().unwrap(), view.byteLength()))
            return false;
    } else if (unwrapped && unwrapped->is<ArrayBufferObject>()) {
        ArrayBufferObject& buffer = unwrapped->as<ArrayBufferObject>();
        if (!(*bytecode)->append(buffer.dataPointer(), buffer.byteLength()))
            return false;
    } else {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_BUF_ARG);
        return false;
    }

    ScriptedCaller scriptedCaller;
    if (!DescribeScriptedCaller(cx, &scriptedCaller))
        return false;

    if (!compileArgs->initFromContext(cx, Move(scriptedCaller)))
        return false;

    return true;
}

/* static */ bool
WasmModuleObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs callArgs = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, callArgs, "Module"))
        return false;

    MutableBytes bytecode;
    CompileArgs compileArgs;
    if (!GetCompileArgs(cx, callArgs, "WebAssembly.Module", &bytecode, &compileArgs))
        return false;

    UniqueChars error;
    SharedModule module = Compile(*bytecode, compileArgs, &error);
    if (!module) {
        if (error) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_COMPILE_ERROR,
                                      error.get());
            return false;
        }
        ReportOutOfMemory(cx);
        return false;
    }

    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmModule).toObject());
    RootedObject moduleObj(cx, WasmModuleObject::create(cx, *module, proto));
    if (!moduleObj)
        return false;

    callArgs.rval().setObject(*moduleObj);
    return true;
}

Module&
WasmModuleObject::module() const
{
    MOZ_ASSERT(is<WasmModuleObject>());
    return *(Module*)getReservedSlot(MODULE_SLOT).toPrivate();
}

// ============================================================================
// WebAssembly.Instance class and methods

const ClassOps WasmInstanceObject::classOps_ =
{
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    WasmInstanceObject::finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    WasmInstanceObject::trace
};

const Class WasmInstanceObject::class_ =
{
    "WebAssembly.Instance",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(WasmInstanceObject::RESERVED_SLOTS) |
    JSCLASS_FOREGROUND_FINALIZE,
    &WasmInstanceObject::classOps_,
};

const JSPropertySpec WasmInstanceObject::properties[] =
{ JS_PS_END };

const JSFunctionSpec WasmInstanceObject::methods[] =
{ JS_FS_END };

bool
WasmInstanceObject::isNewborn() const
{
    MOZ_ASSERT(is<WasmInstanceObject>());
    return getReservedSlot(INSTANCE_SLOT).isUndefined();
}

/* static */ void
WasmInstanceObject::finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(&obj->as<WasmInstanceObject>().exports());
    if (!obj->as<WasmInstanceObject>().isNewborn())
        fop->delete_(&obj->as<WasmInstanceObject>().instance());
}

/* static */ void
WasmInstanceObject::trace(JSTracer* trc, JSObject* obj)
{
    if (!obj->as<WasmInstanceObject>().isNewborn())
        obj->as<WasmInstanceObject>().instance().tracePrivate(trc);
}

/* static */ WasmInstanceObject*
WasmInstanceObject::create(JSContext* cx,
                           UniqueCode code,
                           HandleWasmMemoryObject memory,
                           SharedTableVector&& tables,
                           Handle<FunctionVector> funcImports,
                           const ValVector& globalImports,
                           HandleObject proto)
{
    UniquePtr<WeakExportMap> exports = js::MakeUnique<WeakExportMap>(cx->zone(), ExportMap());
    if (!exports || !exports->init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    AutoSetNewObjectMetadata metadata(cx);
    RootedWasmInstanceObject obj(cx, NewObjectWithGivenProto<WasmInstanceObject>(cx, proto));
    if (!obj)
        return nullptr;

    obj->setReservedSlot(EXPORTS_SLOT, PrivateValue(exports.release()));
    MOZ_ASSERT(obj->isNewborn());

    MOZ_ASSERT(obj->isTenured(), "assumed by WasmTableObject write barriers");

    // Root the Instance via WasmInstanceObject before any possible GC.
    auto* instance = cx->new_<Instance>(cx,
                                        obj,
                                        Move(code),
                                        memory,
                                        Move(tables),
                                        funcImports,
                                        globalImports);
    if (!instance)
        return nullptr;

    obj->initReservedSlot(INSTANCE_SLOT, PrivateValue(instance));
    MOZ_ASSERT(!obj->isNewborn());

    if (!instance->init(cx))
        return nullptr;

    return obj;
}

/* static */ bool
WasmInstanceObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Instance"))
        return false;

    if (!args.requireAtLeast(cx, "WebAssembly.Instance", 1))
        return false;

    if (!args.get(0).isObject() || !args[0].toObject().is<WasmModuleObject>()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_MOD_ARG);
        return false;
    }

    const Module& module = args[0].toObject().as<WasmModuleObject>().module();

    RootedObject importObj(cx);
    if (!args.get(1).isUndefined()) {
        if (!args[1].isObject())
            return ThrowBadImportArg(cx);
        importObj = &args[1].toObject();
    }

    Rooted<FunctionVector> funcs(cx, FunctionVector(cx));
    RootedWasmTableObject table(cx);
    RootedWasmMemoryObject memory(cx);
    ValVector globals;
    if (!GetImports(cx, module, importObj, &funcs, &table, &memory, &globals))
        return false;

    RootedObject instanceProto(cx, &cx->global()->getPrototype(JSProto_WasmInstance).toObject());
    RootedWasmInstanceObject instanceObj(cx);
    if (!module.instantiate(cx, funcs, table, memory, globals, instanceProto, &instanceObj))
        return false;

    args.rval().setObject(*instanceObj);
    return true;
}

Instance&
WasmInstanceObject::instance() const
{
    MOZ_ASSERT(!isNewborn());
    return *(Instance*)getReservedSlot(INSTANCE_SLOT).toPrivate();
}

WasmInstanceObject::WeakExportMap&
WasmInstanceObject::exports() const
{
    return *(WeakExportMap*)getReservedSlot(EXPORTS_SLOT).toPrivate();
}

static bool
WasmCall(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedFunction callee(cx, &args.callee().as<JSFunction>());

    Instance& instance = ExportedFunctionToInstance(callee);
    uint32_t funcDefIndex = ExportedFunctionToDefinitionIndex(callee);
    return instance.callExport(cx, funcDefIndex, args);
}

/* static */ bool
WasmInstanceObject::getExportedFunction(JSContext* cx, HandleWasmInstanceObject instanceObj,
                                        uint32_t funcDefIndex, MutableHandleFunction fun)
{
    if (ExportMap::Ptr p = instanceObj->exports().lookup(funcDefIndex)) {
        fun.set(p->value());
        return true;
    }

    const Instance& instance = instanceObj->instance();
    RootedAtom name(cx, instance.code().getFuncDefAtom(cx, funcDefIndex));
    if (!name)
        return false;

    unsigned numArgs = instance.metadata().lookupFuncDefExport(funcDefIndex).sig().args().length();
    fun.set(NewNativeConstructor(cx, WasmCall, numArgs, name, gc::AllocKind::FUNCTION_EXTENDED,
                                 GenericObject, JSFunction::ASMJS_CTOR));
    if (!fun)
        return false;

    fun->setExtendedSlot(FunctionExtended::WASM_INSTANCE_SLOT, ObjectValue(*instanceObj));
    fun->setExtendedSlot(FunctionExtended::WASM_FUNC_DEF_INDEX_SLOT, Int32Value(funcDefIndex));

    if (!instanceObj->exports().putNew(funcDefIndex, fun)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

const CodeRange&
WasmInstanceObject::getExportedFunctionCodeRange(HandleFunction fun)
{
    uint32_t funcDefIndex = ExportedFunctionToDefinitionIndex(fun);
    MOZ_ASSERT(exports().lookup(funcDefIndex)->value() == fun);
    const Metadata& metadata = instance().metadata();
    return metadata.codeRanges[metadata.lookupFuncDefExport(funcDefIndex).codeRangeIndex()];
}

bool
wasm::IsExportedFunction(JSFunction* fun)
{
    return fun->maybeNative() == WasmCall;
}

bool
wasm::IsExportedWasmFunction(JSFunction* fun)
{
    return IsExportedFunction(fun) && !ExportedFunctionToInstance(fun).isAsmJS();
}

bool
wasm::IsExportedFunction(const Value& v, MutableHandleFunction f)
{
    if (!v.isObject())
        return false;

    JSObject& obj = v.toObject();
    if (!obj.is<JSFunction>() || !IsExportedFunction(&obj.as<JSFunction>()))
        return false;

    f.set(&obj.as<JSFunction>());
    return true;
}

Instance&
wasm::ExportedFunctionToInstance(JSFunction* fun)
{
    return ExportedFunctionToInstanceObject(fun)->instance();
}

WasmInstanceObject*
wasm::ExportedFunctionToInstanceObject(JSFunction* fun)
{
    MOZ_ASSERT(IsExportedFunction(fun));
    const Value& v = fun->getExtendedSlot(FunctionExtended::WASM_INSTANCE_SLOT);
    return &v.toObject().as<WasmInstanceObject>();
}

uint32_t
wasm::ExportedFunctionToDefinitionIndex(JSFunction* fun)
{
    MOZ_ASSERT(IsExportedFunction(fun));
    const Value& v = fun->getExtendedSlot(FunctionExtended::WASM_FUNC_DEF_INDEX_SLOT);
    return v.toInt32();
}

// ============================================================================
// WebAssembly.Memory class and methods

const ClassOps WasmMemoryObject::classOps_ =
{
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    WasmMemoryObject::finalize
};

const Class WasmMemoryObject::class_ =
{
    "WebAssembly.Memory",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(WasmMemoryObject::RESERVED_SLOTS) |
    JSCLASS_FOREGROUND_FINALIZE,
    &WasmMemoryObject::classOps_
};

/* static */ void
WasmMemoryObject::finalize(FreeOp* fop, JSObject* obj)
{
    WasmMemoryObject& memory = obj->as<WasmMemoryObject>();
    if (memory.hasObservers())
        fop->delete_(&memory.observers());
}

/* static */ WasmMemoryObject*
WasmMemoryObject::create(ExclusiveContext* cx, HandleArrayBufferObjectMaybeShared buffer,
                         HandleObject proto)
{
    AutoSetNewObjectMetadata metadata(cx);
    auto* obj = NewObjectWithGivenProto<WasmMemoryObject>(cx, proto);
    if (!obj)
        return nullptr;

    obj->initReservedSlot(BUFFER_SLOT, ObjectValue(*buffer));
    MOZ_ASSERT(!obj->hasObservers());
    return obj;
}

/* static */ bool
WasmMemoryObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Memory"))
        return false;

    if (!args.requireAtLeast(cx, "WebAssembly.Memory", 1))
        return false;

    if (!args.get(0).isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_DESC_ARG, "memory");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());
    ResizableLimits limits;
    if (!GetResizableLimits(cx, obj, UINT32_MAX / PageSize, "Memory", &limits))
        return false;

    limits.initial *= PageSize;
    if (limits.maximum)
        limits.maximum = Some(*limits.maximum * PageSize);

    RootedArrayBufferObject buffer(cx,
        ArrayBufferObject::createForWasm(cx, limits.initial, limits.maximum));
    if (!buffer)
        return false;

    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
    RootedWasmMemoryObject memoryObj(cx, WasmMemoryObject::create(cx, buffer, proto));
    if (!memoryObj)
        return false;

    args.rval().setObject(*memoryObj);
    return true;
}

static bool
IsMemory(HandleValue v)
{
    return v.isObject() && v.toObject().is<WasmMemoryObject>();
}

/* static */ bool
WasmMemoryObject::bufferGetterImpl(JSContext* cx, const CallArgs& args)
{
    args.rval().setObject(args.thisv().toObject().as<WasmMemoryObject>().buffer());
    return true;
}

/* static */ bool
WasmMemoryObject::bufferGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsMemory, bufferGetterImpl>(cx, args);
}

const JSPropertySpec WasmMemoryObject::properties[] =
{
    JS_PSG("buffer", WasmMemoryObject::bufferGetter, 0),
    JS_PS_END
};

/* static */ bool
WasmMemoryObject::growImpl(JSContext* cx, const CallArgs& args)
{
    RootedWasmMemoryObject memory(cx, &args.thisv().toObject().as<WasmMemoryObject>());

    uint32_t delta;
    if (!ToNonWrappingUint32(cx, args.get(0), UINT32_MAX, "Memory", "grow delta", &delta))
        return false;

    uint32_t ret = grow(memory, delta, cx);

    if (ret == uint32_t(-1)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "memory");
        return false;
    }

    args.rval().setInt32(ret);
    return true;
}

/* static */ bool
WasmMemoryObject::grow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsMemory, growImpl>(cx, args);
}

const JSFunctionSpec WasmMemoryObject::methods[] =
{
    JS_FN("grow", WasmMemoryObject::grow, 1, 0),
    JS_FS_END
};

ArrayBufferObjectMaybeShared&
WasmMemoryObject::buffer() const
{
    return getReservedSlot(BUFFER_SLOT).toObject().as<ArrayBufferObjectMaybeShared>();
}

bool
WasmMemoryObject::hasObservers() const
{
    return !getReservedSlot(OBSERVERS_SLOT).isUndefined();
}

WasmMemoryObject::WeakInstanceSet&
WasmMemoryObject::observers() const
{
    MOZ_ASSERT(hasObservers());
    return *reinterpret_cast<WeakInstanceSet*>(getReservedSlot(OBSERVERS_SLOT).toPrivate());
}

WasmMemoryObject::WeakInstanceSet*
WasmMemoryObject::getOrCreateObservers(JSContext* cx)
{
    if (!hasObservers()) {
        auto observers = MakeUnique<WeakInstanceSet>(cx->zone(), InstanceSet());
        if (!observers || !observers->init()) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        setReservedSlot(OBSERVERS_SLOT, PrivateValue(observers.release()));
    }

    return &observers();
}

bool
WasmMemoryObject::movingGrowable() const
{
#ifdef WASM_HUGE_MEMORY
    return false;
#else
    return !buffer().wasmMaxSize();
#endif
}

bool
WasmMemoryObject::addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance)
{
    MOZ_ASSERT(movingGrowable());

    WeakInstanceSet* observers = getOrCreateObservers(cx);
    if (!observers)
        return false;

    if (!observers->putNew(instance)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

/* static */ uint32_t
WasmMemoryObject::grow(HandleWasmMemoryObject memory, uint32_t delta, JSContext* cx)
{
    RootedArrayBufferObject oldBuf(cx, &memory->buffer().as<ArrayBufferObject>());

    MOZ_ASSERT(oldBuf->byteLength() % PageSize == 0);
    uint32_t oldNumPages = oldBuf->byteLength() / PageSize;

    CheckedInt<uint32_t> newSize = oldNumPages;
    newSize += delta;
    newSize *= PageSize;
    if (!newSize.isValid())
        return -1;

    RootedArrayBufferObject newBuf(cx);
    uint8_t* prevMemoryBase = nullptr;

    if (Maybe<uint32_t> maxSize = oldBuf->wasmMaxSize()) {
        if (newSize.value() > maxSize.value())
            return -1;

        if (!ArrayBufferObject::wasmGrowToSizeInPlace(newSize.value(), oldBuf, &newBuf, cx))
            return -1;
    } else {
#ifdef WASM_HUGE_MEMORY
        if (!ArrayBufferObject::wasmGrowToSizeInPlace(newSize.value(), oldBuf, &newBuf, cx))
            return -1;
#else
        MOZ_ASSERT(memory->movingGrowable());
        prevMemoryBase = oldBuf->dataPointer();
        if (!ArrayBufferObject::wasmMovingGrowToSize(newSize.value(), oldBuf, &newBuf, cx))
            return -1;
#endif
    }

    memory->setReservedSlot(BUFFER_SLOT, ObjectValue(*newBuf));

    // Only notify moving-grow-observers after the BUFFER_SLOT has been updated
    // since observers will call buffer().
    if (memory->hasObservers()) {
        MOZ_ASSERT(prevMemoryBase);
        for (InstanceSet::Range r = memory->observers().all(); !r.empty(); r.popFront())
            r.front()->instance().onMovingGrowMemory(prevMemoryBase);
    }

    return oldNumPages;
}

// ============================================================================
// WebAssembly.Table class and methods

const ClassOps WasmTableObject::classOps_ =
{
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    WasmTableObject::finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    WasmTableObject::trace
};

const Class WasmTableObject::class_ =
{
    "WebAssembly.Table",
    JSCLASS_DELAY_METADATA_BUILDER |
    JSCLASS_HAS_RESERVED_SLOTS(WasmTableObject::RESERVED_SLOTS) |
    JSCLASS_FOREGROUND_FINALIZE,
    &WasmTableObject::classOps_
};

bool
WasmTableObject::isNewborn() const
{
    MOZ_ASSERT(is<WasmTableObject>());
    return getReservedSlot(TABLE_SLOT).isUndefined();
}

/* static */ void
WasmTableObject::finalize(FreeOp* fop, JSObject* obj)
{
    WasmTableObject& tableObj = obj->as<WasmTableObject>();
    if (!tableObj.isNewborn())
        tableObj.table().Release();
}

/* static */ void
WasmTableObject::trace(JSTracer* trc, JSObject* obj)
{
    WasmTableObject& tableObj = obj->as<WasmTableObject>();
    if (!tableObj.isNewborn())
        tableObj.table().tracePrivate(trc);
}

/* static */ WasmTableObject*
WasmTableObject::create(JSContext* cx, ResizableLimits limits)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmTable).toObject());

    AutoSetNewObjectMetadata metadata(cx);
    RootedWasmTableObject obj(cx, NewObjectWithGivenProto<WasmTableObject>(cx, proto));
    if (!obj)
        return nullptr;

    MOZ_ASSERT(obj->isNewborn());

    TableDesc td(TableKind::AnyFunction, limits);
    td.external = true;

    SharedTable table = Table::create(cx, td, obj);
    if (!table)
        return nullptr;

    obj->initReservedSlot(TABLE_SLOT, PrivateValue(table.forget().take()));

    MOZ_ASSERT(!obj->isNewborn());
    return obj;
}

/* static */ bool
WasmTableObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Table"))
        return false;

    if (!args.requireAtLeast(cx, "WebAssembly.Table", 1))
        return false;

    if (!args.get(0).isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_DESC_ARG, "table");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());

    JSAtom* elementAtom = Atomize(cx, "element", strlen("element"));
    if (!elementAtom)
        return false;
    RootedId elementId(cx, AtomToId(elementAtom));

    RootedValue elementVal(cx);
    if (!GetProperty(cx, obj, obj, elementId, &elementVal))
        return false;

    if (!elementVal.isString()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_ELEMENT);
        return false;
    }

    JSLinearString* elementStr = elementVal.toString()->ensureLinear(cx);
    if (!elementStr)
        return false;

    if (!StringEqualsAscii(elementStr, "anyfunc")) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_ELEMENT);
        return false;
    }

    ResizableLimits limits;
    if (!GetResizableLimits(cx, obj, UINT32_MAX, "Table", &limits))
        return false;

    RootedWasmTableObject table(cx, WasmTableObject::create(cx, limits));
    if (!table)
        return false;

    args.rval().setObject(*table);
    return true;
}

static bool
IsTable(HandleValue v)
{
    return v.isObject() && v.toObject().is<WasmTableObject>();
}

/* static */ bool
WasmTableObject::lengthGetterImpl(JSContext* cx, const CallArgs& args)
{
    args.rval().setNumber(args.thisv().toObject().as<WasmTableObject>().table().length());
    return true;
}

/* static */ bool
WasmTableObject::lengthGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsTable, lengthGetterImpl>(cx, args);
}

const JSPropertySpec WasmTableObject::properties[] =
{
    JS_PSG("length", WasmTableObject::lengthGetter, 0),
    JS_PS_END
};

/* static */ bool
WasmTableObject::getImpl(JSContext* cx, const CallArgs& args)
{
    RootedWasmTableObject tableObj(cx, &args.thisv().toObject().as<WasmTableObject>());
    const Table& table = tableObj->table();

    uint32_t index;
    if (!ToNonWrappingUint32(cx, args.get(0), table.length() - 1, "Table", "get index", &index))
        return false;

    ExternalTableElem& elem = table.externalArray()[index];
    if (!elem.code) {
        args.rval().setNull();
        return true;
    }

    Instance& instance = *elem.tls->instance;
    const CodeRange& codeRange = *instance.code().lookupRange(elem.code);
    MOZ_ASSERT(codeRange.isFunction());

    RootedWasmInstanceObject instanceObj(cx, instance.object());
    RootedFunction fun(cx);
    if (!instanceObj->getExportedFunction(cx, instanceObj, codeRange.funcDefIndex(), &fun))
        return false;

    args.rval().setObject(*fun);
    return true;
}

/* static */ bool
WasmTableObject::get(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsTable, getImpl>(cx, args);
}

/* static */ bool
WasmTableObject::setImpl(JSContext* cx, const CallArgs& args)
{
    RootedWasmTableObject tableObj(cx, &args.thisv().toObject().as<WasmTableObject>());
    Table& table = tableObj->table();

    if (!args.requireAtLeast(cx, "set", 2))
        return false;

    uint32_t index;
    if (!ToNonWrappingUint32(cx, args.get(0), table.length() - 1, "Table", "set index", &index))
        return false;

    RootedFunction value(cx);
    if (!IsExportedFunction(args[1], &value) && !args[1].isNull()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_TABLE_VALUE);
        return false;
    }

    if (value) {
        RootedWasmInstanceObject instanceObj(cx, ExportedFunctionToInstanceObject(value));
        uint32_t funcDefIndex = ExportedFunctionToDefinitionIndex(value);

#ifdef DEBUG
        RootedFunction f(cx);
        MOZ_ASSERT(instanceObj->getExportedFunction(cx, instanceObj, funcDefIndex, &f));
        MOZ_ASSERT(value == f);
#endif

        Instance& instance = instanceObj->instance();
        const FuncDefExport& funcDefExport = instance.metadata().lookupFuncDefExport(funcDefIndex);
        const CodeRange& codeRange = instance.metadata().codeRanges[funcDefExport.codeRangeIndex()];
        void* code = instance.codeSegment().base() + codeRange.funcTableEntry();
        table.set(index, code, instance);
    } else {
        table.setNull(index);
    }

    args.rval().setUndefined();
    return true;
}

/* static */ bool
WasmTableObject::set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsTable, setImpl>(cx, args);
}

/* static */ bool
WasmTableObject::growImpl(JSContext* cx, const CallArgs& args)
{
    RootedWasmTableObject table(cx, &args.thisv().toObject().as<WasmTableObject>());

    uint32_t delta;
    if (!ToNonWrappingUint32(cx, args.get(0), UINT32_MAX, "Table", "grow delta", &delta))
        return false;

    uint32_t ret = table->table().grow(delta, cx);

    if (ret == uint32_t(-1)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW, "table");
        return false;
    }

    args.rval().setInt32(ret);
    return true;
}

/* static */ bool
WasmTableObject::grow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsTable, growImpl>(cx, args);
}

const JSFunctionSpec WasmTableObject::methods[] =
{
    JS_FN("get", WasmTableObject::get, 1, 0),
    JS_FN("set", WasmTableObject::set, 2, 0),
    JS_FN("grow", WasmTableObject::grow, 1, 0),
    JS_FS_END
};

Table&
WasmTableObject::table() const
{
    return *(Table*)getReservedSlot(TABLE_SLOT).toPrivate();
}

// ============================================================================
// WebAssembly class and static methods

#if JS_HAS_TOSOURCE
static bool
WebAssembly_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().WebAssembly);
    return true;
}
#endif

#ifdef SPIDERMONKEY_PROMISE
static bool
Nop(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return true;
}

static bool
Reject(JSContext* cx, const CompileArgs& args, UniqueChars error, Handle<PromiseObject*> promise)
{
    if (!error) {
        ReportOutOfMemory(cx);

        RootedValue rejectionValue(cx);
        if (!cx->getPendingException(&rejectionValue))
            return false;

        return promise->reject(cx, rejectionValue);
    }

    RootedObject stack(cx, promise->allocationSite());
    RootedString filename(cx, JS_NewStringCopyZ(cx, args.scriptedCaller.filename.get()));
    if (!filename)
        return false;

    unsigned line = args.scriptedCaller.line;
    unsigned column = args.scriptedCaller.column;

    // Ideally we'd report a JSMSG_WASM_COMPILE_ERROR here, but there's no easy
    // way to create an ErrorObject for an arbitrary error code with multiple
    // replacements.
    UniqueChars str(JS_smprintf("wasm validation error: %s", error.get()));
    if (!str)
        return false;

    RootedString message(cx, NewLatin1StringZ(cx, Move(str)));
    if (!message)
        return false;

    RootedObject errorObj(cx,
        ErrorObject::create(cx, JSEXN_TYPEERR, stack, filename, line, column, nullptr, message));
    if (!errorObj)
        return false;

    RootedValue rejectionValue(cx, ObjectValue(*errorObj));
    return promise->reject(cx, rejectionValue);
}

static bool
Resolve(JSContext* cx, Module& module, Handle<PromiseObject*> promise)
{
    RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmModule).toObject());
    RootedObject moduleObj(cx, WasmModuleObject::create(cx, module, proto));
    if (!moduleObj)
        return false;

    RootedValue resolutionValue(cx, ObjectValue(*moduleObj));
    return promise->resolve(cx, resolutionValue);
}

struct CompileTask : PromiseTask
{
    MutableBytes bytecode;
    CompileArgs  compileArgs;
    UniqueChars  error;
    SharedModule module;

    CompileTask(JSContext* cx, Handle<PromiseObject*> promise)
      : PromiseTask(cx, promise)
    {}

    void execute() override {
        module = Compile(*bytecode, compileArgs, &error);
    }

    bool finishPromise(JSContext* cx, Handle<PromiseObject*> promise) override {
        return module
               ? Resolve(cx, *module, promise)
               : Reject(cx, compileArgs, Move(error), promise);
    }
};

static bool
WebAssembly_compile(JSContext* cx, unsigned argc, Value* vp)
{
    if (!cx->startAsyncTaskCallback || !cx->finishAsyncTaskCallback) {
        JS_ReportErrorASCII(cx, "WebAssembly.compile not supported in this runtime.");
        return false;
    }

    CallArgs callArgs = CallArgsFromVp(argc, vp);

    RootedFunction nopFun(cx, NewNativeFunction(cx, Nop, 0, nullptr));
    if (!nopFun)
        return false;

    Rooted<PromiseObject*> promise(cx, PromiseObject::create(cx, nopFun));
    if (!promise)
        return false;

    auto task = cx->make_unique<CompileTask>(cx, promise);
    if (!task)
        return false;

    if (!GetCompileArgs(cx, callArgs, "WebAssembly.compile", &task->bytecode, &task->compileArgs)) {
        if (!cx->isExceptionPending())
            return false;

        RootedValue rejectionValue(cx);
        if (!GetAndClearException(cx, &rejectionValue))
            return false;

        if (!promise->reject(cx, rejectionValue))
            return false;

        callArgs.rval().setObject(*promise);
        return true;
    }

    if (CanUseExtraThreads()) {
        if (!StartPromiseTask(cx, Move(task)))
            return false;
    } else {
        task->execute();
        task->finishPromise(cx, promise);
    }

    callArgs.rval().setObject(*promise);
    return true;
}
#endif

static bool
WebAssembly_validate(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs callArgs = CallArgsFromVp(argc, vp);

    MutableBytes bytecode;
    CompileArgs compileArgs;
    if (!GetCompileArgs(cx, callArgs, "WebAssembly.validate", &bytecode, &compileArgs))
        return false;

    UniqueChars error;
    bool validated = !!Compile(*bytecode, compileArgs, &error);

    // If the reason for validation failure was OOM (signalled by null error
    // message), report out-of-memory so that validate's return is always
    // correct.
    if (!validated && !error) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (error) {
        if (!JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                               JSMSG_WASM_COMPILE_ERROR, error.get()))
        {
            return false;
        }
    }

    callArgs.rval().setBoolean(validated);
    return true;
}

static const JSFunctionSpec WebAssembly_static_methods[] =
{
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str, WebAssembly_toSource, 0, 0),
#endif
#ifdef SPIDERMONKEY_PROMISE
    JS_FN("compile", WebAssembly_compile, 1, 0),
#endif
    JS_FN("validate", WebAssembly_validate, 1, 0),
    JS_FS_END
};

const Class js::WebAssemblyClass =
{
    js_WebAssembly_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_WebAssembly)
};

template <class Class>
static bool
InitConstructor(JSContext* cx, HandleObject wasm, const char* name, MutableHandleObject proto)
{
    proto.set(NewBuiltinClassInstance<PlainObject>(cx, SingletonObject));
    if (!proto)
        return false;

    if (!DefinePropertiesAndFunctions(cx, proto, Class::properties, Class::methods))
        return false;

    RootedAtom className(cx, Atomize(cx, name, strlen(name)));
    if (!className)
        return false;

    RootedFunction ctor(cx, NewNativeConstructor(cx, Class::construct, 1, className));
    if (!ctor)
        return false;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return false;

    RootedId id(cx, AtomToId(className));
    RootedValue ctorValue(cx, ObjectValue(*ctor));
    return DefineProperty(cx, wasm, id, ctorValue, nullptr, nullptr, 0);
}

static bool
InitErrorClass(JSContext* cx, HandleObject wasm, const char* name, JSExnType exn)
{
    Handle<GlobalObject*> global = cx->global();
    RootedObject proto(cx, GlobalObject::getOrCreateCustomErrorPrototype(cx, global, exn));
    if (!proto)
        return false;

    RootedAtom className(cx, Atomize(cx, name, strlen(name)));
    if (!className)
        return false;

    RootedId id(cx, AtomToId(className));
    RootedValue ctorValue(cx, global->getConstructor(GetExceptionProtoKey(exn)));
    return DefineProperty(cx, wasm, id, ctorValue, nullptr, nullptr, 0);
}

JSObject*
js::InitWebAssemblyClass(JSContext* cx, HandleObject obj)
{
    MOZ_RELEASE_ASSERT(HasCompilerSupport(cx));
    MOZ_ASSERT(cx->options().wasm());

    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    MOZ_ASSERT(!global->isStandardClassResolved(JSProto_WebAssembly));

    RootedObject proto(cx, global->getOrCreateObjectPrototype(cx));
    if (!proto)
        return nullptr;

    RootedObject wasm(cx, NewObjectWithGivenProto(cx, &WebAssemblyClass, proto, SingletonObject));
    if (!wasm)
        return nullptr;

    // This property will be removed before the initial WebAssembly release.
    if (!JS_DefineProperty(cx, wasm, "experimentalVersion", EncodingVersion, JSPROP_RESOLVING))
        return nullptr;

    if (!JS_DefineFunctions(cx, wasm, WebAssembly_static_methods))
        return nullptr;

    RootedObject moduleProto(cx), instanceProto(cx), memoryProto(cx), tableProto(cx);
    if (!InitConstructor<WasmModuleObject>(cx, wasm, "Module", &moduleProto))
        return nullptr;
    if (!InitConstructor<WasmInstanceObject>(cx, wasm, "Instance", &instanceProto))
        return nullptr;
    if (!InitConstructor<WasmMemoryObject>(cx, wasm, "Memory", &memoryProto))
        return nullptr;
    if (!InitConstructor<WasmTableObject>(cx, wasm, "Table", &tableProto))
        return nullptr;
    if (!InitErrorClass(cx, wasm, "CompileError", JSEXN_WASMCOMPILEERROR))
        return nullptr;
    if (!InitErrorClass(cx, wasm, "RuntimeError", JSEXN_WASMRUNTIMEERROR))
        return nullptr;

    // Perform the final fallible write of the WebAssembly object to a global
    // object property at the end. Only after that succeeds write all the
    // constructor and prototypes to the JSProto slots. This ensures that
    // initialization is atomic since a failed initialization can be retried.

    if (!JS_DefineProperty(cx, global, js_WebAssembly_str, wasm, JSPROP_RESOLVING))
        return nullptr;

    global->setPrototype(JSProto_WasmModule, ObjectValue(*moduleProto));
    global->setPrototype(JSProto_WasmInstance, ObjectValue(*instanceProto));
    global->setPrototype(JSProto_WasmMemory, ObjectValue(*memoryProto));
    global->setPrototype(JSProto_WasmTable, ObjectValue(*tableProto));
    global->setConstructor(JSProto_WebAssembly, ObjectValue(*wasm));

    MOZ_ASSERT(global->isStandardClassResolved(JSProto_WebAssembly));
    return wasm;
}

