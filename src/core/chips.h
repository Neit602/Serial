#pragma once

// USB VID:PID → chip family (PRD F1).
//
// Only well-known IDs are listed; an unknown adapter reports nothing rather
// than a guess, because a wrong chip name is worse than no chip name when the
// user is trying to work out which driver they need.

#include <QString>

#include <optional>

namespace portside {

// The chip family for vid/pid, or an empty string if unknown.  A known vendor
// with an unknown product still yields the vendor name plus the product ID,
// which is enough to search for a driver.
QString chipFamily(std::optional<quint16> vid, std::optional<quint16> pid);

// Vendor name for a USB vendor ID, or empty.
QString vendorName(quint16 vid);

} // namespace portside
