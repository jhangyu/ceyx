// Minimal stubs for dng_xmp to satisfy linker when qDNGXMPDocOps=0 and
// qDNGXMPFiles=0
#include "dng_fingerprint.h"
#include "dng_memory.h"
#include "dng_orientation.h"
#include "dng_string.h"
#include "dng_string_list.h"
#include "dng_xmp.h"

const char *XMP_NS_TIFF = "http://ns.adobe.com/tiff/1.0/";
const char *XMP_NS_EXIF = "http://ns.adobe.com/exif/1.0/";
const char *XMP_NS_PHOTOSHOP = "http://ns.adobe.com/photoshop/1.0/";
const char *XMP_NS_XAP = "http://ns.adobe.com/xap/1.0/";
const char *XMP_NS_XAP_RIGHTS = "http://ns.adobe.com/xap/1.0/rights/";
const char *XMP_NS_DC = "http://purl.org/dc/elements/1.1/";
const char *XMP_NS_XMP_NOTE = "http://ns.adobe.com/xmp/note/";
const char *XMP_NS_MM = "http://ns.adobe.com/xap/1.0/mm/";
const char *XMP_NS_CRS = "http://ns.adobe.com/camera-raw-settings/1.0/";
const char *XMP_NS_CRSS = "http://ns.adobe.com/camera-raw-saved-settings/1.0/";
const char *XMP_NS_AUX = "http://ns.adobe.com/exif/1.0/aux/";
const char *XMP_NS_LCP = "http://ns.adobe.com/photoshop/1.0/camera-profile";
const char *XMP_NS_IPTC = "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/";
const char *XMP_NS_IPTC_EXT = "http://iptc.org/std/Iptc4xmpExt/2008-02-29/";
const char *XMP_NS_CRX =
    "http://ns.adobe.com/lightroom-settings-experimental/1.0/";
const char *XMP_NS_DNG = "http://ns.adobe.com/dng/1.0/";

dng_xmp::dng_xmp(dng_memory_allocator &a) : fAllocator(a), fSDK(nullptr) {}
dng_xmp::dng_xmp(const dng_xmp &xmp)
    : fAllocator(xmp.fAllocator), fSDK(nullptr) {}
dng_xmp::~dng_xmp() {}
dng_xmp *dng_xmp::Clone() const { return nullptr; }

void dng_xmp::Parse(dng_host &, const void *, uint32) {}
dng_memory_block *dng_xmp::Serialize(bool, uint32, uint32, bool, bool) const {
  return nullptr;
}
void dng_xmp::PackageForJPEG(AutoPtr<dng_memory_block> &,
                             AutoPtr<dng_memory_block> &, dng_string &) const {}
void dng_xmp::MergeFromJPEG(const dng_xmp &) {}
bool dng_xmp::HasMeta() const { return false; }
void *dng_xmp::GetPrivateMeta() { return nullptr; }
bool dng_xmp::Exists(const char *, const char *) const { return false; }
bool dng_xmp::HasNameSpace(const char *) const { return false; }
bool dng_xmp::IteratePaths(IteratePathsCallback *, void *, const char *,
                           const char *) {
  return false;
}

void dng_xmp::Remove(const char *, const char *) {}
void dng_xmp::RemoveProperties(const char *) {}
void dng_xmp::RemoveEmptyStringOrArray(const char *, const char *) {}
void dng_xmp::RemoveEmptyStringsAndArrays(const char *) {}

void dng_xmp::Set(const char *, const char *, const char *) {}
bool dng_xmp::GetString(const char *, const char *, dng_string &) const {
  return false;
}
void dng_xmp::SetString(const char *, const char *, const dng_string &) {}
bool dng_xmp::GetStringList(const char *, const char *,
                            dng_string_list &) const {
  return false;
}
void dng_xmp::SetStringList(const char *, const char *, const dng_string_list &,
                            bool) {}

void dng_xmp::SetStructField(const char *, const char *, const char *,
                             const char *, const dng_string &) {}
void dng_xmp::SetStructField(const char *, const char *, const char *,
                             const char *, const char *) {}
void dng_xmp::DeleteStructField(const char *, const char *, const char *,
                                const char *) {}
bool dng_xmp::GetStructField(const char *, const char *, const char *,
                             const char *, dng_string &) const {
  return false;
}

void dng_xmp::SetAltLangDefault(const char *, const char *,
                                const dng_string &) {}
bool dng_xmp::GetAltLangDefault(const char *, const char *,
                                dng_string &) const {
  return false;
}

bool dng_xmp::GetBoolean(const char *, const char *, bool &) const {
  return false;
}
void dng_xmp::SetBoolean(const char *, const char *, bool) {}

bool dng_xmp::Get_int32(const char *, const char *, int32 &) const {
  return false;
}
void dng_xmp::Set_int32(const char *, const char *, int32, bool) {}

bool dng_xmp::Get_uint32(const char *, const char *, uint32 &) const {
  return false;
}
void dng_xmp::Set_uint32(const char *, const char *, uint32) {}

bool dng_xmp::Get_real64(const char *, const char *, real64 &) const {
  return false;
}
void dng_xmp::Set_real64(const char *, const char *, real64, uint32, bool,
                         bool) {}

bool dng_xmp::Get_urational(const char *, const char *, dng_urational &) const {
  return false;
}
void dng_xmp::Set_urational(const char *, const char *, const dng_urational &) {
}

bool dng_xmp::Get_srational(const char *, const char *, dng_srational &) const {
  return false;
}
void dng_xmp::Set_srational(const char *, const char *, const dng_srational &) {
}

bool dng_xmp::GetFingerprint(const char *, const char *,
                             dng_fingerprint &) const {
  return false;
}
void dng_xmp::SetFingerprint(const char *, const char *,
                             const dng_fingerprint &, bool) {}
void dng_xmp::SetVersion2to4(const char *, const char *, uint32) {}

dng_fingerprint dng_xmp::GetIPTCDigest() const { return dng_fingerprint(); }
void dng_xmp::SetIPTCDigest(dng_fingerprint &) {}
void dng_xmp::ClearIPTCDigest() {}
void dng_xmp::IngestIPTC(dng_metadata &, bool) {}
void dng_xmp::RebuildIPTC(dng_metadata &, dng_memory_allocator &, bool) {}

void dng_xmp::SyncExif(dng_exif &, const dng_exif *, bool, bool) {}
void dng_xmp::ValidateStringList(const char *, const char *) {}
void dng_xmp::ValidateMetadata() {}
void dng_xmp::UpdateDateTime(const dng_date_time_info &) {}
void dng_xmp::UpdateMetadataDate(const dng_date_time_info &) {}
void dng_xmp::UpdateExifDates(dng_exif &, bool) {}

bool dng_xmp::HasOrientation() const { return false; }
dng_orientation dng_xmp::GetOrientation() const {
  return dng_orientation::Unknown();
}
void dng_xmp::ClearOrientation() {}
void dng_xmp::SetOrientation(const dng_orientation &) {}
void dng_xmp::SyncOrientation(dng_negative &, bool) {}
void dng_xmp::SyncOrientation(dng_metadata &, bool) {}

void dng_xmp::ClearImageInfo() {}
void dng_xmp::SetImageSize(const dng_point &) {}
void dng_xmp::SetSampleInfo(uint32, uint32) {}
void dng_xmp::SetPhotometricInterpretation(uint32) {}
void dng_xmp::SetResolution(const dng_resolution &) {}

void dng_xmp::ComposeArrayItemPath(const char *, const char *, int32,
                                   dng_string &) const {}
void dng_xmp::ComposeStructFieldPath(const char *, const char *, const char *,
                                     const char *, dng_string &) const {}
int32 dng_xmp::CountArrayItems(const char *, const char *) const { return 0; }
void dng_xmp::AppendArrayItem(const char *, const char *, const char *, bool,
                              bool) {}

dng_string dng_xmp::EncodeFingerprint(const dng_fingerprint &, bool) {
  return dng_string();
}
dng_fingerprint dng_xmp::DecodeFingerprint(const dng_string &) {
  return dng_fingerprint();
}

void dng_xmp::TrimDecimal(char *) {}
dng_string dng_xmp::EncodeGPSVersion(uint32) { return dng_string(); }
uint32 dng_xmp::DecodeGPSVersion(const dng_string &) { return 0; }
dng_string dng_xmp::EncodeGPSCoordinate(const dng_string &,
                                        const dng_urational *) {
  return dng_string();
}
void dng_xmp::DecodeGPSCoordinate(const dng_string &, dng_string &,
                                  dng_urational *) {}
dng_string dng_xmp::EncodeGPSDateTime(const dng_string &,
                                      const dng_urational *) {
  return dng_string();
}
void dng_xmp::DecodeGPSDateTime(const dng_string &, dng_string &,
                                dng_urational *) {}

bool dng_xmp::SyncString(const char *, const char *, dng_string &, uint32) {
  return false;
}
void dng_xmp::SyncStringList(const char *, const char *, dng_string_list &,
                             bool, uint32) {}
bool dng_xmp::SyncAltLangDefault(const char *, const char *, dng_string &,
                                 uint32) {
  return false;
}
void dng_xmp::Sync_uint32(const char *, const char *, uint32 &, bool, uint32) {}
void dng_xmp::Sync_uint32_array(const char *, const char *, uint32 *, uint32 &,
                                uint32, uint32) {}
void dng_xmp::Sync_urational(const char *, const char *, dng_urational &,
                             uint32) {}
void dng_xmp::Sync_srational(const char *, const char *, dng_srational &,
                             uint32) {}
void dng_xmp::SyncIPTC(dng_iptc &, uint32) {}
void dng_xmp::SyncFlash(uint32 &, uint32 &, uint32) {}
bool dng_xmp::DateTimeIsDateOnly(const char *, const char *) { return false; }
void dng_xmp::SyncApproximateFocusDistance(dng_exif &, const uint32) {}

#if qDNGXMPDocOps
void dng_xmp::DocOpsOpenXMP(const char *) {}
void dng_xmp::DocOpsPrepareForSave(const char *, const char *, bool) {}
void dng_xmp::DocOpsUpdateMetadata(const char *) {}
#endif
