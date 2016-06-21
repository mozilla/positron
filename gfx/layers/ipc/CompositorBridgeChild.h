/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_CompositorBridgeChild_h
#define mozilla_layers_CompositorBridgeChild_h

#include "base/basictypes.h"            // for DISALLOW_EVIL_CONSTRUCTORS
#include "mozilla/Assertions.h"         // for MOZ_ASSERT_HELPER2
#include "mozilla/Attributes.h"         // for override
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/layers/PCompositorBridgeChild.h"
#include "mozilla/layers/TextureForwarder.h" // for TextureForwarder
#include "nsClassHashtable.h"           // for nsClassHashtable
#include "nsCOMPtr.h"                   // for nsCOMPtr
#include "nsHashKeys.h"                 // for nsUint64HashKey
#include "nsISupportsImpl.h"            // for NS_INLINE_DECL_REFCOUNTING
#include "ThreadSafeRefcountingWithMainThreadDestruction.h"
#include "nsWeakReference.h"

namespace mozilla {

namespace dom {
  class TabChild;
} // namespace dom

namespace layers {

using mozilla::dom::TabChild;

class ClientLayerManager;
class CompositorBridgeParent;
class TextureClient;
class TextureClientPool;
struct FrameMetrics;

class CompositorBridgeChild final : public PCompositorBridgeChild,
                                    public TextureForwarder,
                                    public ShmemAllocator
{
  typedef InfallibleTArray<AsyncParentMessageData> AsyncParentMessageArray;

public:
  explicit CompositorBridgeChild(ClientLayerManager *aLayerManager);

  void Destroy();

  /**
   * Lookup the FrameMetrics shared by the compositor process with the
   * associated FrameMetrics::ViewID. The returned FrameMetrics is used
   * in progressive paint calculations.
   */
  bool LookupCompositorFrameMetrics(const FrameMetrics::ViewID aId, FrameMetrics&);

  /**
   * We're asked to create a new Compositor in response to an Opens()
   * or Bridge() request from our parent process.  The Transport is to
   * the compositor's context.
   */
  static PCompositorBridgeChild*
  Create(Transport* aTransport, ProcessId aOtherProcess);

  /**
   * Initialize the CompositorBridgeChild and open the connection in the non-multi-process
   * case.
   */
  bool OpenSameProcess(CompositorBridgeParent* aParent);

  static CompositorBridgeChild* Get();

  static bool ChildProcessHasCompositorBridge();

  void AddOverfillObserver(ClientLayerManager* aLayerManager);

  virtual bool
  RecvClearCachedResources(const uint64_t& id) override;

  virtual bool
  RecvDidComposite(const uint64_t& aId, const uint64_t& aTransactionId,
                   const TimeStamp& aCompositeStart,
                   const TimeStamp& aCompositeEnd) override;

  virtual bool
  RecvInvalidateLayers(const uint64_t& aLayersId) override;

  virtual bool
  RecvCompositorUpdated(const uint64_t& aLayersId,
                        const TextureFactoryIdentifier& aNewIdentifier) override;

  virtual bool
  RecvOverfill(const uint32_t &aOverfill) override;

  virtual bool
  RecvUpdatePluginConfigurations(const LayoutDeviceIntPoint& aContentOffset,
                                 const LayoutDeviceIntRegion& aVisibleRegion,
                                 nsTArray<PluginWindowData>&& aPlugins) override;

  virtual bool
  RecvHideAllPlugins(const uintptr_t& aParentWidget) override;

  virtual PTextureChild* AllocPTextureChild(const SurfaceDescriptor& aSharedData,
                                            const LayersBackend& aLayersBackend,
                                            const TextureFlags& aFlags,
                                            const uint64_t& aId,
                                            const uint64_t& aSerial) override;

  virtual bool DeallocPTextureChild(PTextureChild* actor) override;

  virtual bool
  RecvParentAsyncMessages(InfallibleTArray<AsyncParentMessageData>&& aMessages) override;
  virtual PTextureChild* CreateTexture(const SurfaceDescriptor& aSharedData,
                                       LayersBackend aLayersBackend,
                                       TextureFlags aFlags,
                                       uint64_t aSerial) override;

  /**
   * Request that the parent tell us when graphics are ready on GPU.
   * When we get that message, we bounce it to the TabParent via
   * the TabChild
   * @param tabChild The object to bounce the note to.  Non-NULL.
   */
  void RequestNotifyAfterRemotePaint(TabChild* aTabChild);

  void CancelNotifyAfterRemotePaint(TabChild* aTabChild);

  // Beware that these methods don't override their super-class equivalent (which
  // are not virtual), they just overload them.
  // All of these Send* methods just add a sanity check (that it is not too late
  // send a message) and forward the call to the super-class's equivalent method.
  // This means that it is correct to call directly the super-class methods, but
  // you won't get the extra safety provided here.
  bool SendWillClose();
  bool SendPause();
  bool SendResume();
  bool SendNotifyHidden(const uint64_t& id);
  bool SendNotifyVisible(const uint64_t& id);
  bool SendNotifyChildCreated(const uint64_t& id);
  bool SendAdoptChild(const uint64_t& id);
  bool SendMakeSnapshot(const SurfaceDescriptor& inSnapshot, const gfx::IntRect& dirtyRect);
  bool SendFlushRendering();
  bool SendGetTileSize(int32_t* tileWidth, int32_t* tileHeight);
  bool SendStartFrameTimeRecording(const int32_t& bufferSize, uint32_t* startIndex);
  bool SendStopFrameTimeRecording(const uint32_t& startIndex, nsTArray<float>* intervals);
  bool SendNotifyRegionInvalidated(const nsIntRegion& region);
  bool SendRequestNotifyAfterRemotePaint();
  bool SendClearVisibleRegions(uint64_t aLayersId, uint32_t aPresShellId);
  bool SendUpdateVisibleRegion(VisibilityCounter aCounter,
                               const ScrollableLayerGuid& aGuid,
                               const mozilla::CSSIntRegion& aRegion);
  bool IsSameProcess() const override;

  virtual bool IPCOpen() const override { return mCanSend; }

  static void ShutDown();

  void UpdateFwdTransactionId() { ++mFwdTransactionId; }
  uint64_t GetFwdTransactionId() { return mFwdTransactionId; }

  /**
   * Hold TextureClient ref until end of usage on host side if TextureFlags::RECYCLE is set.
   * Host side's usage is checked via CompositableRef.
   */
  void HoldUntilCompositableRefReleasedIfNecessary(TextureClient* aClient);

  /**
   * Notify id of Texture When host side end its use. Transaction id is used to
   * make sure if there is no newer usage.
   */
  void NotifyNotUsed(uint64_t aTextureId, uint64_t aFwdTransactionId);

  void DeliverFence(uint64_t aTextureId, FenceHandle& aReleaseFenceHandle);

  virtual void CancelWaitForRecycle(uint64_t aTextureId) override;

  TextureClientPool* GetTexturePool(gfx::SurfaceFormat aFormat, TextureFlags aFlags);
  void ClearTexturePool();

  void HandleMemoryPressure();

  virtual MessageLoop* GetMessageLoop() const override { return mMessageLoop; }

  virtual bool AllocUnsafeShmem(size_t aSize,
                                mozilla::ipc::SharedMemory::SharedMemoryType aShmType,
                                mozilla::ipc::Shmem* aShmem) override;
  virtual bool AllocShmem(size_t aSize,
                          mozilla::ipc::SharedMemory::SharedMemoryType aShmType,
                          mozilla::ipc::Shmem* aShmem) override;
  virtual void DeallocShmem(mozilla::ipc::Shmem& aShmem) override;

  virtual ShmemAllocator* AsShmemAllocator() override { return this; }

private:
  // Private destructor, to discourage deletion outside of Release():
  virtual ~CompositorBridgeChild();

  virtual PLayerTransactionChild*
    AllocPLayerTransactionChild(const nsTArray<LayersBackend>& aBackendHints,
                                const uint64_t& aId,
                                TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                bool* aSuccess) override;

  virtual bool DeallocPLayerTransactionChild(PLayerTransactionChild *aChild) override;

  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

  virtual bool RecvSharedCompositorFrameMetrics(const mozilla::ipc::SharedMemoryBasic::Handle& metrics,
                                                const CrossProcessMutexHandle& handle,
                                                const uint64_t& aLayersId,
                                                const uint32_t& aAPZCId) override;

  virtual bool RecvReleaseSharedCompositorFrameMetrics(const ViewID& aId,
                                                       const uint32_t& aAPZCId) override;

  virtual bool
  RecvRemotePaintIsReady() override;

  // Class used to store the shared FrameMetrics, mutex, and APZCId  in a hash table
  class SharedFrameMetricsData {
  public:
    SharedFrameMetricsData(
        const mozilla::ipc::SharedMemoryBasic::Handle& metrics,
        const CrossProcessMutexHandle& handle,
        const uint64_t& aLayersId,
        const uint32_t& aAPZCId);

    ~SharedFrameMetricsData();

    void CopyFrameMetrics(FrameMetrics* aFrame);
    FrameMetrics::ViewID GetViewID();
    uint64_t GetLayersId() const;
    uint32_t GetAPZCId();

  private:
    // Pointer to the class that allows access to the shared memory that contains
    // the shared FrameMetrics
    RefPtr<mozilla::ipc::SharedMemoryBasic> mBuffer;
    CrossProcessMutex* mMutex;
    uint64_t mLayersId;
    // Unique ID of the APZC that is sharing the FrameMetrics
    uint32_t mAPZCId;
  };

  RefPtr<ClientLayerManager> mLayerManager;
  // When not multi-process, hold a reference to the CompositorBridgeParent to keep it
  // alive. This reference should be null in multi-process.
  RefPtr<CompositorBridgeParent> mCompositorBridgeParent;

  // The ViewID of the FrameMetrics is used as the key for this hash table.
  // While this should be safe to use since the ViewID is unique
  nsClassHashtable<nsUint64HashKey, SharedFrameMetricsData> mFrameMetricsTable;

  // Weakly hold the TabChild that made a request to be alerted when
  // the transaction has been received.
  nsWeakPtr mWeakTabChild;      // type is TabChild

  DISALLOW_EVIL_CONSTRUCTORS(CompositorBridgeChild);

  // When we receive overfill numbers, notify these client layer managers
  AutoTArray<ClientLayerManager*,0> mOverfillObservers;

  // True until the beginning of the two-step shutdown sequence of this actor.
  bool mCanSend;

  /**
   * Transaction id of ShadowLayerForwarder.
   * It is incrementaed by UpdateFwdTransactionId() in each BeginTransaction() call.
   */
  uint64_t mFwdTransactionId;

  /**
   * Hold TextureClients refs until end of their usages on host side.
   * It defer calling of TextureClient recycle callback.
   */
  nsDataHashtable<nsUint64HashKey, RefPtr<TextureClient> > mTexturesWaitingRecycled;

  MessageLoop* mMessageLoop;

  AutoTArray<RefPtr<TextureClientPool>,2> mTexturePools;
};

} // namespace layers
} // namespace mozilla

#endif // mozilla_layers_CompositorBrigedChild_h
