#pragma once

// The version this build says it is. "Check for updates" compares it with this same file on the main
// branch on GitHub, so a release is: bump this, push to main.
inline constexpr const char* ecps_version = "0.96";

// update test: this comment only exists to make a new commit, so "Check for updates" has something to find
// update test 2: another comment-only change, to get a newer commit than the one the installed build was made from
