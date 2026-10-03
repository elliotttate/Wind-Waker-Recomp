// Minimal Dolphin Common services for the donor DSP on iOS.
//
// The DSP interpreter needs nine Common helpers. The desktop build takes them
// from RecompCore's libcommon, which drags in the log manager, config and file
// layers; on iOS these small equivalents keep the donor self-contained.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/PcapFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

namespace Common
{
std::string GetStringT(const char* string)
{
  return string;
}

bool MsgAlertFmtImpl(bool yes_no, MsgType, Log::LogType, const char* file, int line,
                     fmt::string_view format, const fmt::format_args& args)
{
  const std::string message = fmt::vformat(format, args);
  std::fprintf(stderr, "[dsp-alert] %s:%d %s\n", file, line, message.c_str());
  return yes_no;
}

namespace Log
{
void GenericLogFmtImpl(LogLevel level, LogType, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args)
{
  if (level > LogLevel::LWARNING)
    return;
  const std::string message = fmt::vformat(format, args);
  std::fprintf(stderr, "[dsp-log] %s:%d %s\n", file, line, message.c_str());
}
}  // namespace Log

u32 HashAdler32(const u8* data, size_t len)
{
  return static_cast<u32>(adler32_z(1, data, len));
}

void ToLower(std::string* str)
{
  std::transform(str->begin(), str->end(), str->begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

void* AllocateMemoryPages(size_t size)
{
  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  return ptr == MAP_FAILED ? nullptr : ptr;
}

bool FreeMemoryPages(void* ptr, size_t size)
{
  return ptr == nullptr || munmap(ptr, size) == 0;
}

// iOS forbids executable pages without JIT entitlements; the DSP interpreter
// never asks for them, so only the read/write protection is applied.
bool WriteProtectMemory(void* ptr, size_t size, bool)
{
  return mprotect(ptr, size, PROT_READ) == 0;
}

bool UnWriteProtectMemory(void* ptr, size_t size, bool)
{
  return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
}
}  // namespace Common

// --- Services the high-level DSP (DSPHLE) reaches; only ucode dumping and
// --- config lookups use most of them, and both are off on iOS.
#include <cstdio>
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

namespace Common
{
u32 HashEctor(const u8* data, size_t len)
{
  u32 crc = 0;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}
}  // namespace Common

bool TryParse(const std::string& str, bool* output)
{
  if (str == "1" || str == "true" || str == "True" || str == "TRUE")
    *output = true;
  else if (str == "0" || str == "false" || str == "False" || str == "FALSE")
    *output = false;
  else
    return false;
  return true;
}

namespace Config
{
u64 GetConfigVersion()
{
  return 0;
}

std::optional<std::string> GetAsString(const Location&)
{
  return std::nullopt;
}
}  // namespace Config

namespace File
{
const std::string& GetUserPath(unsigned int)
{
  static const std::string empty;
  return empty;
}

const std::string& GetSysDirectory()
{
  static const std::string empty;
  return empty;
}

bool ReadFileToString(const std::string&, std::string&)
{
  return false;
}

bool WriteStringToFile(const std::string&, std::string_view)
{
  return false;
}

u64 GetSize(const std::string&)
{
  return 0;
}

IOFile::IOFile(const std::string&, const char[], SharedAccess) : m_file(nullptr), m_good(false)
{
}

IOFile::~IOFile()
{
  if (m_file != nullptr)
    std::fclose(m_file);
}
}  // namespace File

// Dolphin's PCAP capture writer (Common/PCAP.cpp) is not part of the trimmed
// RecompCore tree, and the donor DSP never captures at runtime. Stub the two
// members the capture logger links against so the donor builds self-contained.
namespace Common
{
void PCAP::AddHeader(u32)
{
}

void PCAP::AddPacket(const u8*, size_t)
{
}
}  // namespace Common
