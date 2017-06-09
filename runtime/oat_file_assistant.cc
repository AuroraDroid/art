/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "oat_file_assistant.h"

#include <sstream>

#include <sys/stat.h>

#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "compiler_filter.h"
#include "class_linker.h"
#include "exec_utils.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "image.h"
#include "oat.h"
#include "os.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "utils.h"
#include "vdex_file.h"

namespace art {

using android::base::StringPrintf;

std::ostream& operator << (std::ostream& stream, const OatFileAssistant::OatStatus status) {
  switch (status) {
    case OatFileAssistant::kOatCannotOpen:
      stream << "kOatCannotOpen";
      break;
    case OatFileAssistant::kOatDexOutOfDate:
      stream << "kOatDexOutOfDate";
      break;
    case OatFileAssistant::kOatBootImageOutOfDate:
      stream << "kOatBootImageOutOfDate";
      break;
    case OatFileAssistant::kOatRelocationOutOfDate:
      stream << "kOatRelocationOutOfDate";
      break;
    case OatFileAssistant::kOatUpToDate:
      stream << "kOatUpToDate";
      break;
    default:
      UNREACHABLE();
  }

  return stream;
}

OatFileAssistant::OatFileAssistant(const char* dex_location,
                                   const InstructionSet isa,
                                   bool load_executable)
    : isa_(isa),
      load_executable_(load_executable),
      odex_(this, /*is_oat_location*/ false),
      oat_(this, /*is_oat_location*/ true) {
  CHECK(dex_location != nullptr) << "OatFileAssistant: null dex location";

  // Try to get the realpath for the dex location.
  //
  // This is OK with respect to dalvik cache naming scheme because we never
  // generate oat files starting from symlinks which go into dalvik cache.
  // (recall that the oat files in dalvik cache are encoded by replacing '/'
  // with '@' in the path).
  // The boot image oat files (which are symlinked in dalvik-cache) are not
  // loaded via the oat file assistant.
  //
  // The only case when the dex location may resolve to a different path
  // is for secondary dex files (e.g. /data/user/0 symlinks to /data/data and
  // the app is free to create its own internal layout). Related to this it is
  // worthwhile to mention that installd resolves the secondary dex location
  // before calling dex2oat.
  UniqueCPtr<const char[]> dex_location_real(realpath(dex_location, nullptr));
  if (dex_location_real != nullptr) {
    dex_location_.assign(dex_location_real.get());
  } else {
    // If we can't get the realpath of the location there's not much point in trying to move on.
    PLOG(ERROR) << "Could not get the realpath of dex_location " << dex_location;
    return;
  }

  if (load_executable_ && isa != kRuntimeISA) {
    LOG(WARNING) << "OatFileAssistant: Load executable specified, "
      << "but isa is not kRuntimeISA. Will not attempt to load executable.";
    load_executable_ = false;
  }

  // Get the odex filename.
  std::string error_msg;
  std::string odex_file_name;
  if (DexLocationToOdexFilename(dex_location_, isa_, &odex_file_name, &error_msg)) {
    odex_.Reset(odex_file_name);
  } else {
    LOG(WARNING) << "Failed to determine odex file name: " << error_msg;
  }

  // Get the oat filename.
  std::string oat_file_name;
  if (DexLocationToOatFilename(dex_location_, isa_, &oat_file_name, &error_msg)) {
    oat_.Reset(oat_file_name);
  } else {
    LOG(WARNING) << "Failed to determine oat file name for dex location "
        << dex_location_ << ": " << error_msg;
  }

  // Check if the dex directory is writable.
  // This will be needed in most uses of OatFileAssistant and so it's OK to
  // compute it eagerly. (the only use which will not make use of it is
  // OatFileAssistant::GetStatusDump())
  size_t pos = dex_location_.rfind('/');
  if (pos == std::string::npos) {
    LOG(WARNING) << "Failed to determine dex file parent directory: " << dex_location_;
  } else {
    std::string parent = dex_location_.substr(0, pos);
    if (access(parent.c_str(), W_OK) == 0) {
      dex_parent_writable_ = true;
    } else {
      VLOG(oat) << "Dex parent of " << dex_location_ << " is not writable: " << strerror(errno);
    }
  }
}

OatFileAssistant::~OatFileAssistant() {
  // Clean up the lock file.
  if (flock_.HasFile()) {
    unlink(flock_.GetFile()->GetPath().c_str());
  }
}

bool OatFileAssistant::IsInBootClassPath() {
  // Note: We check the current boot class path, regardless of the ISA
  // specified by the user. This is okay, because the boot class path should
  // be the same for all ISAs.
  // TODO: Can we verify the boot class path is the same for all ISAs?
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const auto& boot_class_path = class_linker->GetBootClassPath();
  for (size_t i = 0; i < boot_class_path.size(); i++) {
    if (boot_class_path[i]->GetLocation() == dex_location_) {
      VLOG(oat) << "Dex location " << dex_location_ << " is in boot class path";
      return true;
    }
  }
  return false;
}

bool OatFileAssistant::Lock(std::string* error_msg) {
  CHECK(error_msg != nullptr);
  CHECK(!flock_.HasFile()) << "OatFileAssistant::Lock already acquired";

  // Note the lock will only succeed for secondary dex files and in test
  // environment.
  //
  // The lock *will fail* for all primary apks in a production environment.
  // The app does not have permissions to create locks next to its dex location
  // (be it system, data or vendor parition). We also cannot use the odex or
  // oat location for the same reasoning.
  //
  // This is best effort and if it fails it's unlikely that we will be able
  // to generate oat files anyway.
  std::string lock_file_name = dex_location_ + "." + GetInstructionSetString(isa_) + ".flock";

  if (!flock_.Init(lock_file_name.c_str(), error_msg)) {
    unlink(lock_file_name.c_str());
    return false;
  }
  return true;
}

int OatFileAssistant::GetDexOptNeeded(CompilerFilter::Filter target, bool profile_changed) {
  OatFileInfo& info = GetBestInfo();
  DexOptNeeded dexopt_needed = info.GetDexOptNeeded(target, profile_changed);
  if (info.IsOatLocation() || dexopt_needed == kDex2OatFromScratch) {
    return dexopt_needed;
  }
  return -dexopt_needed;
}

// Figure out the currently specified compile filter option in the runtime.
// Returns true on success, false if the compiler filter is invalid, in which
// case error_msg describes the problem.
static bool GetRuntimeCompilerFilterOption(CompilerFilter::Filter* filter,
                                           std::string* error_msg) {
  CHECK(filter != nullptr);
  CHECK(error_msg != nullptr);

  *filter = OatFileAssistant::kDefaultCompilerFilterForDexLoading;
  for (StringPiece option : Runtime::Current()->GetCompilerOptions()) {
    if (option.starts_with("--compiler-filter=")) {
      const char* compiler_filter_string = option.substr(strlen("--compiler-filter=")).data();
      if (!CompilerFilter::ParseCompilerFilter(compiler_filter_string, filter)) {
        *error_msg = std::string("Unknown --compiler-filter value: ")
                   + std::string(compiler_filter_string);
        return false;
      }
    }
  }
  return true;
}

bool OatFileAssistant::IsUpToDate() {
  return GetBestInfo().Status() == kOatUpToDate;
}

OatFileAssistant::ResultOfAttemptToUpdate
OatFileAssistant::MakeUpToDate(bool profile_changed, std::string* error_msg) {
  CompilerFilter::Filter target;
  if (!GetRuntimeCompilerFilterOption(&target, error_msg)) {
    return kUpdateNotAttempted;
  }

  OatFileInfo& info = GetBestInfo();
  switch (info.GetDexOptNeeded(target, profile_changed)) {
    case kNoDexOptNeeded:
      return kUpdateSucceeded;

    // TODO: For now, don't bother with all the different ways we can call
    // dex2oat to generate the oat file. Always generate the oat file as if it
    // were kDex2OatFromScratch.
    case kDex2OatFromScratch:
    case kDex2OatForBootImage:
    case kDex2OatForRelocation:
    case kDex2OatForFilter:
      return GenerateOatFileNoChecks(info, target, error_msg);
  }
  UNREACHABLE();
}

std::unique_ptr<OatFile> OatFileAssistant::GetBestOatFile() {
  return GetBestInfo().ReleaseFileForUse();
}

std::string OatFileAssistant::GetStatusDump() {
  std::ostringstream status;
  bool oat_file_exists = false;
  bool odex_file_exists = false;
  if (oat_.Status() != kOatCannotOpen) {
    // If we can open the file, Filename should not return null.
    CHECK(oat_.Filename() != nullptr);

    oat_file_exists = true;
    status << *oat_.Filename() << "[status=" << oat_.Status() << ", ";
    const OatFile* file = oat_.GetFile();
    if (file == nullptr) {
      // If the file is null even though the status is not kOatCannotOpen, it
      // means we must have a vdex file with no corresponding oat file. In
      // this case we cannot determine the compilation filter. Indicate that
      // we have only the vdex file instead.
      status << "vdex-only";
    } else {
      status << "compilation_filter=" << CompilerFilter::NameOfFilter(file->GetCompilerFilter());
    }
  }

  if (odex_.Status() != kOatCannotOpen) {
    // If we can open the file, Filename should not return null.
    CHECK(odex_.Filename() != nullptr);

    odex_file_exists = true;
    if (oat_file_exists) {
      status << "] ";
    }
    status << *odex_.Filename() << "[status=" << odex_.Status() << ", ";
    const OatFile* file = odex_.GetFile();
    if (file == nullptr) {
      status << "vdex-only";
    } else {
      status << "compilation_filter=" << CompilerFilter::NameOfFilter(file->GetCompilerFilter());
    }
  }

  if (!oat_file_exists && !odex_file_exists) {
    status << "invalid[";
  }

  status << "]";
  return status.str();
}

std::vector<std::unique_ptr<const DexFile>> OatFileAssistant::LoadDexFiles(
    const OatFile& oat_file, const char* dex_location) {
  std::vector<std::unique_ptr<const DexFile>> dex_files;

  // Load the main dex file.
  std::string error_msg;
  const OatFile::OatDexFile* oat_dex_file = oat_file.GetOatDexFile(
      dex_location, nullptr, &error_msg);
  if (oat_dex_file == nullptr) {
    LOG(WARNING) << error_msg;
    return std::vector<std::unique_ptr<const DexFile>>();
  }

  std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
  if (dex_file.get() == nullptr) {
    LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
    return std::vector<std::unique_ptr<const DexFile>>();
  }
  dex_files.push_back(std::move(dex_file));

  // Load the rest of the multidex entries
  for (size_t i = 1; ; i++) {
    std::string multidex_dex_location = DexFile::GetMultiDexLocation(i, dex_location);
    oat_dex_file = oat_file.GetOatDexFile(multidex_dex_location.c_str(), nullptr);
    if (oat_dex_file == nullptr) {
      // There are no more multidex entries to load.
      break;
    }

    dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
      return std::vector<std::unique_ptr<const DexFile>>();
    }
    dex_files.push_back(std::move(dex_file));
  }
  return dex_files;
}

bool OatFileAssistant::HasOriginalDexFiles() {
  // Ensure GetRequiredDexChecksums has been run so that
  // has_original_dex_files_ is initialized. We don't care about the result of
  // GetRequiredDexChecksums.
  GetRequiredDexChecksums();
  return has_original_dex_files_;
}

OatFileAssistant::OatStatus OatFileAssistant::OdexFileStatus() {
  return odex_.Status();
}

OatFileAssistant::OatStatus OatFileAssistant::OatFileStatus() {
  return oat_.Status();
}

bool OatFileAssistant::DexChecksumUpToDate(const VdexFile& file, std::string* error_msg) {
  const std::vector<uint32_t>* required_dex_checksums = GetRequiredDexChecksums();
  if (required_dex_checksums == nullptr) {
    LOG(WARNING) << "Required dex checksums not found. Assuming dex checksums are up to date.";
    return true;
  }

  uint32_t number_of_dex_files = file.GetHeader().GetNumberOfDexFiles();
  if (required_dex_checksums->size() != number_of_dex_files) {
    *error_msg = StringPrintf("expected %zu dex files but found %u",
                              required_dex_checksums->size(),
                              number_of_dex_files);
    return false;
  }

  for (uint32_t i = 0; i < number_of_dex_files; i++) {
    uint32_t expected_checksum = (*required_dex_checksums)[i];
    uint32_t actual_checksum = file.GetLocationChecksum(i);
    if (expected_checksum != actual_checksum) {
      std::string dex = DexFile::GetMultiDexLocation(i, dex_location_.c_str());
      *error_msg = StringPrintf("Dex checksum does not match for dex: %s."
                                "Expected: %u, actual: %u",
                                dex.c_str(),
                                expected_checksum,
                                actual_checksum);
      return false;
    }
  }

  return true;
}

bool OatFileAssistant::DexChecksumUpToDate(const OatFile& file, std::string* error_msg) {
  const std::vector<uint32_t>* required_dex_checksums = GetRequiredDexChecksums();
  if (required_dex_checksums == nullptr) {
    LOG(WARNING) << "Required dex checksums not found. Assuming dex checksums are up to date.";
    return true;
  }

  uint32_t number_of_dex_files = file.GetOatHeader().GetDexFileCount();
  if (required_dex_checksums->size() != number_of_dex_files) {
    *error_msg = StringPrintf("expected %zu dex files but found %u",
                              required_dex_checksums->size(),
                              number_of_dex_files);
    return false;
  }

  for (uint32_t i = 0; i < number_of_dex_files; i++) {
    std::string dex = DexFile::GetMultiDexLocation(i, dex_location_.c_str());
    uint32_t expected_checksum = (*required_dex_checksums)[i];
    const OatFile::OatDexFile* oat_dex_file = file.GetOatDexFile(dex.c_str(), nullptr);
    if (oat_dex_file == nullptr) {
      *error_msg = StringPrintf("failed to find %s in %s", dex.c_str(), file.GetLocation().c_str());
      return false;
    }
    uint32_t actual_checksum = oat_dex_file->GetDexFileLocationChecksum();
    if (expected_checksum != actual_checksum) {
      VLOG(oat) << "Dex checksum does not match for dex: " << dex
        << ". Expected: " << expected_checksum
        << ", Actual: " << actual_checksum;
      return false;
    }
  }
  return true;
}

OatFileAssistant::OatStatus OatFileAssistant::GivenOatFileStatus(const OatFile& file) {
  // Verify the ART_USE_READ_BARRIER state.
  // TODO: Don't fully reject files due to read barrier state. If they contain
  // compiled code and are otherwise okay, we should return something like
  // kOatRelocationOutOfDate. If they don't contain compiled code, the read
  // barrier state doesn't matter.
  const bool is_cc = file.GetOatHeader().IsConcurrentCopying();
  constexpr bool kRuntimeIsCC = kUseReadBarrier;
  if (is_cc != kRuntimeIsCC) {
    return kOatCannotOpen;
  }

  // Verify the dex checksum.
  std::string error_msg;
  if (kIsVdexEnabled) {
    VdexFile* vdex = file.GetVdexFile();
    if (!DexChecksumUpToDate(*vdex, &error_msg)) {
      LOG(ERROR) << error_msg;
      return kOatDexOutOfDate;
    }
  } else {
    if (!DexChecksumUpToDate(file, &error_msg)) {
      LOG(ERROR) << error_msg;
      return kOatDexOutOfDate;
    }
  }

  CompilerFilter::Filter current_compiler_filter = file.GetCompilerFilter();

  // Verify the image checksum
  if (CompilerFilter::DependsOnImageChecksum(current_compiler_filter)) {
    const ImageInfo* image_info = GetImageInfo();
    if (image_info == nullptr) {
      VLOG(oat) << "No image for oat image checksum to match against.";

      if (HasOriginalDexFiles()) {
        return kOatBootImageOutOfDate;
      }

      // If there is no original dex file to fall back to, grudgingly accept
      // the oat file. This could technically lead to crashes, but there's no
      // way we could find a better oat file to use for this dex location,
      // and it's better than being stuck in a boot loop with no way out.
      // The problem will hopefully resolve itself the next time the runtime
      // starts up.
      LOG(WARNING) << "Dex location " << dex_location_ << " does not seem to include dex file. "
        << "Allow oat file use. This is potentially dangerous.";
    } else if (file.GetOatHeader().GetImageFileLocationOatChecksum() != image_info->oat_checksum) {
      VLOG(oat) << "Oat image checksum does not match image checksum.";
      return kOatBootImageOutOfDate;
    }
  } else {
    VLOG(oat) << "Image checksum test skipped for compiler filter " << current_compiler_filter;
  }

  if (CompilerFilter::IsAotCompilationEnabled(current_compiler_filter)) {
    if (!file.IsPic()) {
      const ImageInfo* image_info = GetImageInfo();
      if (image_info == nullptr) {
        VLOG(oat) << "No image to check oat relocation against.";
        return kOatRelocationOutOfDate;
      }

      // Verify the oat_data_begin recorded for the image in the oat file matches
      // the actual oat_data_begin for boot.oat in the image.
      const OatHeader& oat_header = file.GetOatHeader();
      uintptr_t oat_data_begin = oat_header.GetImageFileLocationOatDataBegin();
      if (oat_data_begin != image_info->oat_data_begin) {
        VLOG(oat) << file.GetLocation() <<
          ": Oat file image oat_data_begin (" << oat_data_begin << ")"
          << " does not match actual image oat_data_begin ("
          << image_info->oat_data_begin << ")";
        return kOatRelocationOutOfDate;
      }

      // Verify the oat_patch_delta recorded for the image in the oat file matches
      // the actual oat_patch_delta for the image.
      int32_t oat_patch_delta = oat_header.GetImagePatchDelta();
      if (oat_patch_delta != image_info->patch_delta) {
        VLOG(oat) << file.GetLocation() <<
          ": Oat file image patch delta (" << oat_patch_delta << ")"
          << " does not match actual image patch delta ("
          << image_info->patch_delta << ")";
        return kOatRelocationOutOfDate;
      }
    } else {
      // Oat files compiled in PIC mode do not require relocation.
      VLOG(oat) << "Oat relocation test skipped for PIC oat file";
    }
  } else {
    VLOG(oat) << "Oat relocation test skipped for compiler filter " << current_compiler_filter;
  }
  return kOatUpToDate;
}

static bool DexLocationToOdexNames(const std::string& location,
                                   InstructionSet isa,
                                   std::string* odex_filename,
                                   std::string* oat_dir,
                                   std::string* isa_dir,
                                   std::string* error_msg) {
  CHECK(odex_filename != nullptr);
  CHECK(error_msg != nullptr);

  // The odex file name is formed by replacing the dex_location extension with
  // .odex and inserting an oat/<isa> directory. For example:
  //   location = /foo/bar/baz.jar
  //   odex_location = /foo/bar/oat/<isa>/baz.odex

  // Find the directory portion of the dex location and add the oat/<isa>
  // directory.
  size_t pos = location.rfind('/');
  if (pos == std::string::npos) {
    *error_msg = "Dex location " + location + " has no directory.";
    return false;
  }
  std::string dir = location.substr(0, pos+1);
  // Add the oat directory.
  dir += "oat";
  if (oat_dir != nullptr) {
    *oat_dir = dir;
  }
  // Add the isa directory
  dir += "/" + std::string(GetInstructionSetString(isa));
  if (isa_dir != nullptr) {
    *isa_dir = dir;
  }

  // Get the base part of the file without the extension.
  std::string file = location.substr(pos+1);
  pos = file.rfind('.');
  if (pos == std::string::npos) {
    *error_msg = "Dex location " + location + " has no extension.";
    return false;
  }
  std::string base = file.substr(0, pos);

  *odex_filename = dir + "/" + base + ".odex";
  return true;
}

// Prepare a subcomponent of the odex directory.
// (i.e. create and set the expected permissions on the path `dir`).
static bool PrepareDirectory(const std::string& dir, std::string* error_msg) {
  struct stat dir_stat;
  if (TEMP_FAILURE_RETRY(stat(dir.c_str(), &dir_stat)) == 0) {
    // The directory exists. Check if it is indeed a directory.
    if (!S_ISDIR(dir_stat.st_mode)) {
      *error_msg = dir + " is not a dir";
      return false;
    } else {
      // The dir is already on disk.
      return true;
    }
  }

  // Failed to stat. We need to create the directory.
  if (errno != ENOENT) {
    *error_msg = "Could not stat isa dir " + dir + ":" + strerror(errno);
    return false;
  }

  mode_t mode = S_IRWXU | S_IXGRP | S_IXOTH;
  if (mkdir(dir.c_str(), mode) != 0) {
    *error_msg = "Could not create dir " + dir + ":" + strerror(errno);
    return false;
  }
  if (chmod(dir.c_str(), mode) != 0) {
    *error_msg = "Could not create the oat dir " + dir + ":" + strerror(errno);
    return false;
  }
  return true;
}

// Prepares the odex directory for the given dex location.
static bool PrepareOdexDirectories(const std::string& dex_location,
                                   const std::string& expected_odex_location,
                                   InstructionSet isa,
                                   std::string* error_msg) {
  std::string actual_odex_location;
  std::string oat_dir;
  std::string isa_dir;
  if (!DexLocationToOdexNames(
        dex_location, isa, &actual_odex_location, &oat_dir, &isa_dir, error_msg)) {
    return false;
  }
  DCHECK_EQ(expected_odex_location, actual_odex_location);

  if (!PrepareDirectory(oat_dir, error_msg)) {
    return false;
  }
  if (!PrepareDirectory(isa_dir, error_msg)) {
    return false;
  }
  return true;
}

OatFileAssistant::ResultOfAttemptToUpdate OatFileAssistant::GenerateOatFileNoChecks(
      OatFileAssistant::OatFileInfo& info, CompilerFilter::Filter filter, std::string* error_msg) {
  CHECK(error_msg != nullptr);

  Runtime* runtime = Runtime::Current();
  if (!runtime->IsDex2OatEnabled()) {
    *error_msg = "Generation of oat file for dex location " + dex_location_
      + " not attempted because dex2oat is disabled.";
    return kUpdateNotAttempted;
  }

  if (info.Filename() == nullptr) {
    *error_msg = "Generation of oat file for dex location " + dex_location_
      + " not attempted because the oat file name could not be determined.";
    return kUpdateNotAttempted;
  }
  const std::string& oat_file_name = *info.Filename();
  const std::string& vdex_file_name = GetVdexFilename(oat_file_name);

  // dex2oat ignores missing dex files and doesn't report an error.
  // Check explicitly here so we can detect the error properly.
  // TODO: Why does dex2oat behave that way?
  struct stat dex_path_stat;
  if (TEMP_FAILURE_RETRY(stat(dex_location_.c_str(), &dex_path_stat)) != 0) {
    *error_msg = "Could not access dex location " + dex_location_ + ":" + strerror(errno);
    return kUpdateNotAttempted;
  }

  // If this is the odex location, we need to create the odex file layout (../oat/isa/..)
  if (!info.IsOatLocation()) {
    if (!PrepareOdexDirectories(dex_location_, oat_file_name, isa_, error_msg)) {
      return kUpdateNotAttempted;
    }
  }

  // Set the permissions for the oat and the vdex files.
  // The user always gets read and write while the group and others propagate
  // the reading access of the original dex file.
  mode_t file_mode = S_IRUSR | S_IWUSR |
      (dex_path_stat.st_mode & S_IRGRP) |
      (dex_path_stat.st_mode & S_IROTH);

  std::unique_ptr<File> vdex_file(OS::CreateEmptyFile(vdex_file_name.c_str()));
  if (vdex_file.get() == nullptr) {
    *error_msg = "Generation of oat file " + oat_file_name
      + " not attempted because the vdex file " + vdex_file_name
      + " could not be opened.";
    return kUpdateNotAttempted;
  }

  if (fchmod(vdex_file->Fd(), file_mode) != 0) {
    *error_msg = "Generation of oat file " + oat_file_name
      + " not attempted because the vdex file " + vdex_file_name
      + " could not be made world readable.";
    return kUpdateNotAttempted;
  }

  std::unique_ptr<File> oat_file(OS::CreateEmptyFile(oat_file_name.c_str()));
  if (oat_file.get() == nullptr) {
    *error_msg = "Generation of oat file " + oat_file_name
      + " not attempted because the oat file could not be created.";
    return kUpdateNotAttempted;
  }

  if (fchmod(oat_file->Fd(), file_mode) != 0) {
    *error_msg = "Generation of oat file " + oat_file_name
      + " not attempted because the oat file could not be made world readable.";
    oat_file->Erase();
    return kUpdateNotAttempted;
  }

  std::vector<std::string> args;
  args.push_back("--dex-file=" + dex_location_);
  args.push_back("--output-vdex-fd=" + std::to_string(vdex_file->Fd()));
  args.push_back("--oat-fd=" + std::to_string(oat_file->Fd()));
  args.push_back("--oat-location=" + oat_file_name);
  args.push_back("--compiler-filter=" + CompilerFilter::NameOfFilter(filter));

  if (!Dex2Oat(args, error_msg)) {
    // Manually delete the oat and vdex files. This ensures there is no garbage
    // left over if the process unexpectedly died.
    vdex_file->Erase();
    unlink(vdex_file_name.c_str());
    oat_file->Erase();
    unlink(oat_file_name.c_str());
    return kUpdateFailed;
  }

  if (vdex_file->FlushCloseOrErase() != 0) {
    *error_msg = "Unable to close vdex file " + vdex_file_name;
    unlink(vdex_file_name.c_str());
    return kUpdateFailed;
  }

  if (oat_file->FlushCloseOrErase() != 0) {
    *error_msg = "Unable to close oat file " + oat_file_name;
    unlink(oat_file_name.c_str());
    return kUpdateFailed;
  }

  // Mark that the odex file has changed and we should try to reload.
  info.Reset();
  return kUpdateSucceeded;
}

bool OatFileAssistant::Dex2Oat(const std::vector<std::string>& args,
                               std::string* error_msg) {
  Runtime* runtime = Runtime::Current();
  std::string image_location = ImageLocation();
  if (image_location.empty()) {
    *error_msg = "No image location found for Dex2Oat.";
    return false;
  }

  std::vector<std::string> argv;
  argv.push_back(runtime->GetCompilerExecutable());
  argv.push_back("--runtime-arg");
  argv.push_back("-classpath");
  argv.push_back("--runtime-arg");
  std::string class_path = runtime->GetClassPathString();
  if (class_path == "") {
    class_path = OatFile::kSpecialSharedLibrary;
  }
  argv.push_back(class_path);
  if (runtime->IsJavaDebuggable()) {
    argv.push_back("--debuggable");
  }
  runtime->AddCurrentRuntimeFeaturesAsDex2OatArguments(&argv);

  if (!runtime->IsVerificationEnabled()) {
    argv.push_back("--compiler-filter=verify-none");
  }

  if (runtime->MustRelocateIfPossible()) {
    argv.push_back("--runtime-arg");
    argv.push_back("-Xrelocate");
  } else {
    argv.push_back("--runtime-arg");
    argv.push_back("-Xnorelocate");
  }

  if (!kIsTargetBuild) {
    argv.push_back("--host");
  }

  argv.push_back("--boot-image=" + image_location);

  std::vector<std::string> compiler_options = runtime->GetCompilerOptions();
  argv.insert(argv.end(), compiler_options.begin(), compiler_options.end());

  argv.insert(argv.end(), args.begin(), args.end());

  std::string command_line(android::base::Join(argv, ' '));
  return Exec(argv, error_msg);
}

bool OatFileAssistant::DexLocationToOdexFilename(const std::string& location,
                                                 InstructionSet isa,
                                                 std::string* odex_filename,
                                                 std::string* error_msg) {
  return DexLocationToOdexNames(location, isa, odex_filename, nullptr, nullptr, error_msg);
}

bool OatFileAssistant::DexLocationToOatFilename(const std::string& location,
                                                InstructionSet isa,
                                                std::string* oat_filename,
                                                std::string* error_msg) {
  CHECK(oat_filename != nullptr);
  CHECK(error_msg != nullptr);

  std::string cache_dir = GetDalvikCache(GetInstructionSetString(isa));
  if (cache_dir.empty()) {
    *error_msg = "Dalvik cache directory does not exist";
    return false;
  }

  // TODO: The oat file assistant should be the definitive place for
  // determining the oat file name from the dex location, not
  // GetDalvikCacheFilename.
  return GetDalvikCacheFilename(location.c_str(), cache_dir.c_str(), oat_filename, error_msg);
}

std::string OatFileAssistant::ImageLocation() {
  Runtime* runtime = Runtime::Current();
  const std::vector<gc::space::ImageSpace*>& image_spaces =
      runtime->GetHeap()->GetBootImageSpaces();
  if (image_spaces.empty()) {
    return "";
  }
  return image_spaces[0]->GetImageLocation();
}

const std::vector<uint32_t>* OatFileAssistant::GetRequiredDexChecksums() {
  if (!required_dex_checksums_attempted_) {
    required_dex_checksums_attempted_ = true;
    required_dex_checksums_found_ = false;
    cached_required_dex_checksums_.clear();
    std::string error_msg;
    if (DexFile::GetMultiDexChecksums(dex_location_.c_str(),
                                      &cached_required_dex_checksums_,
                                      &error_msg)) {
      required_dex_checksums_found_ = true;
      has_original_dex_files_ = true;
    } else {
      // This can happen if the original dex file has been stripped from the
      // apk.
      VLOG(oat) << "OatFileAssistant: " << error_msg;
      has_original_dex_files_ = false;

      // Get the checksums from the odex if we can.
      const OatFile* odex_file = odex_.GetFile();
      if (odex_file != nullptr) {
        required_dex_checksums_found_ = true;
        for (size_t i = 0; i < odex_file->GetOatHeader().GetDexFileCount(); i++) {
          std::string dex = DexFile::GetMultiDexLocation(i, dex_location_.c_str());
          const OatFile::OatDexFile* odex_dex_file = odex_file->GetOatDexFile(dex.c_str(), nullptr);
          if (odex_dex_file == nullptr) {
            required_dex_checksums_found_ = false;
            break;
          }
          cached_required_dex_checksums_.push_back(odex_dex_file->GetDexFileLocationChecksum());
        }
      }
    }
  }
  return required_dex_checksums_found_ ? &cached_required_dex_checksums_ : nullptr;
}

std::unique_ptr<OatFileAssistant::ImageInfo>
OatFileAssistant::ImageInfo::GetRuntimeImageInfo(InstructionSet isa, std::string* error_msg) {
  CHECK(error_msg != nullptr);

  Runtime* runtime = Runtime::Current();
  std::unique_ptr<ImageInfo> info(new ImageInfo());
  info->location = runtime->GetImageLocation();

  std::unique_ptr<ImageHeader> image_header(
      gc::space::ImageSpace::ReadImageHeader(info->location.c_str(), isa, error_msg));
  if (image_header == nullptr) {
    return nullptr;
  }

  info->oat_checksum = image_header->GetOatChecksum();
  info->oat_data_begin = reinterpret_cast<uintptr_t>(image_header->GetOatDataBegin());
  info->patch_delta = image_header->GetPatchDelta();
  return info;
}

const OatFileAssistant::ImageInfo* OatFileAssistant::GetImageInfo() {
  if (!image_info_load_attempted_) {
    image_info_load_attempted_ = true;
    std::string error_msg;
    cached_image_info_ = ImageInfo::GetRuntimeImageInfo(isa_, &error_msg);
    if (cached_image_info_ == nullptr) {
      LOG(WARNING) << "Unable to get runtime image info: " << error_msg;
    }
  }
  return cached_image_info_.get();
}

OatFileAssistant::OatFileInfo& OatFileAssistant::GetBestInfo() {
  // TODO(calin): Document the side effects of class loading when
  // running dalvikvm command line.
  if (dex_parent_writable_) {
    // If the parent of the dex file is writable it means that we can
    // create the odex file. In this case we unconditionally pick the odex
    // as the best oat file. This corresponds to the regular use case when
    // apps gets installed or when they load private, secondary dex file.
    // For apps on the system partition the odex location will not be
    // writable and thus the oat location might be more up to date.
    return odex_;
  }

  // We cannot write to the odex location. This must be a system app.

  // If the oat location is usable take it.
  if (oat_.IsUseable()) {
    return oat_;
  }

  // The oat file is not usable but the odex file might be up to date.
  // This is an indication that we are dealing with an up to date prebuilt
  // (that doesn't need relocation).
  if (odex_.Status() == kOatUpToDate) {
    return odex_;
  }

  // The oat file is not usable and the odex file is not up to date.
  // However we have access to the original dex file which means we can make
  // the oat location up to date.
  if (HasOriginalDexFiles()) {
    return oat_;
  }

  // We got into the worst situation here:
  // - the oat location is not usable
  // - the prebuild odex location is not up to date
  // - and we don't have the original dex file anymore (stripped).
  // Pick the odex if it exists, or the oat if not.
  return (odex_.Status() == kOatCannotOpen) ? oat_ : odex_;
}

std::unique_ptr<gc::space::ImageSpace> OatFileAssistant::OpenImageSpace(const OatFile* oat_file) {
  DCHECK(oat_file != nullptr);
  std::string art_file = ReplaceFileExtension(oat_file->GetLocation(), "art");
  if (art_file.empty()) {
    return nullptr;
  }
  std::string error_msg;
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<gc::space::ImageSpace> ret =
      gc::space::ImageSpace::CreateFromAppImage(art_file.c_str(), oat_file, &error_msg);
  if (ret == nullptr && (VLOG_IS_ON(image) || OS::FileExists(art_file.c_str()))) {
    LOG(INFO) << "Failed to open app image " << art_file.c_str() << " " << error_msg;
  }
  return ret;
}

OatFileAssistant::OatFileInfo::OatFileInfo(OatFileAssistant* oat_file_assistant,
                                           bool is_oat_location)
  : oat_file_assistant_(oat_file_assistant), is_oat_location_(is_oat_location)
{}

bool OatFileAssistant::OatFileInfo::IsOatLocation() {
  return is_oat_location_;
}

const std::string* OatFileAssistant::OatFileInfo::Filename() {
  return filename_provided_ ? &filename_ : nullptr;
}

bool OatFileAssistant::OatFileInfo::IsUseable() {
  switch (Status()) {
    case kOatCannotOpen:
    case kOatDexOutOfDate:
    case kOatBootImageOutOfDate: return false;

    case kOatRelocationOutOfDate:
    case kOatUpToDate: return true;
  }
  UNREACHABLE();
}

OatFileAssistant::OatStatus OatFileAssistant::OatFileInfo::Status() {
  if (!status_attempted_) {
    status_attempted_ = true;
    const OatFile* file = GetFile();
    if (file == nullptr) {
      // Check to see if there is a vdex file we can make use of.
      std::string error_msg;
      std::string vdex_filename = GetVdexFilename(filename_);
      std::unique_ptr<VdexFile> vdex = VdexFile::Open(vdex_filename,
                                                      /*writeable*/false,
                                                      /*low_4gb*/false,
                                                      /*unquicken*/false,
                                                      &error_msg);
      if (vdex == nullptr) {
        status_ = kOatCannotOpen;
        VLOG(oat) << "unable to open vdex file " << vdex_filename << ": " << error_msg;
      } else {
        if (oat_file_assistant_->DexChecksumUpToDate(*vdex, &error_msg)) {
          // The vdex file does not contain enough information to determine
          // whether it is up to date with respect to the boot image, so we
          // assume it is out of date.
          VLOG(oat) << error_msg;
          status_ = kOatBootImageOutOfDate;
        } else {
          status_ = kOatDexOutOfDate;
        }
      }
    } else {
      status_ = oat_file_assistant_->GivenOatFileStatus(*file);
      VLOG(oat) << file->GetLocation() << " is " << status_
          << " with filter " << file->GetCompilerFilter();
    }
  }
  return status_;
}

OatFileAssistant::DexOptNeeded OatFileAssistant::OatFileInfo::GetDexOptNeeded(
    CompilerFilter::Filter target, bool profile_changed) {
  bool compilation_desired = CompilerFilter::IsAotCompilationEnabled(target);
  bool filter_okay = CompilerFilterIsOkay(target, profile_changed);

  if (filter_okay && Status() == kOatUpToDate) {
    // The oat file is in good shape as is.
    return kNoDexOptNeeded;
  }

  if (filter_okay && !compilation_desired && Status() == kOatRelocationOutOfDate) {
    // If no compilation is desired, then it doesn't matter if the oat
    // file needs relocation. It's in good shape as is.
    return kNoDexOptNeeded;
  }

  if (filter_okay && Status() == kOatRelocationOutOfDate) {
    return kDex2OatForRelocation;
  }

  if (IsUseable()) {
    return kDex2OatForFilter;
  }

  if (Status() == kOatBootImageOutOfDate) {
    return kDex2OatForBootImage;
  }

  if (oat_file_assistant_->HasOriginalDexFiles()) {
    return kDex2OatFromScratch;
  } else {
    // Otherwise there is nothing we can do, even if we want to.
    return kNoDexOptNeeded;
  }
}

const OatFile* OatFileAssistant::OatFileInfo::GetFile() {
  CHECK(!file_released_) << "GetFile called after oat file released.";
  if (!load_attempted_) {
    load_attempted_ = true;
    if (filename_provided_) {
      std::string error_msg;
      file_.reset(OatFile::Open(filename_.c_str(),
                                filename_.c_str(),
                                nullptr,
                                nullptr,
                                oat_file_assistant_->load_executable_,
                                /*low_4gb*/false,
                                oat_file_assistant_->dex_location_.c_str(),
                                &error_msg));
      if (file_.get() == nullptr) {
        VLOG(oat) << "OatFileAssistant test for existing oat file "
          << filename_ << ": " << error_msg;
      }
    }
  }
  return file_.get();
}

bool OatFileAssistant::OatFileInfo::CompilerFilterIsOkay(
    CompilerFilter::Filter target, bool profile_changed) {
  const OatFile* file = GetFile();
  if (file == nullptr) {
    return false;
  }

  CompilerFilter::Filter current = file->GetCompilerFilter();
  if (profile_changed && CompilerFilter::DependsOnProfile(current)) {
    VLOG(oat) << "Compiler filter not okay because Profile changed";
    return false;
  }
  return CompilerFilter::IsAsGoodAs(current, target);
}

bool OatFileAssistant::OatFileInfo::IsExecutable() {
  const OatFile* file = GetFile();
  return (file != nullptr && file->IsExecutable());
}

void OatFileAssistant::OatFileInfo::Reset() {
  load_attempted_ = false;
  file_.reset();
  status_attempted_ = false;
}

void OatFileAssistant::OatFileInfo::Reset(const std::string& filename) {
  filename_provided_ = true;
  filename_ = filename;
  Reset();
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFile() {
  file_released_ = true;
  return std::move(file_);
}

std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFileForUse() {
  if (Status() == kOatUpToDate) {
    return ReleaseFile();
  }

  VLOG(oat) << "Oat File Assistant: No relocated oat file found,"
    << " attempting to fall back to interpreting oat file instead.";

  if (Status() == kOatRelocationOutOfDate && !IsExecutable()) {
    return ReleaseFile();
  }

  if (Status() == kOatRelocationOutOfDate) {
    // We are loading an oat file for runtime use that needs relocation.
    // Reload the file non-executable to ensure that we interpret out of the
    // dex code in the oat file rather than trying to execute the unrelocated
    // compiled code.
    oat_file_assistant_->load_executable_ = false;
    Reset();
    if (IsUseable()) {
      CHECK(!IsExecutable());
      return ReleaseFile();
    }
  }
  return std::unique_ptr<OatFile>();
}
}  // namespace art
