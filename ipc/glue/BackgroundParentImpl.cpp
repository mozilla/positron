/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundParentImpl.h"

#include "BroadcastChannelParent.h"
#include "FileDescriptorSetParent.h"
#ifdef MOZ_WEBRTC
#include "CamerasParent.h"
#endif
#include "mozilla/media/MediaParent.h"
#include "mozilla/AppProcessChecker.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/DOMTypes.h"
#include "mozilla/dom/FileSystemBase.h"
#include "mozilla/dom/FileSystemRequestParent.h"
#include "mozilla/dom/NuwaParent.h"
#include "mozilla/dom/PBlobParent.h"
#include "mozilla/dom/MessagePortParent.h"
#include "mozilla/dom/ServiceWorkerRegistrar.h"
#include "mozilla/dom/asmjscache/AsmJSCache.h"
#include "mozilla/dom/cache/ActorUtils.h"
#include "mozilla/dom/indexedDB/ActorsParent.h"
#include "mozilla/dom/ipc/BlobParent.h"
#include "mozilla/dom/quota/ActorsParent.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "mozilla/ipc/PBackgroundTestParent.h"
#include "mozilla/ipc/PSendStreamParent.h"
#include "mozilla/ipc/SendStreamAlloc.h"
#include "mozilla/layout/VsyncParent.h"
#include "mozilla/dom/network/UDPSocketParent.h"
#include "mozilla/Preferences.h"
#include "nsIAppsService.h"
#include "nsNetUtil.h"
#include "nsIScriptSecurityManager.h"
#include "nsProxyRelease.h"
#include "mozilla/RefPtr.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "nsXULAppAPI.h"
#include "ServiceWorkerManagerParent.h"

#ifdef DISABLE_ASSERTS_FOR_FUZZING
#define ASSERT_UNLESS_FUZZING(...) do { } while (0)
#else
#define ASSERT_UNLESS_FUZZING(...) MOZ_ASSERT(false)
#endif

using mozilla::ipc::AssertIsOnBackgroundThread;
using mozilla::dom::asmjscache::PAsmJSCacheEntryParent;
using mozilla::dom::cache::PCacheParent;
using mozilla::dom::cache::PCacheStorageParent;
using mozilla::dom::cache::PCacheStreamControlParent;
using mozilla::dom::FileSystemBase;
using mozilla::dom::FileSystemRequestParent;
using mozilla::dom::MessagePortParent;
using mozilla::dom::NuwaParent;
using mozilla::dom::PMessagePortParent;
using mozilla::dom::PNuwaParent;
using mozilla::dom::UDPSocketParent;

namespace {

void
AssertIsOnMainThread()
{
  MOZ_ASSERT(NS_IsMainThread());
}

class TestParent final : public mozilla::ipc::PBackgroundTestParent
{
  friend class mozilla::ipc::BackgroundParentImpl;

  TestParent()
  {
    MOZ_COUNT_CTOR(TestParent);
  }

protected:
  ~TestParent()
  {
    MOZ_COUNT_DTOR(TestParent);
  }

public:
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) override;
};

} // namespace

namespace mozilla {
namespace ipc {

using mozilla::dom::ContentParent;
using mozilla::dom::BroadcastChannelParent;
using mozilla::dom::ServiceWorkerRegistrationData;
using mozilla::dom::workers::ServiceWorkerManagerParent;

BackgroundParentImpl::BackgroundParentImpl()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundParentImpl);
}

BackgroundParentImpl::~BackgroundParentImpl()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundParentImpl);
}

void
BackgroundParentImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}

BackgroundParentImpl::PBackgroundTestParent*
BackgroundParentImpl::AllocPBackgroundTestParent(const nsCString& aTestArg)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new TestParent();
}

bool
BackgroundParentImpl::RecvPBackgroundTestConstructor(
                                                  PBackgroundTestParent* aActor,
                                                  const nsCString& aTestArg)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return PBackgroundTestParent::Send__delete__(aActor, aTestArg);
}

bool
BackgroundParentImpl::DeallocPBackgroundTestParent(
                                                  PBackgroundTestParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<TestParent*>(aActor);
  return true;
}

auto
BackgroundParentImpl::AllocPBackgroundIDBFactoryParent(
                                                const LoggingInfo& aLoggingInfo)
  -> PBackgroundIDBFactoryParent*
{
  using mozilla::dom::indexedDB::AllocPBackgroundIDBFactoryParent;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return AllocPBackgroundIDBFactoryParent(aLoggingInfo);
}

bool
BackgroundParentImpl::RecvPBackgroundIDBFactoryConstructor(
                                            PBackgroundIDBFactoryParent* aActor,
                                            const LoggingInfo& aLoggingInfo)
{
  using mozilla::dom::indexedDB::RecvPBackgroundIDBFactoryConstructor;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return RecvPBackgroundIDBFactoryConstructor(aActor, aLoggingInfo);
}

bool
BackgroundParentImpl::DeallocPBackgroundIDBFactoryParent(
                                            PBackgroundIDBFactoryParent* aActor)
{
  using mozilla::dom::indexedDB::DeallocPBackgroundIDBFactoryParent;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return DeallocPBackgroundIDBFactoryParent(aActor);
}

auto
BackgroundParentImpl::AllocPBackgroundIndexedDBUtilsParent()
  -> PBackgroundIndexedDBUtilsParent*
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::indexedDB::AllocPBackgroundIndexedDBUtilsParent();
}

bool
BackgroundParentImpl::DeallocPBackgroundIndexedDBUtilsParent(
                                        PBackgroundIndexedDBUtilsParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return
    mozilla::dom::indexedDB::DeallocPBackgroundIndexedDBUtilsParent(aActor);
}

bool
BackgroundParentImpl::RecvFlushPendingFileDeletions()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::indexedDB::RecvFlushPendingFileDeletions();
}

auto
BackgroundParentImpl::AllocPBlobParent(const BlobConstructorParams& aParams)
  -> PBlobParent*
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (NS_WARN_IF(aParams.type() !=
                   BlobConstructorParams::TParentBlobConstructorParams)) {
    ASSERT_UNLESS_FUZZING();
    return nullptr;
  }

  return mozilla::dom::BlobParent::Create(this, aParams);
}

bool
BackgroundParentImpl::DeallocPBlobParent(PBlobParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  mozilla::dom::BlobParent::Destroy(aActor);
  return true;
}

bool
BackgroundParentImpl::RecvPBlobConstructor(PBlobParent* aActor,
                                           const BlobConstructorParams& aParams)
{
  const ParentBlobConstructorParams& params = aParams;
  if (params.blobParams().type() == AnyBlobConstructorParams::TKnownBlobConstructorParams) {
    return aActor->SendCreatedFromKnownBlob();
  }

  return true;
}

PFileDescriptorSetParent*
BackgroundParentImpl::AllocPFileDescriptorSetParent(
                                          const FileDescriptor& aFileDescriptor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new FileDescriptorSetParent(aFileDescriptor);
}

bool
BackgroundParentImpl::DeallocPFileDescriptorSetParent(
                                               PFileDescriptorSetParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<FileDescriptorSetParent*>(aActor);
  return true;
}

PNuwaParent*
BackgroundParentImpl::AllocPNuwaParent()
{
  return mozilla::dom::NuwaParent::Alloc();
}

bool
BackgroundParentImpl::RecvPNuwaConstructor(PNuwaParent* aActor)
{
  return mozilla::dom::NuwaParent::ActorConstructed(aActor);
}

bool
BackgroundParentImpl::DeallocPNuwaParent(PNuwaParent *aActor)
{
  return mozilla::dom::NuwaParent::Dealloc(aActor);
}

PSendStreamParent*
BackgroundParentImpl::AllocPSendStreamParent()
{
  return mozilla::ipc::AllocPSendStreamParent();
}

bool
BackgroundParentImpl::DeallocPSendStreamParent(PSendStreamParent* aActor)
{
  delete aActor;
  return true;
}

BackgroundParentImpl::PVsyncParent*
BackgroundParentImpl::AllocPVsyncParent()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<mozilla::layout::VsyncParent> actor =
      mozilla::layout::VsyncParent::Create();
  // There still has one ref-count after return, and it will be released in
  // DeallocPVsyncParent().
  return actor.forget().take();
}

bool
BackgroundParentImpl::DeallocPVsyncParent(PVsyncParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  // This actor already has one ref-count. Please check AllocPVsyncParent().
  RefPtr<mozilla::layout::VsyncParent> actor =
      dont_AddRef(static_cast<mozilla::layout::VsyncParent*>(aActor));
  return true;
}

camera::PCamerasParent*
BackgroundParentImpl::AllocPCamerasParent()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

#ifdef MOZ_WEBRTC
  RefPtr<mozilla::camera::CamerasParent> actor =
      mozilla::camera::CamerasParent::Create();
  return actor.forget().take();
#else
  return nullptr;
#endif
}

bool
BackgroundParentImpl::DeallocPCamerasParent(camera::PCamerasParent *aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

#ifdef MOZ_WEBRTC
  RefPtr<mozilla::camera::CamerasParent> actor =
      dont_AddRef(static_cast<mozilla::camera::CamerasParent*>(aActor));
#endif
  return true;
}

namespace {

class InitUDPSocketParentCallback final : public Runnable
{
public:
  InitUDPSocketParentCallback(UDPSocketParent* aActor,
                              const nsACString& aFilter)
    : mActor(aActor)
    , mFilter(aFilter)
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();
  }

  NS_IMETHODIMP
  Run()
  {
    AssertIsInMainProcess();

    IPC::Principal principal;
    if (!mActor->Init(principal, mFilter)) {
      MOZ_CRASH("UDPSocketCallback - failed init");
    }
    return NS_OK;
  }

private:
  ~InitUDPSocketParentCallback() {};

  RefPtr<UDPSocketParent> mActor;
  nsCString mFilter;
};

} // namespace

auto
BackgroundParentImpl::AllocPUDPSocketParent(const OptionalPrincipalInfo& /* unused */,
                                            const nsCString& /* unused */)
  -> PUDPSocketParent*
{
  RefPtr<UDPSocketParent> p = new UDPSocketParent(this);

  return p.forget().take();
}

bool
BackgroundParentImpl::RecvPUDPSocketConstructor(PUDPSocketParent* aActor,
                                                const OptionalPrincipalInfo& aOptionalPrincipal,
                                                const nsCString& aFilter)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (aOptionalPrincipal.type() == OptionalPrincipalInfo::TPrincipalInfo) {
    // Support for checking principals (for non-mtransport use) will be handled in
    // bug 1167039
    return false;
  }
  // No principal - This must be from mtransport (WebRTC/ICE) - We'd want
  // to DispatchToMainThread() here, but if we do we must block RecvBind()
  // until Init() gets run.  Since we don't have a principal, and we verify
  // we have a filter, we can safely skip the Dispatch and just invoke Init()
  // to install the filter.

  // For mtransport, this will always be "stun", which doesn't allow outbound
  // packets if they aren't STUN packets until a STUN response is seen.
  if (!aFilter.EqualsASCII(NS_NETWORK_SOCKET_FILTER_HANDLER_STUN_SUFFIX)) {
    return false;
  }

  IPC::Principal principal;
  if (!static_cast<UDPSocketParent*>(aActor)->Init(principal, aFilter)) {
    MOZ_CRASH("UDPSocketCallback - failed init");
  }

  return true;
}

bool
BackgroundParentImpl::DeallocPUDPSocketParent(PUDPSocketParent* actor)
{
  UDPSocketParent* p = static_cast<UDPSocketParent*>(actor);
  p->Release();
  return true;
}

mozilla::dom::PBroadcastChannelParent*
BackgroundParentImpl::AllocPBroadcastChannelParent(
                                            const PrincipalInfo& aPrincipalInfo,
                                            const nsCString& aOrigin,
                                            const nsString& aChannel)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  nsString originChannelKey;

  // The format of originChannelKey is:
  //  <channelName>|<origin+OriginAttributes>

  originChannelKey.Assign(aChannel);

  originChannelKey.AppendLiteral("|");

  originChannelKey.Append(NS_ConvertUTF8toUTF16(aOrigin));

  return new BroadcastChannelParent(originChannelKey);
}

namespace {

struct MOZ_STACK_CLASS NullifyContentParentRAII
{
  explicit NullifyContentParentRAII(RefPtr<ContentParent>& aContentParent)
    : mContentParent(aContentParent)
  {}

  ~NullifyContentParentRAII()
  {
    mContentParent = nullptr;
  }

  RefPtr<ContentParent>& mContentParent;
};

class CheckPrincipalRunnable final : public Runnable
{
public:
  CheckPrincipalRunnable(already_AddRefed<ContentParent> aParent,
                         const PrincipalInfo& aPrincipalInfo,
                         const nsCString& aOrigin)
    : mContentParent(aParent)
    , mPrincipalInfo(aPrincipalInfo)
    , mOrigin(aOrigin)
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    MOZ_ASSERT(mContentParent);
  }

  NS_IMETHODIMP Run()
  {
    MOZ_ASSERT(NS_IsMainThread());

    NullifyContentParentRAII raii(mContentParent);

    nsCOMPtr<nsIPrincipal> principal = PrincipalInfoToPrincipal(mPrincipalInfo);
    AssertAppPrincipal(mContentParent, principal);

    bool isNullPrincipal;
    nsresult rv = principal->GetIsNullPrincipal(&isNullPrincipal);
    if (NS_WARN_IF(NS_FAILED(rv)) || isNullPrincipal) {
      mContentParent->KillHard("BroadcastChannel killed: no null principal.");
      return NS_OK;
    }

    nsAutoCString origin;
    rv = principal->GetOrigin(origin);
    if (NS_FAILED(rv)) {
      mContentParent->KillHard("BroadcastChannel killed: principal::GetOrigin failed.");
      return NS_OK;
    }

    if (NS_WARN_IF(!mOrigin.Equals(origin))) {
      mContentParent->KillHard("BroadcastChannel killed: origins do not match.");
      return NS_OK;
    }

    return NS_OK;
  }

private:
  RefPtr<ContentParent> mContentParent;
  PrincipalInfo mPrincipalInfo;
  nsCString mOrigin;
};

class CheckPermissionRunnable final : public Runnable
{
public:
  CheckPermissionRunnable(already_AddRefed<ContentParent> aParent,
                          FileSystemRequestParent* aActor,
                          FileSystemBase::ePermissionCheckType aPermissionCheckType,
                          const nsCString& aPermissionName)
    : mContentParent(aParent)
    , mActor(aActor)
    , mPermissionCheckType(aPermissionCheckType)
    , mPermissionName(aPermissionName)
    , mBackgroundEventTarget(NS_GetCurrentThread())
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    MOZ_ASSERT(mContentParent);
    MOZ_ASSERT(mBackgroundEventTarget);
    MOZ_ASSERT(mPermissionCheckType == FileSystemBase::ePermissionCheckRequired ||
               mPermissionCheckType == FileSystemBase::ePermissionCheckByTestingPref);
  }

  NS_IMETHOD
  Run() override
  {
    if (NS_IsMainThread()) {
      NullifyContentParentRAII raii(mContentParent);

      // If the permission is granted, we go back to the background thread to
      // dispatch this task.
      if (CheckPermission()) {
        return mBackgroundEventTarget->Dispatch(this, NS_DISPATCH_NORMAL);
      }

      return NS_OK;
    }

    AssertIsOnBackgroundThread();

    // It can happen that this actor has been destroyed in the meantime we were
    // on the main-thread.
    if (!mActor->Destroyed()) {
      mActor->Start();
    }

    return NS_OK;
  }

private:
  ~CheckPermissionRunnable()
  {
     NS_ProxyRelease(mBackgroundEventTarget, mActor.forget());
  }

  bool
  CheckPermission()
  {
    if (mPermissionCheckType == FileSystemBase::ePermissionCheckByTestingPref &&
        mozilla::Preferences::GetBool("device.storage.prompt.testing", false)) {
      return true;
    }

    if (!AssertAppProcessPermission(mContentParent.get(),
                                    mPermissionName.get())) {
      mContentParent->KillHard("PBackground actor killed: permission denied.");
      return false;
    }

    return true;
  }

  RefPtr<ContentParent> mContentParent;

  RefPtr<FileSystemRequestParent> mActor;

  FileSystemBase::ePermissionCheckType mPermissionCheckType;
  nsCString mPermissionName;

  nsCOMPtr<nsIEventTarget> mBackgroundEventTarget;
};

} // namespace

bool
BackgroundParentImpl::RecvPBroadcastChannelConstructor(
                                            PBroadcastChannelParent* actor,
                                            const PrincipalInfo& aPrincipalInfo,
                                            const nsCString& aOrigin,
                                            const nsString& aChannel)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<ContentParent> parent = BackgroundParent::GetContentParent(this);

  // If the ContentParent is null we are dealing with a same-process actor.
  if (!parent) {
    MOZ_ASSERT(aPrincipalInfo.type() != PrincipalInfo::TNullPrincipalInfo);
    return true;
  }

  RefPtr<CheckPrincipalRunnable> runnable =
    new CheckPrincipalRunnable(parent.forget(), aPrincipalInfo, aOrigin);
  MOZ_ALWAYS_SUCCEEDS(NS_DispatchToMainThread(runnable));

  return true;
}

bool
BackgroundParentImpl::DeallocPBroadcastChannelParent(
                                                PBroadcastChannelParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BroadcastChannelParent*>(aActor);
  return true;
}

mozilla::dom::PServiceWorkerManagerParent*
BackgroundParentImpl::AllocPServiceWorkerManagerParent()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<dom::workers::ServiceWorkerManagerParent> agent =
    new dom::workers::ServiceWorkerManagerParent();
  return agent.forget().take();
}

bool
BackgroundParentImpl::DeallocPServiceWorkerManagerParent(
                                            PServiceWorkerManagerParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  RefPtr<dom::workers::ServiceWorkerManagerParent> parent =
    dont_AddRef(static_cast<dom::workers::ServiceWorkerManagerParent*>(aActor));
  MOZ_ASSERT(parent);
  return true;
}

bool
BackgroundParentImpl::RecvShutdownServiceWorkerRegistrar()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return false;
  }

  RefPtr<dom::ServiceWorkerRegistrar> service =
    dom::ServiceWorkerRegistrar::Get();
  MOZ_ASSERT(service);

  service->Shutdown();
  return true;
}

PCacheStorageParent*
BackgroundParentImpl::AllocPCacheStorageParent(const Namespace& aNamespace,
                                               const PrincipalInfo& aPrincipalInfo)
{
  return dom::cache::AllocPCacheStorageParent(this, aNamespace, aPrincipalInfo);
}

bool
BackgroundParentImpl::DeallocPCacheStorageParent(PCacheStorageParent* aActor)
{
  dom::cache::DeallocPCacheStorageParent(aActor);
  return true;
}

PCacheParent*
BackgroundParentImpl::AllocPCacheParent()
{
  MOZ_CRASH("CacheParent actor must be provided to PBackground manager");
  return nullptr;
}

bool
BackgroundParentImpl::DeallocPCacheParent(PCacheParent* aActor)
{
  dom::cache::DeallocPCacheParent(aActor);
  return true;
}

PCacheStreamControlParent*
BackgroundParentImpl::AllocPCacheStreamControlParent()
{
  MOZ_CRASH("CacheStreamControlParent actor must be provided to PBackground manager");
  return nullptr;
}

bool
BackgroundParentImpl::DeallocPCacheStreamControlParent(PCacheStreamControlParent* aActor)
{
  dom::cache::DeallocPCacheStreamControlParent(aActor);
  return true;
}

PMessagePortParent*
BackgroundParentImpl::AllocPMessagePortParent(const nsID& aUUID,
                                              const nsID& aDestinationUUID,
                                              const uint32_t& aSequenceID)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new MessagePortParent(aUUID);
}

bool
BackgroundParentImpl::RecvPMessagePortConstructor(PMessagePortParent* aActor,
                                                  const nsID& aUUID,
                                                  const nsID& aDestinationUUID,
                                                  const uint32_t& aSequenceID)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  MessagePortParent* mp = static_cast<MessagePortParent*>(aActor);
  return mp->Entangle(aDestinationUUID, aSequenceID);
}

bool
BackgroundParentImpl::DeallocPMessagePortParent(PMessagePortParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<MessagePortParent*>(aActor);
  return true;
}

bool
BackgroundParentImpl::RecvMessagePortForceClose(const nsID& aUUID,
                                                const nsID& aDestinationUUID,
                                                const uint32_t& aSequenceID)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return MessagePortParent::ForceClose(aUUID, aDestinationUUID, aSequenceID);
}

PAsmJSCacheEntryParent*
BackgroundParentImpl::AllocPAsmJSCacheEntryParent(
                               const dom::asmjscache::OpenMode& aOpenMode,
                               const dom::asmjscache::WriteParams& aWriteParams,
                               const PrincipalInfo& aPrincipalInfo)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return
    dom::asmjscache::AllocEntryParent(aOpenMode, aWriteParams, aPrincipalInfo);
}

bool
BackgroundParentImpl::DeallocPAsmJSCacheEntryParent(
                                                 PAsmJSCacheEntryParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  dom::asmjscache::DeallocEntryParent(aActor);
  return true;
}

BackgroundParentImpl::PQuotaParent*
BackgroundParentImpl::AllocPQuotaParent()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::quota::AllocPQuotaParent();
}

bool
BackgroundParentImpl::DeallocPQuotaParent(PQuotaParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::quota::DeallocPQuotaParent(aActor);
}

dom::PFileSystemRequestParent*
BackgroundParentImpl::AllocPFileSystemRequestParent(
                                                const FileSystemParams& aParams)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<FileSystemRequestParent> result = new FileSystemRequestParent();

  if (NS_WARN_IF(!result->Initialize(aParams))) {
    return nullptr;
  }

  return result.forget().take();
}

bool
BackgroundParentImpl::RecvPFileSystemRequestConstructor(
                                               PFileSystemRequestParent* aActor,
                                               const FileSystemParams& aParams)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<FileSystemRequestParent> actor = static_cast<FileSystemRequestParent*>(aActor);

  if (actor->PermissionCheckType() == FileSystemBase::ePermissionCheckNotRequired) {
    actor->Start();
    return true;
  }

  RefPtr<ContentParent> parent = BackgroundParent::GetContentParent(this);

  // If the ContentParent is null we are dealing with a same-process actor.
  if (!parent) {
    actor->Start();
    return true;
  }

  const nsCString& permissionName = actor->PermissionName();
  MOZ_ASSERT(!permissionName.IsEmpty());

  // At this point we should have the right permission but we do the last check
  // with this runnable. If the app doesn't have the permission, we kill the
  // child process.
  RefPtr<CheckPermissionRunnable> runnable =
    new CheckPermissionRunnable(parent.forget(), actor,
                                actor->PermissionCheckType(), permissionName);

  nsresult rv = NS_DispatchToMainThread(runnable);
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(rv));

  return true;
}

bool
BackgroundParentImpl::DeallocPFileSystemRequestParent(
                                              PFileSystemRequestParent* aDoomed)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  RefPtr<FileSystemRequestParent> parent =
    dont_AddRef(static_cast<FileSystemRequestParent*>(aDoomed));
  return true;
}

} // namespace ipc
} // namespace mozilla

void
TestParent::ActorDestroy(ActorDestroyReason aWhy)
{
  mozilla::ipc::AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}
