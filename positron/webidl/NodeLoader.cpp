/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ModuleUtils.h"
#include "nsIModule.h"

#include "nsINodeLoader.h"
#include "NodeLoader.h"
#include "node.h"

////////////////////////////////////////////////////////////////////////
// Define the contructor function for the objects
//
// NOTE: This creates an instance of objects by using the default constructor
//
NS_GENERIC_FACTORY_CONSTRUCTOR(NodeLoader)

////////////////////////////////////////////////////////////////////////
// Define a table of CIDs implemented by this module along with other
// information like the function to create an instance, contractid, and
// class name.
//
#define NS_NODELOADER_CONTRACTID \
  "@mozilla.org/positron/nodeloader;1"

#define NS_NODELOADER_CID             \
{ /* 019718E3-CDB5-11d2-8D3C-000000000000 */    \
0x019618e3, 0xcdb5, 0x11d2,                     \
{ 0x8d, 0x3c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 } }

// static const nsModuleComponentInfo components[] = {
//   { nullptr, NS_NODELOADER_CID, "@mozilla.org/positron/nodeloader;1", NodeLoaderConstructor },
// };


/* Implementation file */
NS_IMPL_ISUPPORTS(NodeLoader, nsINodeLoader)

NodeLoader::NodeLoader()
{
  /* member initializers and constructor code */
}

NodeLoader::~NodeLoader()
{
  /* destructor code */
}

/* void init (); */
NS_IMETHODIMP NodeLoader::Init(JSContext* aContext)
{
  printf("!!!!!!!!!!!!!!NodeLoader::Init\n");
  // (we assume node::Init would not modify the parameters under embedded mode).
  int exec_argc;
  const char** exec_argv;
  int argc = 1;
  char **argv = new char *[argc + 1];
  argv[0] = new char[10];
  node::Init(&argc, const_cast<const char**>(argv),  &exec_argc, &exec_argv);

  v8::V8::Initialize();
  v8::Isolate* isolate = v8::Isolate::New(js::GetRuntime(aContext));
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  node::CreateEnvironment(
    isolate,
    // nullptr /*struct uv_loop_s* loop*/,
    context,
    argc, argv, 0, nullptr);

  //   V8Engine engine;

  // Isolate::Scope isolate_scope(engine.isolate());

  // HandleScope handle_scope(engine.isolate());
  // Local<Context> context = Context::New(engine.isolate());
  // Context::Scope context_scope(context);

  return NS_OK;
}

NS_DEFINE_NAMED_CID(NS_NODELOADER_CID);

static const mozilla::Module::CIDEntry kEmbeddingCIDs[] = {
    { &kNS_NODELOADER_CID, false, nullptr, NodeLoaderConstructor },
    { nullptr }
};

static const mozilla::Module::ContractIDEntry kEmbeddingContracts[] = {
    { NS_NODELOADER_CONTRACTID, &kNS_NODELOADER_CID },
    { nullptr }
};

static const mozilla::Module kEmbeddingModule = {
    mozilla::Module::kVersion,
    kEmbeddingCIDs,
    kEmbeddingContracts
};

NSMODULE_DEFN(NodeLoader) = &kEmbeddingModule;