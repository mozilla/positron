/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ModuleUtils.h"
#include "nsIModule.h"

#include "nsINodeLoader.h"
#include "NodeLoader.h"
#include "NodeBindings.h"
#include "nsString.h"
#include "nsITimer.h"
#include "nsComponentManagerUtils.h"
#include "mozilla/dom/ScriptSettings.h" // for AutoJSAPI

using namespace mozilla;

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

/* Implementation file */
NS_IMPL_ISUPPORTS(NodeLoader, nsINodeLoader)

class InitTimerCallback final : public nsITimerCallback
{
  ~InitTimerCallback() {
    printf(">>> Destryoing InitTimerCallback\n");
  }

public:
  InitTimerCallback(NodeLoader* aNodeLoader, JSObject* aGlobal, bool isBrowser, JSContext* aContext)
    : mNodeLoader(aNodeLoader), mGlobal(aGlobal), mIsBrowser(isBrowser), mContext(aContext)
  { }

  NS_DECL_ISUPPORTS

  NS_IMETHODIMP Notify(nsITimer* aTimer) final
  {
    printf(">>> InitTimerCallback::Notify\n");
    mNodeLoader->timer = nullptr;
    mNodeLoader->nodeBindings = NodeBindings::Create(mIsBrowser);
    mNodeLoader->nodeBindings->Initialize(mContext, mGlobal);
    return NS_OK;
  }

protected:
  NodeLoader* mNodeLoader;
  JSObject* mGlobal;
  bool mIsBrowser;
  JSContext* mContext;
};

NS_IMPL_ISUPPORTS(InitTimerCallback, nsITimerCallback)

NodeLoader::NodeLoader()
{
  /* member initializers and constructor code */
}

NodeLoader::~NodeLoader()
{
}

/* void init (); */
NS_IMETHODIMP NodeLoader::Init(const nsACString& type, JSContext* aContext)
{
  MOZ_ASSERT(type.EqualsLiteral("browser") || type.EqualsLiteral("renderer"));
  JSObject* globalObject = JS::CurrentGlobalOrNull(aContext);
  MOZ_ASSERT(globalObject);
  RefPtr<nsITimerCallback> timerCb = new InitTimerCallback(this, globalObject, type.EqualsLiteral("browser"), aContext);
  timer = do_CreateInstance("@mozilla.org/timer;1");
  nsresult rv = timer->InitWithCallback(timerCb, 0, nsITimer::TYPE_ONE_SHOT);
  NS_ENSURE_SUCCESS(rv, rv);
  printf(">>> HI\n");
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