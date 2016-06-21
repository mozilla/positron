/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/MediaKeySystemAccess.h"
#include "mozilla/dom/MediaKeySystemAccessBinding.h"
#include "mozilla/Preferences.h"
#include "MediaPrefs.h"
#include "nsContentTypeParser.h"
#ifdef MOZ_FMP4
#include "MP4Decoder.h"
#endif
#ifdef XP_WIN
#include "mozilla/WindowsVersion.h"
#include "WMFDecoderModule.h"
#endif
#ifdef XP_MACOSX
#include "nsCocoaFeatures.h"
#endif
#include "nsContentCID.h"
#include "nsServiceManagerUtils.h"
#include "mozIGeckoMediaPluginService.h"
#include "VideoUtils.h"
#include "mozilla/Services.h"
#include "nsIObserverService.h"
#include "mozilla/EMEUtils.h"
#include "GMPUtils.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsXULAppAPI.h"
#include "gmp-audio-decode.h"
#include "gmp-video-decode.h"
#include "DecoderDoctorDiagnostics.h"

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MediaKeySystemAccess,
                                      mParent)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MediaKeySystemAccess)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MediaKeySystemAccess)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MediaKeySystemAccess)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

MediaKeySystemAccess::MediaKeySystemAccess(nsPIDOMWindowInner* aParent,
                                           const nsAString& aKeySystem,
                                           const nsAString& aCDMVersion,
                                           const MediaKeySystemConfiguration& aConfig)
  : mParent(aParent)
  , mKeySystem(aKeySystem)
  , mCDMVersion(aCDMVersion)
  , mConfig(aConfig)
{
}

MediaKeySystemAccess::~MediaKeySystemAccess()
{
}

JSObject*
MediaKeySystemAccess::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return MediaKeySystemAccessBinding::Wrap(aCx, this, aGivenProto);
}

nsPIDOMWindowInner*
MediaKeySystemAccess::GetParentObject() const
{
  return mParent;
}

void
MediaKeySystemAccess::GetKeySystem(nsString& aOutKeySystem) const
{
  aOutKeySystem.Assign(mKeySystem);
}

void
MediaKeySystemAccess::GetConfiguration(MediaKeySystemConfiguration& aConfig)
{
  aConfig = mConfig;
}

already_AddRefed<Promise>
MediaKeySystemAccess::CreateMediaKeys(ErrorResult& aRv)
{
  RefPtr<MediaKeys> keys(new MediaKeys(mParent, mKeySystem, mCDMVersion));
  return keys->Init(aRv);
}

static bool
HaveGMPFor(mozIGeckoMediaPluginService* aGMPService,
           const nsCString& aKeySystem,
           const nsCString& aAPI,
           const nsCString& aTag = EmptyCString())
{
  nsTArray<nsCString> tags;
  tags.AppendElement(aKeySystem);
  if (!aTag.IsEmpty()) {
    tags.AppendElement(aTag);
  }
  bool hasPlugin = false;
  if (NS_FAILED(aGMPService->HasPluginForAPI(aAPI,
                                             &tags,
                                             &hasPlugin))) {
    return false;
  }
  return hasPlugin;
}

#ifdef XP_WIN
static bool
AdobePluginFileExists(const nsACString& aVersionStr,
                      const nsAString& aFilename)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);

  nsCOMPtr<nsIFile> path;
  nsresult rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(path));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return false;
  }

  rv = path->Append(NS_LITERAL_STRING("gmp-eme-adobe"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return false;
  }

  rv = path->AppendNative(aVersionStr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return false;
  }

  rv = path->Append(aFilename);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return false;
  }

  bool exists = false;
  return NS_SUCCEEDED(path->Exists(&exists)) && exists;
}

static bool
AdobePluginDLLExists(const nsACString& aVersionStr)
{
  return AdobePluginFileExists(aVersionStr, NS_LITERAL_STRING("eme-adobe.dll"));
}

static bool
AdobePluginVoucherExists(const nsACString& aVersionStr)
{
  return AdobePluginFileExists(aVersionStr, NS_LITERAL_STRING("eme-adobe.voucher"));
}
#endif

/* static */ bool
MediaKeySystemAccess::IsGMPPresentOnDisk(const nsAString& aKeySystem,
                                         const nsACString& aVersion,
                                         nsACString& aOutMessage)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (XRE_GetProcessType() != GeckoProcessType_Default) {
    // We need to be able to access the filesystem, so call this in the
    // main process via ContentChild.
    ContentChild* contentChild = ContentChild::GetSingleton();
    if (NS_WARN_IF(!contentChild)) {
      return false;
    }

    nsCString message;
    bool result = false;
    bool ok = contentChild->SendIsGMPPresentOnDisk(nsString(aKeySystem), nsCString(aVersion),
                                                   &result, &message);
    aOutMessage = message;
    return ok && result;
  }

  bool isPresent = true;

#if XP_WIN
  if (aKeySystem.EqualsLiteral("com.adobe.primetime")) {
    if (!AdobePluginDLLExists(aVersion)) {
      NS_WARNING("Adobe EME plugin disappeared from disk!");
      aOutMessage = NS_LITERAL_CSTRING("Adobe DLL was expected to be on disk but was not");
      isPresent = false;
    }
    if (!AdobePluginVoucherExists(aVersion)) {
      NS_WARNING("Adobe EME voucher disappeared from disk!");
      aOutMessage = NS_LITERAL_CSTRING("Adobe plugin voucher was expected to be on disk but was not");
      isPresent = false;
    }

    if (!isPresent) {
      // Reset the prefs that Firefox's GMP downloader sets, so that
      // Firefox will try to download the plugin next time the updater runs.
      Preferences::ClearUser("media.gmp-eme-adobe.lastUpdate");
      Preferences::ClearUser("media.gmp-eme-adobe.version");
    } else if (!EMEVoucherFileExists()) {
      // Gecko doesn't have a voucher file for the plugin-container.
      // Adobe EME isn't going to work, so don't advertise that it will.
      aOutMessage = NS_LITERAL_CSTRING("Plugin-container voucher not present");
      isPresent = false;
    }
  }
#endif

  return isPresent;
}

static MediaKeySystemStatus
EnsureMinCDMVersion(mozIGeckoMediaPluginService* aGMPService,
                    const nsAString& aKeySystem,
                    int32_t aMinCdmVersion,
                    nsACString& aOutMessage,
                    nsACString& aOutCdmVersion)
{
  nsTArray<nsCString> tags;
  tags.AppendElement(NS_ConvertUTF16toUTF8(aKeySystem));
  bool hasPlugin;
  nsAutoCString versionStr;
  if (NS_FAILED(aGMPService->GetPluginVersionForAPI(NS_LITERAL_CSTRING(GMP_API_DECRYPTOR),
                                                    &tags,
                                                    &hasPlugin,
                                                    versionStr))) {
    aOutMessage = NS_LITERAL_CSTRING("GetPluginVersionForAPI failed");
    return MediaKeySystemStatus::Error;
  }

  aOutCdmVersion = versionStr;

  if (!hasPlugin) {
    aOutMessage = NS_LITERAL_CSTRING("CDM is not installed");
    return MediaKeySystemStatus::Cdm_not_installed;
  }

  if (!MediaKeySystemAccess::IsGMPPresentOnDisk(aKeySystem, versionStr, aOutMessage)) {
    return MediaKeySystemStatus::Cdm_not_installed;
  }

  nsresult rv;
  int32_t version = versionStr.ToInteger(&rv);
  if (aMinCdmVersion != NO_CDM_VERSION &&
      (NS_FAILED(rv) || version < 0 || aMinCdmVersion > version)) {
    aOutMessage = NS_LITERAL_CSTRING("Installed CDM version insufficient");
    return MediaKeySystemStatus::Cdm_insufficient_version;
  }

  return MediaKeySystemStatus::Available;
}

/* static */
MediaKeySystemStatus
MediaKeySystemAccess::GetKeySystemStatus(const nsAString& aKeySystem,
                                         int32_t aMinCdmVersion,
                                         nsACString& aOutMessage,
                                         nsACString& aOutCdmVersion)
{
  MOZ_ASSERT(MediaPrefs::EMEEnabled());
  nsCOMPtr<mozIGeckoMediaPluginService> mps =
    do_GetService("@mozilla.org/gecko-media-plugin-service;1");
  if (NS_WARN_IF(!mps)) {
    aOutMessage = NS_LITERAL_CSTRING("Failed to get GMP service");
    return MediaKeySystemStatus::Error;
  }

  if (aKeySystem.EqualsLiteral("org.w3.clearkey")) {
    if (!Preferences::GetBool("media.eme.clearkey.enabled", true)) {
      aOutMessage = NS_LITERAL_CSTRING("ClearKey was disabled");
      return MediaKeySystemStatus::Cdm_disabled;
    }
    return EnsureMinCDMVersion(mps, aKeySystem, aMinCdmVersion, aOutMessage, aOutCdmVersion);
  }

  if (Preferences::GetBool("media.gmp-eme-adobe.visible", false)) {
    if (aKeySystem.EqualsLiteral("com.adobe.primetime")) {
      if (!Preferences::GetBool("media.gmp-eme-adobe.enabled", false)) {
        aOutMessage = NS_LITERAL_CSTRING("Adobe EME disabled");
        return MediaKeySystemStatus::Cdm_disabled;
      }
#ifdef XP_WIN
      // Win Vista and later only.
      if (!IsVistaOrLater()) {
        aOutMessage = NS_LITERAL_CSTRING("Minimum Windows version (Vista) not met for Adobe EME");
        return MediaKeySystemStatus::Cdm_not_supported;
      }
#endif
#ifdef XP_MACOSX
      if (!nsCocoaFeatures::OnLionOrLater()) {
        aOutMessage = NS_LITERAL_CSTRING("Minimum MacOSX version (10.7) not met for Adobe EME");
        return MediaKeySystemStatus::Cdm_not_supported;
      }
#endif
      return EnsureMinCDMVersion(mps, aKeySystem, aMinCdmVersion, aOutMessage, aOutCdmVersion);
    }
  }

  if (Preferences::GetBool("media.gmp-widevinecdm.visible", false)) {
    if (aKeySystem.EqualsLiteral("com.widevine.alpha")) {
#ifdef XP_WIN
      // Win Vista and later only.
      if (!IsVistaOrLater()) {
        aOutMessage = NS_LITERAL_CSTRING("Minimum Windows version (Vista) not met for Widevine EME");
        return MediaKeySystemStatus::Cdm_not_supported;
      }
#endif
#ifdef XP_MACOSX
      if (!nsCocoaFeatures::OnLionOrLater()) {
        aOutMessage = NS_LITERAL_CSTRING("Minimum MacOSX version (10.7) not met for Widevine EME");
        return MediaKeySystemStatus::Cdm_not_supported;
      }
#endif
      if (!Preferences::GetBool("media.gmp-widevinecdm.enabled", false)) {
        aOutMessage = NS_LITERAL_CSTRING("Widevine EME disabled");
        return MediaKeySystemStatus::Cdm_disabled;
      }
      return EnsureMinCDMVersion(mps, aKeySystem, aMinCdmVersion, aOutMessage, aOutCdmVersion);
    }
  }

  return MediaKeySystemStatus::Cdm_not_supported;
}

static bool
GMPDecryptsAndDecodesAAC(mozIGeckoMediaPluginService* aGMPS,
                         const nsAString& aKeySystem,
                         DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(HaveGMPFor(aGMPS,
                        NS_ConvertUTF16toUTF8(aKeySystem),
                        NS_LITERAL_CSTRING(GMP_API_DECRYPTOR)));
  return HaveGMPFor(aGMPS,
                    NS_ConvertUTF16toUTF8(aKeySystem),
                    NS_LITERAL_CSTRING(GMP_API_AUDIO_DECODER),
                    NS_LITERAL_CSTRING("aac"));
}

static bool
GMPDecryptsAndDecodesH264(mozIGeckoMediaPluginService* aGMPS,
                          const nsAString& aKeySystem,
                          DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(HaveGMPFor(aGMPS,
                        NS_ConvertUTF16toUTF8(aKeySystem),
                        NS_LITERAL_CSTRING(GMP_API_DECRYPTOR)));
  return HaveGMPFor(aGMPS,
                    NS_ConvertUTF16toUTF8(aKeySystem),
                    NS_LITERAL_CSTRING(GMP_API_VIDEO_DECODER),
                    NS_LITERAL_CSTRING("h264"));
}

// If this keysystem's CDM explicitly says it doesn't support decoding,
// that means it's OK with passing the decrypted samples back to Gecko
// for decoding.
static bool
GMPDecryptsAndGeckoDecodesH264(mozIGeckoMediaPluginService* aGMPService,
                               const nsAString& aKeySystem,
                               const nsAString& aContentType,
                               DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(HaveGMPFor(aGMPService,
                        NS_ConvertUTF16toUTF8(aKeySystem),
                        NS_LITERAL_CSTRING(GMP_API_DECRYPTOR)));
  MOZ_ASSERT(IsH264ContentType(aContentType));
  return !HaveGMPFor(aGMPService,
                     NS_ConvertUTF16toUTF8(aKeySystem),
                     NS_LITERAL_CSTRING(GMP_API_VIDEO_DECODER),
                     NS_LITERAL_CSTRING("h264")) &&
         MP4Decoder::CanHandleMediaType(aContentType, aDiagnostics);
}

static bool
GMPDecryptsAndGeckoDecodesAAC(mozIGeckoMediaPluginService* aGMPService,
                              const nsAString& aKeySystem,
                              const nsAString& aContentType,
                              DecoderDoctorDiagnostics* aDiagnostics)
{
  MOZ_ASSERT(HaveGMPFor(aGMPService,
                        NS_ConvertUTF16toUTF8(aKeySystem),
                        NS_LITERAL_CSTRING(GMP_API_DECRYPTOR)));
  MOZ_ASSERT(IsAACContentType(aContentType));

  if (HaveGMPFor(aGMPService,
    NS_ConvertUTF16toUTF8(aKeySystem),
    NS_LITERAL_CSTRING(GMP_API_AUDIO_DECODER),
    NS_LITERAL_CSTRING("aac"))) {
    // We do have a GMP for AAC -> Gecko itself does *not* decode AAC.
    return false;
  }
#if defined(MOZ_WIDEVINE_EME) && defined(XP_WIN)
  // Widevine CDM doesn't include an AAC decoder. So if WMF can't
  // decode AAC, and a codec wasn't specified, be conservative
  // and reject the MediaKeys request, since our policy is to prevent
  //  the Adobe GMP's unencrypted AAC decoding path being used to
  // decode content decrypted by the Widevine CDM.
  if (aKeySystem.EqualsLiteral("com.widevine.alpha") &&
      !WMFDecoderModule::HasAAC()) {
    if (aDiagnostics) {
      aDiagnostics->SetKeySystemIssue(
        DecoderDoctorDiagnostics::eWidevineWithNoWMF);
    }
    return false;
  }
#endif
  return MP4Decoder::CanHandleMediaType(aContentType, aDiagnostics);
}

static bool
IsSupportedAudio(mozIGeckoMediaPluginService* aGMPService,
                 const nsAString& aKeySystem,
                 const nsAString& aAudioType,
                 DecoderDoctorDiagnostics* aDiagnostics)
{
  return IsAACContentType(aAudioType) &&
         (GMPDecryptsAndDecodesAAC(aGMPService, aKeySystem, aDiagnostics) ||
          GMPDecryptsAndGeckoDecodesAAC(aGMPService, aKeySystem, aAudioType, aDiagnostics));
}

static bool
IsSupportedVideo(mozIGeckoMediaPluginService* aGMPService,
                 const nsAString& aKeySystem,
                 const nsAString& aVideoType,
                 DecoderDoctorDiagnostics* aDiagnostics)
{
  return IsH264ContentType(aVideoType) &&
         (GMPDecryptsAndDecodesH264(aGMPService, aKeySystem, aDiagnostics) ||
          GMPDecryptsAndGeckoDecodesH264(aGMPService, aKeySystem, aVideoType, aDiagnostics));
}

static bool
IsSupported(mozIGeckoMediaPluginService* aGMPService,
            const nsAString& aKeySystem,
            const MediaKeySystemConfiguration& aConfig,
            DecoderDoctorDiagnostics* aDiagnostics)
{
  if (aConfig.mInitDataType.IsEmpty() &&
      aConfig.mAudioType.IsEmpty() &&
      aConfig.mVideoType.IsEmpty()) {
    // Not an old-style request.
    return false;
  }

  // Backwards compatibility with legacy MediaKeySystemConfiguration method.
  if (!aConfig.mInitDataType.IsEmpty() &&
      !aConfig.mInitDataType.EqualsLiteral("cenc")) {
    return false;
  }
  if (!aConfig.mAudioType.IsEmpty() &&
      !IsSupportedAudio(aGMPService, aKeySystem, aConfig.mAudioType, aDiagnostics)) {
    return false;
  }
  if (!aConfig.mVideoType.IsEmpty() &&
      !IsSupportedVideo(aGMPService, aKeySystem, aConfig.mVideoType, aDiagnostics)) {
    return false;
  }

  return true;
}

static bool
IsSupportedInitDataType(const nsString& aCandidate, const nsAString& aKeySystem)
{
  // All supported keySystems can handle "cenc" initDataType.
  // ClearKey also supports "keyids" and "webm" initDataTypes.
  return aCandidate.EqualsLiteral("cenc") ||
    ((aKeySystem.EqualsLiteral("org.w3.clearkey")
#ifdef MOZ_WIDEVINE_EME
    || aKeySystem.EqualsLiteral("com.widevine.alpha")
#endif
    ) &&
    (aCandidate.EqualsLiteral("keyids") || aCandidate.EqualsLiteral("webm)")));
}

static bool
GetSupportedConfig(mozIGeckoMediaPluginService* aGMPService,
                   const nsAString& aKeySystem,
                   const MediaKeySystemConfiguration& aCandidate,
                   MediaKeySystemConfiguration& aOutConfig,
                   DecoderDoctorDiagnostics* aDiagnostics)
{
  MediaKeySystemConfiguration config;
  config.mLabel = aCandidate.mLabel;
  if (aCandidate.mInitDataTypes.WasPassed()) {
    nsTArray<nsString> initDataTypes;
    for (const nsString& candidate : aCandidate.mInitDataTypes.Value()) {
      if (IsSupportedInitDataType(candidate, aKeySystem)) {
        initDataTypes.AppendElement(candidate);
      }
    }
    if (initDataTypes.IsEmpty()) {
      return false;
    }
    config.mInitDataTypes.Construct();
    config.mInitDataTypes.Value().Assign(initDataTypes);
  }
  if (aCandidate.mAudioCapabilities.WasPassed()) {
    nsTArray<MediaKeySystemMediaCapability> caps;
    for (const MediaKeySystemMediaCapability& cap : aCandidate.mAudioCapabilities.Value()) {
      if (IsSupportedAudio(aGMPService, aKeySystem, cap.mContentType, aDiagnostics)) {
        caps.AppendElement(cap);
      }
    }
    if (caps.IsEmpty()) {
      return false;
    }
    config.mAudioCapabilities.Construct();
    config.mAudioCapabilities.Value().Assign(caps);
  }
  if (aCandidate.mVideoCapabilities.WasPassed()) {
    nsTArray<MediaKeySystemMediaCapability> caps;
    for (const MediaKeySystemMediaCapability& cap : aCandidate.mVideoCapabilities.Value()) {
      if (IsSupportedVideo(aGMPService, aKeySystem, cap.mContentType, aDiagnostics)) {
        caps.AppendElement(cap);
      }
    }
    if (caps.IsEmpty()) {
      return false;
    }
    config.mVideoCapabilities.Construct();
    config.mVideoCapabilities.Value().Assign(caps);
  }

#if defined(MOZ_WIDEVINE_EME) && defined(XP_WIN)
  // Widevine CDM doesn't include an AAC decoder. So if WMF can't decode AAC,
  // and a codec wasn't specified, be conservative and reject the MediaKeys request.
  if (aKeySystem.EqualsLiteral("com.widevine.alpha") &&
      (!aCandidate.mAudioCapabilities.WasPassed() ||
       !aCandidate.mVideoCapabilities.WasPassed()) &&
     !WMFDecoderModule::HasAAC()) {
    if (aDiagnostics) {
      aDiagnostics->SetKeySystemIssue(
        DecoderDoctorDiagnostics::eWidevineWithNoWMF);
    }
    return false;
  }
#endif

  aOutConfig = config;

  return true;
}

// Backwards compatibility with legacy requestMediaKeySystemAccess with fields
// from old MediaKeySystemOptions dictionary.
/* static */
bool
MediaKeySystemAccess::IsSupported(const nsAString& aKeySystem,
                                  const Sequence<MediaKeySystemConfiguration>& aConfigs,
                                  DecoderDoctorDiagnostics* aDiagnostics)
{
  nsCOMPtr<mozIGeckoMediaPluginService> mps =
    do_GetService("@mozilla.org/gecko-media-plugin-service;1");
  if (NS_WARN_IF(!mps)) {
    return false;
  }

  if (!HaveGMPFor(mps,
                  NS_ConvertUTF16toUTF8(aKeySystem),
                  NS_LITERAL_CSTRING(GMP_API_DECRYPTOR))) {
    return false;
  }

  for (const MediaKeySystemConfiguration& config : aConfigs) {
    if (mozilla::dom::IsSupported(mps, aKeySystem, config, aDiagnostics)) {
      return true;
    }
  }
  return false;
}

/* static */
bool
MediaKeySystemAccess::GetSupportedConfig(const nsAString& aKeySystem,
                                         const Sequence<MediaKeySystemConfiguration>& aConfigs,
                                         MediaKeySystemConfiguration& aOutConfig,
                                         DecoderDoctorDiagnostics* aDiagnostics)
{
  nsCOMPtr<mozIGeckoMediaPluginService> mps =
    do_GetService("@mozilla.org/gecko-media-plugin-service;1");
  if (NS_WARN_IF(!mps)) {
    return false;
  }

  if (!HaveGMPFor(mps,
                  NS_ConvertUTF16toUTF8(aKeySystem),
                  NS_LITERAL_CSTRING(GMP_API_DECRYPTOR))) {
    return false;
  }

  for (const MediaKeySystemConfiguration& config : aConfigs) {
    if (mozilla::dom::GetSupportedConfig(
          mps, aKeySystem, config, aOutConfig, aDiagnostics)) {
      return true;
    }
  }

  return false;
}


/* static */
void
MediaKeySystemAccess::NotifyObservers(nsPIDOMWindowInner* aWindow,
                                      const nsAString& aKeySystem,
                                      MediaKeySystemStatus aStatus)
{
  RequestMediaKeySystemAccessNotification data;
  data.mKeySystem = aKeySystem;
  data.mStatus = aStatus;
  nsAutoString json;
  data.ToJSON(json);
  EME_LOG("MediaKeySystemAccess::NotifyObservers() %s", NS_ConvertUTF16toUTF8(json).get());
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (obs) {
    obs->NotifyObservers(aWindow, "mediakeys-request", json.get());
  }
}

} // namespace dom
} // namespace mozilla
