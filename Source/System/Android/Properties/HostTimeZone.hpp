// HostTimeZone.hpp — the host's IANA time zone, for persist.sys.timezone.
#pragma once
#include <string>

namespace arm64emu::yggdrasil {

// "America/New_York" from a TZ value or a path into a zoneinfo tree; "" if it
// is not a zone name (a POSIX rule such as "EST5EDT,M3.2.0,M11.1.0").
std::string timezone_name(std::string value);

// The zone the host uses, or "". tz is the host's TZ variable (null if unset);
// root prefixes the files read, and is empty outside tests.
std::string host_timezone(const char* tz, const std::string& root = "");

}  // namespace arm64emu::yggdrasil
