// Makes a ready-to-use CMD HD image: a blank drive of the size asked for, set up the way its
// owner would set up a real one, by the drive's own programs, run in a private machine:
//   1. the install program from the user's utilities disk writes the system and the DOS
//      (the version the disk's system header is for);
//   2. a short BASIC program writes the partition table with the drive's own partition
//      commands (as the install program does): Native partitions of 16 MB, the largest the
//      DOS allows, filling the drive up to 1 GB (the DOS's 6-byte SCSI commands reach no
//      further on one disk; beyond it the image stays unused);
//   3. the drive is reset, its DOS boots from the disk, and each partition is formatted with
//      the DOS's NEW command.
// The utilities disk (a D81) is recognized by the content of the programs it needs; they are
// served to the machine through the folder drive under the names the disk gives them.
// Notes: local-notes/hw/cmdhd.md, PLAN Phase 11b.
#pragma once

#include "roms.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace k64 {

constexpr uint64_t kHardDriveMinBytes = 20ull << 20;   // the smallest CMD HD sold
constexpr uint64_t kHardDriveMaxBytes = 2ull << 30;    // our limit (USER 2026-11-24)

// Finds the utilities disk in DIR or DIR/kernal64: a D81 holding every program step 1 needs.
bool findUtilitiesDisk(const std::string& dir, std::vector<uint8_t>& d81, std::string& from);

// Progress: 0-100; return false to cancel. The image is removed on failure or cancel.
bool makeHardDrive(const std::string& path, uint64_t bytes, const RomSet& roms, const std::vector<uint8_t>& bootRom,
                   const std::vector<uint8_t>& d81, const std::function<bool(int)>& progress, std::string& error,
                   int* partitionsMade = nullptr);

} // namespace k64
