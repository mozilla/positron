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

#ifndef wasm_stubs_h
#define wasm_stubs_h

#include "asmjs/WasmTypes.h"

namespace js {

namespace jit { class MacroAssembler; }

namespace wasm {

class Export;
class Import;

extern Offsets
GenerateEntry(jit::MacroAssembler& masm, const Export& exp, bool usesHeap);

extern ProfilingOffsets
GenerateInterpExit(jit::MacroAssembler& masm, const Import& import, uint32_t importIndex);

extern ProfilingOffsets
GenerateJitExit(jit::MacroAssembler& masm, const Import& import, bool usesHeap);

extern Offsets
GenerateJumpTarget(jit::MacroAssembler& masm, JumpTarget target);

extern Offsets
GenerateInterruptStub(jit::MacroAssembler& masm);

} // namespace wasm
} // namespace js

#endif // wasm_stubs_h
